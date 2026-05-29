import numpy as np
import tensorflow as tf

base_path = r"C:\Users\INNOV8HUB230625\Documents\Projects\Fall_Sensor\tests\three_classes\\"

# ─── LOAD REPRESENTATIVE DATASET ─────────────────────────────────────────────
# Quantization needs sample data to calibrate int8 ranges
print("Loading calibration data...")
y_train = np.frombuffer(open(base_path + 'y_train_3', 'rb').read(), dtype=np.uint8)
X_train = np.frombuffer(open(base_path + 'x_train_3', 'rb').read(), dtype=np.float32)
X_train = X_train.reshape(y_train.shape[0], 512, 1)

# Use 500 random samples for calibration - enough for accurate range estimation
calibration_indices = np.random.choice(len(X_train), 500, replace=False)
calibration_data    = X_train[calibration_indices]

def representative_dataset():
    for sample in calibration_data:
        yield [sample.reshape(1, 512, 1).astype(np.float32)]

# ─── QUANTIZE ─────────────────────────────────────────────────────────────────
print("Quantizing to int8...")
converter = tf.lite.TFLiteConverter.from_keras_model(
    tf.keras.models.load_model(base_path + 'fall_cnn_model.keras')
)

converter.optimizations                     = [tf.lite.Optimize.DEFAULT]
converter.representative_dataset            = representative_dataset
converter.target_spec.supported_ops         = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
converter.inference_input_type              = tf.int8
converter.inference_output_type             = tf.int8

tflite_int8 = converter.convert()

with open(base_path + 'fall_cnn_model_int8.tflite', 'wb') as f:
    f.write(tflite_int8)

print(f"int8 model size: {len(tflite_int8)/1024:.1f} KB")
print(f"float32 model size: 545.2 KB")
print(f"Size reduction: {(1 - len(tflite_int8)/(545.2*1024))*100:.1f}%")

# ─── VERIFY ACCURACY AFTER QUANTIZATION ───────────────────────────────────────
print("\nVerifying quantized model accuracy...")
y_test = np.frombuffer(open(base_path + 'y_test_3', 'rb').read(), dtype=np.uint8)
X_test = np.frombuffer(open(base_path + 'x_test_3', 'rb').read(), dtype=np.float32)
X_test = X_test.reshape(y_test.shape[0], 512, 1)

interpreter = tf.lite.Interpreter(model_path=base_path + 'fall_cnn_model_int8.tflite')
interpreter.allocate_tensors()

input_details  = interpreter.get_input_details()
output_details = interpreter.get_output_details()

# int8 models use scale and zero_point for input
input_scale, input_zero_point = input_details[0]['quantization']
print(f"Input scale: {input_scale}, zero_point: {input_zero_point}")

correct = 0
total   = len(y_test)

for i in range(total):
    sample = X_test[i].reshape(1, 512, 1)
    # Quantize input from float32 to int8
    sample_int8 = (sample / input_scale + input_zero_point).astype(np.int8)

    interpreter.set_tensor(input_details[0]['index'], sample_int8)
    interpreter.invoke()

    output    = interpreter.get_tensor(output_details[0]['index'])
    predicted = 1 if output[0][0] > 0 else 0
    correct  += (predicted == y_test[i])

    if i % 5000 == 0:
        print(f"  {i}/{total} evaluated...")

accuracy = correct / total * 100
print(f"\nint8 model accuracy: {accuracy:.2f}%")
print(f"float32 model accuracy: 97.77%")
print(f"Accuracy drop: {97.77 - accuracy:.2f}%")
print("\nDone. int8 model saved as fall_cnn_model_int8.tflite")