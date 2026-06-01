#!/usr/bin/env python3
"""
Extract quantization parameters from a TFLite model and generate C++ code.

Usage:
    python extract_quantization.py <model.tflite> [--output inference_params.h]
"""

import tensorflow as tf
import struct
import sys
import json
from pathlib import Path


def extract_quantization_params(model_path):
    """Extract quantization parameters from a TFLite model."""
    
    print(f"\n{'='*60}")
    print(f"TFLite Quantization Parameter Extractor")
    print(f"{'='*60}")
    print(f"Model: {model_path}\n")
    
    # Load the model
    try:
        interpreter = tf.lite.Interpreter(model_path=model_path)
        interpreter.allocate_tensors()
    except Exception as e:
        print(f"Error loading model: {e}")
        return None
    
    # Get input and output details
    input_details = interpreter.get_input_details()
    output_details = interpreter.get_output_details()
    
    params = {}
    
    # Extract input parameters
    print("INPUT TENSORS:")
    print("-" * 60)
    input_scales = []
    input_zero_points = []
    
    for idx, inp in enumerate(input_details):
        name = inp.get('name', f'input_{idx}')
        dtype = inp.get('dtype', 'unknown')
        shape = inp.get('shape', [])
        quantization = inp.get('quantization', (None, None))
        
        scale, zero_point = quantization
        
        print(f"\nInput {idx}: {name}")
        print(f"  Data type: {dtype}")
        print(f"  Shape: {shape}")
        print(f"  Total elements: {int(np.prod(shape)) if shape else 'unknown'}")
        
        if scale is not None:
            print(f"  Quantization Scale: {scale}")
            print(f"  Quantization Zero Point: {zero_point}")
            input_scales.append(float(scale) if scale else 1.0)
            input_zero_points.append(int(zero_point) if zero_point else 0)
        else:
            print(f"  Quantization: None (Float32)")
            input_scales.append(1.0)
            input_zero_points.append(0)
    
    # Extract output parameters
    print("\n" + "=" * 60)
    print("OUTPUT TENSORS:")
    print("-" * 60)
    output_scales = []
    output_zero_points = []
    
    for idx, out in enumerate(output_details):
        name = out.get('name', f'output_{idx}')
        dtype = out.get('dtype', 'unknown')
        shape = out.get('shape', [])
        quantization = out.get('quantization', (None, None))
        
        scale, zero_point = quantization
        
        print(f"\nOutput {idx}: {name}")
        print(f"  Data type: {dtype}")
        print(f"  Shape: {shape}")
        print(f"  Total elements: {int(np.prod(shape)) if shape else 'unknown'}")
        
        if scale is not None:
            print(f"  Quantization Scale: {scale}")
            print(f"  Quantization Zero Point: {zero_point}")
            output_scales.append(float(scale) if scale else 1.0)
            output_zero_points.append(int(zero_point) if zero_point else 0)
        else:
            print(f"  Quantization: None (Float32)")
            output_scales.append(1.0)
            output_zero_points.append(0)
    
    # Store parameters
    params = {
        'model_path': str(model_path),
        'input_scales': input_scales,
        'input_zero_points': input_zero_points,
        'output_scales': output_scales,
        'output_zero_points': output_zero_points,
        'input_details': input_details,
        'output_details': output_details,
    }
    
    return params


def generate_cpp_code(params):
    """Generate C++ code with the extracted parameters."""
    
    input_scales = params['input_scales']
    input_zero_points = params['input_zero_points']
    output_scales = params['output_scales']
    output_zero_points = params['output_zero_points']
    
    # Use first scale/zero_point if multiple
    input_scale = input_scales[0] if input_scales else 0.01
    input_zero_point = input_zero_points[0] if input_zero_points else 0
    output_scale = output_scales[0] if output_scales else 0.00390625
    output_zero_point = output_zero_points[0] if output_zero_points else -128
    
    cpp_code = f'''// Auto-generated quantization parameters
// Generated from TFLite model quantization inspection

#ifndef INFERENCE_QUANTIZATION_H
#define INFERENCE_QUANTIZATION_H

namespace {{
  // INPUT QUANTIZATION PARAMETERS
  // These convert float sensor input to int8 for the model
  // Formula: int8_value = round(float_value / scale) + zero_point
  const float input_scale = {input_scale}f;        // Quantization scale
  const int32_t input_zero_point = {input_zero_point};   // Zero point offset
  
  // OUTPUT QUANTIZATION PARAMETERS  
  // These convert int8 model output back to probabilities
  // Formula: float_value = (int8_value - zero_point) * scale
  const float output_scale = {output_scale}f;    // Dequantization scale
  const int32_t output_zero_point = {output_zero_point};  // Zero point offset
}}

#endif // INFERENCE_QUANTIZATION_H
'''
    
    return cpp_code


