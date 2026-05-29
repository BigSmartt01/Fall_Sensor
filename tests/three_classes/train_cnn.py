import numpy as np
import tensorflow as tf
from tensorflow.keras.models import Sequential
from tensorflow.keras.layers import Conv1D, MaxPooling1D, BatchNormalization, Flatten, Dense, Dropout
from tensorflow.keras.callbacks import EarlyStopping, ReduceLROnPlateau
from sklearn.metrics import classification_report, confusion_matrix
from sklearn.utils.class_weight import compute_class_weight

base_path = r"C:\Users\INNOV8HUB230625\Documents\Projects\Fall_Sensor\tests\three_classes\\"

# ─── LOAD DATA ────────────────────────────────────────────────────────────────
print("Loading data...")
y_train = np.frombuffer(open(base_path + 'y_train_3', 'rb').read(), dtype=np.uint8)
y_val   = np.frombuffer(open(base_path + 'y_val_3',   'rb').read(), dtype=np.uint8)
y_test  = np.frombuffer(open(base_path + 'y_test_3',  'rb').read(), dtype=np.uint8)

X_train = np.frombuffer(open(base_path + 'x_train_3', 'rb').read(), dtype=np.float32)
X_val   = np.frombuffer(open(base_path + 'x_val_3',   'rb').read(), dtype=np.float32)
X_test  = np.frombuffer(open(base_path + 'x_test_3',  'rb').read(), dtype=np.float32)

# Reshape for CNN - (samples, timesteps, channels)
X_train = X_train.reshape(y_train.shape[0], 512, 1)
X_val   = X_val.reshape(y_val.shape[0],     512, 1)
X_test  = X_test.reshape(y_test.shape[0],   512, 1)

print(f"X_train: {X_train.shape}, X_val: {X_val.shape}, X_test: {X_test.shape}")

# ─── CLASS WEIGHTS ────────────────────────────────────────────────────────────
classes = np.unique(y_train)
weights = compute_class_weight('balanced', classes=classes, y=y_train)
class_weight = dict(zip(classes, weights))
print(f"Class weights: {class_weight}")

# ─── MODEL ────────────────────────────────────────────────────────────────────
model = Sequential([
    Conv1D(32, 5, activation='relu', input_shape=(512, 1)),
    BatchNormalization(),
    MaxPooling1D(2),

    Conv1D(64, 3, activation='relu'),
    BatchNormalization(),
    MaxPooling1D(2),

    Conv1D(128, 3, activation='relu'),
    BatchNormalization(),
    MaxPooling1D(2),

    Flatten(),
    Dense(64, activation='relu'),
    Dropout(0.3),
    Dense(1, activation='sigmoid')
])

model.compile(
    optimizer='adam',
    loss='binary_crossentropy',
    metrics=['accuracy', tf.keras.metrics.AUC(name='auc')]
)

model.summary()

# ─── CALLBACKS ────────────────────────────────────────────────────────────────
early_stop = EarlyStopping(
    monitor='val_auc',
    patience=5,
    restore_best_weights=True,
    mode='max'
)
reduce_lr = ReduceLROnPlateau(
    monitor='val_loss',
    patience=3,
    factor=0.5
)

# ─── TRAIN ────────────────────────────────────────────────────────────────────
print("\nTraining...")
history = model.fit(
    X_train, y_train,
    validation_data=(X_val, y_val),
    epochs=20,
    batch_size=512,
    class_weight=class_weight,
    callbacks=[early_stop, reduce_lr]
)

# ─── EVALUATE ─────────────────────────────────────────────────────────────────
print("\nEvaluating on test set...")
y_pred_prob = model.predict(X_test)
y_pred = (y_pred_prob > 0.5).astype(int)

print("\nClassification Report:")
print(classification_report(y_test, y_pred, digits=4))

print("Confusion Matrix:")
print(confusion_matrix(y_test, y_pred))

# ─── SAVE MODEL ───────────────────────────────────────────────────────────────
model.save(base_path + 'fall_cnn_model.keras')
print("\nModel saved.")

# ─── EXPORT TFLITE ────────────────────────────────────────────────────────────
print("Exporting TFLite...")
converter = tf.lite.TFLiteConverter.from_keras_model(model)
converter.optimizations = [tf.lite.Optimize.DEFAULT]
tflite_model = converter.convert()

with open(base_path + 'fall_cnn_model.tflite', 'wb') as f:
    f.write(tflite_model)

print(f"TFLite model size: {len(tflite_model)/1024:.1f} KB")
print("Done.")