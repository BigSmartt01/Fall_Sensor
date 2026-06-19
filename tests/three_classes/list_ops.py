import tensorflow as tf

interpreter = tf.lite.Interpreter(model_path=r"C:\Users\user\Documents\Projects\Fall_Sensor\tests\three_classes\fall_cnn_qat_int8.tflite")
interpreter.allocate_tensors()

# Get all unique ops used
import flatbuffers
from tensorflow.lite.python import schema_py_generated as schema_fb

with open(r"C:\Users\user\Documents\Projects\Fall_Sensor\tests\three_classes\fall_cnn_qat_int8.tflite", "rb") as f:
    buf = f.read()

model = schema_fb.Model.GetRootAsModel(buf, 0)
op_codes = set()
for i in range(model.OperatorCodesLength()):
    op_code = model.OperatorCodes(i)
    builtin_code = op_code.BuiltinCode()
    op_name = schema_fb.BuiltinOperator.__dict__
    for name, val in op_name.items():
        if val == builtin_code:
            op_codes.add(name)

print("Ops used in model:")
for op in sorted(op_codes):
    print(" -", op)