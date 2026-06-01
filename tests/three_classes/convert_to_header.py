# Run this in your venv_compat to convert the TFLite model to a C header file
# python tests/three_classes/convert_to_header.py

import os

model_path  = r"C:\Users\INNOV8HUB230625\Documents\Projects\Fall_Sensor\tests\three_classes\fall_cnn_qat_int8.tflite"
output_path = r"C:\Users\INNOV8HUB230625\Documents\Projects\Fall_Sensor\firmware\include\fall_model.h"

with open(model_path, 'rb') as f:
    model_data = f.read()

os.makedirs(os.path.dirname(output_path), exist_ok=True)

with open(output_path, 'w') as f:
    f.write("#pragma once\n\n")
    f.write("// Auto-generated from fall_cnn_qat_int8.tflite\n")
    f.write("// Lightweight CNN - QAT int8\n")
    f.write("// Flash: 16.9 KB | Inference RAM: 47.1 KB\n")
    f.write("// Accuracy: 94.78% | Fall recall: 97.63%\n\n")
    f.write("#include <stdint.h>\n\n")
    f.write(f"const unsigned int fall_model_len = {len(model_data)};\n\n")
    f.write("alignas(8) const uint8_t fall_model_data[] = {\n  ")

    for i, byte in enumerate(model_data):
        f.write(f"0x{byte:02x}")
        if i < len(model_data) - 1:
            f.write(", ")
            if (i + 1) % 12 == 0:
                f.write("\n  ")

    f.write("\n};\n")

print(f"Header written: {output_path}")
print(f"Model size: {len(model_data)} bytes ({len(model_data)/1024:.1f} KB)")
