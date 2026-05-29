import tensorflow as tf

interpreter = tf.lite.Interpreter(model_path='fall_cnn_model.tflite')
interpreter.allocate_tensors()

tensors = interpreter.get_tensor_details()
total_bytes = sum(
    t['shape'].prod() * 4 
    for t in tensors 
    if len(t['shape']) > 0
)
print(f"Estimated inference RAM: {total_bytes/1024:.1f} KB")