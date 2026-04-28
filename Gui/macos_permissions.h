#pragma once
#include <functional>

// Checks and, if needed, requests microphone access via AVFoundation.
// The callback is invoked with true if access is granted, false if denied.
// It may be called from any thread — dispatch to the main thread if needed.
void requestMicrophonePermission(std::function<void(bool granted)> callback);
