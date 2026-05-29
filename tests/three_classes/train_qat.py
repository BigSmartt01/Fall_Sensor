import numpy as np
import tensorflow as tf
from tensorflow import keras
from sklearn.metrics import classification_report, confusion_matrix
from sklearn.utils.class_weight import compute_class_weight

# Quantization aware training requires this
import tensorflow_model_optimization as tfmot

class Conv1DQuantizeConfig(tfmot.quantization.keras.QuantizeConfig):
    def get_weights_and_quantizers(self, layer):
        return [(layer.kernel,
                 tfmot.quantization.keras.quantizers.LastValueQuantizer(
                     num_bits=8, symmetric=True, narrow_range=False, per_axis=False))]
    def get_activations_and_quantizers(self, layer):
        return [(layer.activation,
                 tfmot.quantization.keras.quantizers.MovingAverageQuantizer(
                     num_bits=8, symmetric=True, narrow_range=False, per_axis=False))]
    def set_quantize_weights(self, layer, quantize_weights):
        layer.kernel = quantize_weights[0]
    def set_quantize_activations(self, layer, quantize_activations):
        layer.activation = quantize_activations[0]
    def get_output_quantizers(self, layer):
        return []
    def get_config(self):
        return {}


base_path = r"C:\Users\INNOV8HUB230625\Documents\Projects\Fall_Sensor\tests\three_classes\\"

# ─── LOAD DATA ────────────────────────────────────────────────────────────────
print("Loading data...")
y_train = np.frombuffer(open(base_path + 'y_train_3', 'rb').read(), dtype=np.uint8)
y_val   = np.frombuffer(open(base_path + 'y_val_3',   'rb').read(), dtype=np.uint8)
y_test  = np.frombuffer(open(base_path + 'y_test_3',  'rb').read(), dtype=np.uint8)

X_train = np.frombuffer(open(base_path + 'x_train_3', 'rb').read(), dtype=np.float32)
X_val   = np.frombuffer(open(base_path + 'x_val_3',   'rb').read(), dtype=np.float32)
X_test  = np.frombuffer(open(base_path + 'x_test_3',  'rb').read(), dtype=np.float32)

X_train = X_train.reshape(y_train.shape[0], 512, 1)
X_val   = X_val.reshape(y_val.shape[0],     512, 1)
X_test  = X_test.reshape(y_test.shape[0],   512, 1)

print(f"X_train: {X_train.shape}")

# ─── CLASS WEIGHTS ────────────────────────────────────────────────────────────
classes     = np.unique(y_train)
weights     = compute_class_weight('balanced', classes=classes, y=y_train)
class_weight = dict(zip(classes, weights))
print(f"Class weights: {class_weight}")

# ─── LIGHTWEIGHT BASE MODEL ───────────────────────────────────────────────────
# Designed specifically for ESP32-C3 RAM constraint
# Target: <150KB inference RAM, <200KB flash
def build_base_model():
    inputs = keras.Input(shape=(512, 1))

    # Block 1
    x = keras.layers.Conv1D(8, 5, activation='relu', padding='same')(inputs)
    x = keras.layers.BatchNormalization()(x)
    x = keras.layers.MaxPooling1D(4)(x)

    # Block 2
    x = keras.layers.Conv1D(16, 3, activation='relu', padding='same')(x)
    x = keras.layers.BatchNormalization()(x)
    x = keras.layers.MaxPooling1D(4)(x)

    # Block 3
    x = keras.layers.Conv1D(32, 3, activation='relu', padding='same')(x)
    x = keras.layers.BatchNormalization()(x)
    x = keras.layers.MaxPooling1D(4)(x)

    # Classifier head
    x = keras.layers.Flatten()(x)
    x = keras.layers.Dense(16, activation='relu')(x)
    x = keras.layers.Dropout(0.3)(x)
    outputs = keras.layers.Dense(1, activation='sigmoid')(x)

    return keras.Model(inputs, outputs)

base_model = build_base_model()
base_model.summary()

# ─── STEP 1: TRAIN BASE MODEL FIRST ──────────────────────────────────────────
print("\nStep 1: Training base model...")
base_model.compile(
    optimizer=keras.optimizers.Adam(0.001),
    loss='binary_crossentropy',
    metrics=['accuracy', keras.metrics.AUC(name='auc')]
)

early_stop = keras.callbacks.EarlyStopping(
    monitor='val_auc', patience=5,
    restore_best_weights=True, mode='max'
)
reduce_lr = keras.callbacks.ReduceLROnPlateau(
    monitor='val_loss', patience=3, factor=0.5
)

