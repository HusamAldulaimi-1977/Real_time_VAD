import torch

model = torch.jit.load("orvae_mfcc.ptl")
model.eval()

print("Model loaded successfully")
print(model)