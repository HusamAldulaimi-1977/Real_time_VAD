import torch
import librosa
import numpy as np

# Load model
model = torch.jit.load("orvae_mfcc.ptl")
model.eval()

wav = "vacuum noise_48.wav"

# Load audio and resample to 16k
y, sr = librosa.load(wav, sr=16000)

# Extract 80 MFCCs
mfcc = librosa.feature.mfcc(
    y=y,
    sr=16000,
    n_mfcc=80,
    n_fft=512,
    hop_length=160,
    win_length=400
)

# Make shape = (80,64)
mfcc = mfcc[:, :64]

if mfcc.shape[1] < 64:
    pad = np.zeros((80, 64 - mfcc.shape[1]))
    mfcc = np.hstack((mfcc, pad))

MIN_VAL = -17559.234375
MAX_VAL = 4361.3876953125

mfcc = (mfcc - MIN_VAL) / (MAX_VAL - MIN_VAL)

x = torch.tensor(mfcc, dtype=torch.float32).unsqueeze(0).unsqueeze(0)
with torch.no_grad():
    out = model(x)

out = out[0].squeeze(0).squeeze(0).numpy()
mse = np.mean((mfcc - out) ** 2)

print("MSE =", mse)