history = base_model.fit(
    X_train, y_train,
    validation_data=(X_val, y_val),
    epochs=20,
    batch_size=512,
    class_weight=class_weight,
    callbacks=[early_stop, reduce_lr]
)

# Evaluate base model
y_pred = (base_model.predict(X_test) > 0.5).astype(int)
print("\nBase Model Results:")
print(classification_report(y_test, y_pred, digits=4))

# ─── STEP 2: APPLY QAT ────────────────────────────────────────────────────────
print("\nStep 2: Applying Quantization Aware Training...")

# Annotate Conv1D layers with custom config
annotated_model = tf.keras.models.clone_model(
    base_model,
    clone_function=lambda layer: (
        tfmot.quantization.keras.quantize_annotate_layer(layer, Conv1DQuantizeConfig())
        if isinstance(layer, tf.keras.layers.Conv1D) else layer
    )
)

# Apply quantization inside a custom object scope
with tfmot.quantization.keras.quantize_scope({'Conv1DQuantizeConfig': Conv1DQuantizeConfig}):
    qat_model = tfmot.quantization.keras.quantize_apply(annotated_model)


qat_model.compile(
    optimizer=keras.optimizers.Adam(0.0001),  # lower LR for fine-tuning
    loss='binary_crossentropy',
    metrics=['accuracy', keras.metrics.AUC(name='auc')]
)

qat_model.summary()

# Fine-tune with QAT for fewer epochs
history_qat = qat_model.fit(
    X_train, y_train,
    validation_data=(X_val, y_val),
    epochs=10,
    batch_size=512,
    class_weight=class_weight,
    callbacks=[
        keras.callbacks.EarlyStopping(
            monitor='val_auc', patience=3,
            restore_best_weights=True, mode='max'
        )
    ]
)

# ─── STEP 3: EVALUATE QAT MODEL ───────────────────────────────────────────────
print("\nEvaluating QAT model...")
y_pred_qat = (qat_model.predict(X_test) > 0.5).astype(int)
print("\nQAT Model Results:")
print(classification_report(y_test, y_pred_qat, digits=4))
print("Confusion Matrix:")
print(confusion_matrix(y_test, y_pred_qat))

# ─── STEP 4: CONVERT TO INT8 TFLITE ──────────────────────────────────────────
print("\nConverting to int8 TFLite...")

def representative_dataset():
    indices = np.random.choice(len(X_train), 500, replace=False)
    for i in indices:
        yield [X_train[i].reshape(1, 512, 1).astype(np.float32)]

converter = tf.lite.TFLiteConverter.from_keras_model(qat_model)
converter.optimizations                 = [tf.lite.Optimize.DEFAULT]
converter.representative_dataset        = representative_dataset
converter.target_spec.supported_ops     = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
converter.inference_input_type          = tf.int8
converter.inference_output_type         = tf.int8

tflite_int8 = converter.convert()

with open(base_path + 'fall_cnn_qat_int8.tflite', 'wb') as f:
    f.write(tflite_int8)

print(f"int8 model size: {len(tflite_int8)/1024:.1f} KB")

# ─── STEP 5: VERIFY INT8 ACCURACY ────────────────────────────────────────────
print("\nVerifying int8 model accuracy...")
interpreter = tf.lite.Interpreter(
    model_path=base_path + 'fall_cnn_qat_int8.tflite'
)
interpreter.allocate_tensors()

input_details  = interpreter.get_input_details()
output_details = interpreter.get_output_details()

input_scale      = input_details[0]['quantization'][0]
input_zero_point = input_details[0]['quantization'][1]

print(f"Input scale: {input_scale}, zero_point: {input_zero_point}")

correct = 0
for i in range(len(X_test)):
    sample      = X_test[i].reshape(1, 512, 1)
    sample_int8 = (sample / input_scale + input_zero_point).astype(np.int8)
    interpreter.set_tensor(input_details[0]['index'], sample_int8)
    interpreter.invoke()
    output    = interpreter.get_tensor(output_details[0]['index'])
    predicted = 1 if output[0][0] > 0 else 0
    correct  += (predicted == y_test[i])
    if i % 5000 == 0:
        print(f"  {i}/{len(X_test)} verified...")

print(f"\nint8 accuracy: {correct/len(X_test)*100:.2f}%")

# ─── STEP 6: CHECK RAM USAGE ──────────────────────────────────────────────────
tensors     = interpreter.get_tensor_details()
total_bytes = sum(
    t['shape'].prod() * 1
    for t in tensors
    if len(t['shape']) > 0
)
print(f"Estimated inference RAM: {total_bytes/1024:.1f} KB")
print(f"Model flash size: {len(tflite_int8)/1024:.1f} KB")
print("\nDone. Saved as fall_cnn_qat_int8.tflite")