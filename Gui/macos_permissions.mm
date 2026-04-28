#import <AVFoundation/AVFoundation.h>
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
