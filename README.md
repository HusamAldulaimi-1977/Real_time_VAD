# Real-time Voice Activity Detection (VAD)

An Android application that detects human voice activity in real time using DSP techniques.

## Features
- Real-time audio capture and processing
- Voice activity detection using signal processing
- Visual feedback of detection results

## Requirements
- Android Studio
- Android SDK (API level XX+)
- Java / Kotlin

## Getting Started
1. Clone the repo
   git clone https://github.com/HusamAldulaimi-1977/Real_time_VAD.git
2. Open in Android Studio
3. Build and run on a device or emulator

## How It Works
The app captures microphone input and applies DSP algorithms to distinguish 
voice from background noise in real time.


## Cross-project evaluation

The companion [MATLAB evaluation workflow](https://github.com/HusamAldulaimi-1977/Voice-Activity-Detection-VAD-for-Embedded-Systems-ML-DSP-#cross-project-evaluation) connects VAD decision CSVs to manual speech/noise labels and frame-level confusion matrices. It describes tests varying SNR, MSE percentile thresholds, and majority-voting window sizes. Exact tested settings, TP/TN/FP/FN counts, accuracy, precision, recall, and F1 are **not yet reported**; proposed follow-up experiments are listed separately. The evaluated Android revision/export version is also not yet reported.

## Author
Husam Aldulaimi
