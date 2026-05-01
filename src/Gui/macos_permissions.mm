#import <AVFoundation/AVFoundation.h>
#import <Speech/Speech.h>
#include "macos_permissions.h"

void requestMicrophonePermission(std::function<void(bool granted)> callback)
{
    AVAuthorizationStatus status =
        [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];

    if (status == AVAuthorizationStatusAuthorized) {
        callback(true);
    } else if (status == AVAuthorizationStatusNotDetermined) {
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                               completionHandler:^(BOOL granted) {
            callback(granted);
        }];
    } else {
        // AVAuthorizationStatusDenied or AVAuthorizationStatusRestricted
        callback(false);
    }
}

void requestSpeechRecognitionPermission(std::function<void(bool granted)> callback)
{
    SFSpeechRecognizerAuthorizationStatus status = [SFSpeechRecognizer authorizationStatus];

    if (status == SFSpeechRecognizerAuthorizationStatusAuthorized) {
        callback(true);
    } else if (status == SFSpeechRecognizerAuthorizationStatusNotDetermined) {
        // The completion block always fires on the main thread.
        [SFSpeechRecognizer requestAuthorization:^(SFSpeechRecognizerAuthorizationStatus s) {
            callback(s == SFSpeechRecognizerAuthorizationStatusAuthorized);
        }];
    } else {
        // Denied or restricted — callback immediately so callers can act.
        callback(false);
    }
}
