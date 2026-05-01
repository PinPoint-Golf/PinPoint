#pragma once
#include <functional>

// Checks and, if needed, requests microphone access via AVFoundation.
// The callback is invoked with true if access is granted, false if denied.
// It may be called from any thread — dispatch to the main thread if needed.
void requestMicrophonePermission(std::function<void(bool granted)> callback);

// Checks and, if needed, requests speech recognition access via the Speech framework.
// The callback is invoked on the main thread with true if authorized, false otherwise.
// Call this before starting the STT pipeline so loadModel() finds a determined status.
void requestSpeechRecognitionPermission(std::function<void(bool granted)> callback);

// Checks and, if needed, requests camera access via AVFoundation.
// The callback is invoked with true if access is granted, false if denied.
// It may be called from any thread — dispatch to the main thread if needed.
void requestCameraPermission(std::function<void(bool granted)> callback);
