#pragma once

class AudioPipeline;

bool SuperpoweredIO_Start(AudioPipeline* pipe, int sampleRate, int bufferSize);
void SuperpoweredIO_Stop();
float SuperpoweredIO_GetAvgCallbackMs();
bool SuperpoweredIO_StartFileMode(AudioPipeline* pipe, const char* path, int bufferSize, int outSampleRate);
void SuperpoweredIO_SetFileMonitor(bool enabled);
bool SuperpoweredIO_IsFilePlaying();
float SuperpoweredIO_GetMicRms();
