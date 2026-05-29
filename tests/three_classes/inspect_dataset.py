import numpy as np

base_path = r"C:\Users\INNOV8HUB230625\Documents\Projects\Fall_Sensor\tests\three_classes\\"

# Load y files
y_train = np.frombuffer(open(base_path + 'y_train_3', 'rb').read(), dtype=np.uint8)
y_val   = np.frombuffer(open(base_path + 'y_val_3',   'rb').read(), dtype=np.uint8)
y_test  = np.frombuffer(open(base_path + 'y_test_3',  'rb').read(), dtype=np.uint8)

# Load X files
X_train = np.frombuffer(open(base_path + 'x_train_3', 'rb').read(), dtype=np.float32)
X_val   = np.frombuffer(open(base_path + 'x_val_3',   'rb').read(), dtype=np.float32)
X_test  = np.frombuffer(open(base_path + 'x_test_3',  'rb').read(), dtype=np.float32)

# Reshape X files
X_train = X_train.reshape(y_train.shape[0], 512)
X_val   = X_val.reshape(y_val.shape[0],     512)
X_test  = X_test.reshape(y_test.shape[0],   512)

print("X_train shape:", X_train.shape)
print("X_val shape:",   X_val.shape)
print("X_test shape:",  X_test.shape)
print("y_train shape:", y_train.shape)
print("y_val shape:",   y_val.shape)
print("y_test shape:",  y_test.shape)

print("\nClass distribution:")
unique, counts = np.unique(y_train, return_counts=True)
for u, c in zip(unique, counts):
    print(f"  Class {u}: {c} samples ({c/len(y_train)*100:.1f}%)")