def generate_calibration_summary(params):
    """Generate a human-readable calibration summary."""
    
    input_details = params['input_details']
    output_details = params['output_details']
    
    summary = f"""
{'='*60}
QUANTIZATION CALIBRATION SUMMARY
{'='*60}

INPUT CONFIGURATION:
  Number of inputs: {len(input_details)}
  First input scale: {params['input_scales'][0]:.10f}
  First input zero point: {params['input_zero_points'][0]}
  
  Usage in inference.cpp:
    // Update these in the namespace at the top of inference.cpp
    float input_scale = {params['input_scales'][0]};
    int32_t input_zero_point = {params['input_zero_points'][0]};

OUTPUT CONFIGURATION:
  Number of outputs: {len(output_details)}
  First output scale: {params['output_scales'][0]:.10f}
  First output zero point: {params['output_zero_points'][0]}
  
  Usage in inference.cpp:
    // Update these in the namespace at the top of inference.cpp
    float output_scale = {params['output_scales'][0]};
    int32_t output_zero_point = {params['output_zero_points'][0]};

CALIBRATION CHECKLIST:
  ✓ Update inference.cpp with above values
  ✓ Rebuild: platformio run -e esp32-c3 --target upload
  ✓ Monitor: platformio device monitor --baud 115200
  ✓ Validate: Check serial output for clipped int8 values
  ✓ Test: Run fall detection and verify accuracy

TROUBLESHOOTING:
  • If input values are clipped to ±127: Increase input_scale
  • If output probabilities are inverted: Flip output_zero_point sign
  • If noisy predictions: Increase input_scale (less resolution)
  • If flat predictions: Decrease input_scale (more resolution)

For more details, see docs/QUANTIZATION_CALIBRATION.md
{'='*60}
"""
    
    return summary


def main():
    if len(sys.argv) < 2:
        print("Usage: python extract_quantization.py <model.tflite> [--output file.h]")
        sys.exit(1)
    
    model_path = sys.argv[1]
    output_file = "inference_quantization.h"
    
    # Parse arguments
    if "--output" in sys.argv:
        idx = sys.argv.index("--output")
        if idx + 1 < len(sys.argv):
            output_file = sys.argv[idx + 1]
    
    # Check if model exists
    if not Path(model_path).exists():
        print(f"Error: Model file not found: {model_path}")
        sys.exit(1)
    
    # Extract parameters
    params = extract_quantization_params(model_path)
    if not params:
        sys.exit(1)
    
    # Generate C++ code
    cpp_code = generate_cpp_code(params)
    
    # Generate summary
    summary = generate_calibration_summary(params)
    
    # Save C++ header file
    with open(output_file, 'w') as f:
        f.write(cpp_code)
    print(f"\n✓ Generated C++ header: {output_file}")
    print(f"  → Copy contents to inference.cpp namespace")
    
    # Print summary
    print(summary)
    
    # Save detailed JSON report
    json_file = "quantization_report.json"
    report = {
        'model': str(model_path),
        'input_scales': params['input_scales'],
        'input_zero_points': params['input_zero_points'],
        'output_scales': params['output_scales'],
        'output_zero_points': params['output_zero_points'],
    }
    
    with open(json_file, 'w') as f:
        json.dump(report, f, indent=2)
    print(f"✓ Generated JSON report: {json_file}")
    
    return 0


if __name__ == '__main__':
    import numpy as np
    sys.exit(main())
