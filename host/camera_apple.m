/* The host's camera (AVFoundation) into the virtual Mac's USB camera. See camera_apple.h.

   The USB camera tells us on the CPU thread when macOS starts and stops streaming; the capture session is
   started and stopped on a serial queue of its own (starting one blocks), and its frames (NV12, video range,
   which is what the USB camera takes without converting the colours) go straight to hv_usb_video_frame, which
   crops and scales them to what macOS asked for. On the iPad the picture is turned to stay upright as the iPad
   turns (iOS 17's rotation coordinator), and is not mirrored: a video call mirrors its own preview. */
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#include <TargetConditionals.h>
#include <stdatomic.h>
#include <stdio.h>

#include "camera_apple.h"

@interface HengeCamera : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property(nonatomic) HvUsbVideo *video;
@property(nonatomic, strong) dispatch_queue_t queue;  /* starts and stops */
@property(nonatomic, strong) dispatch_queue_t frames; /* the frames' delegate */
@property(nonatomic, strong) AVCaptureSession *session;
@property(nonatomic, strong) AVCaptureConnection *connection;
#if TARGET_OS_IPHONE
@property(nonatomic, strong) AVCaptureDeviceRotationCoordinator *rotation;
#endif
@end

static HengeCamera *camera;
static _Atomic int want;      /* macOS streams (and the machine isn't paused) */
static _Atomic int want_w, want_h;
static int paused_want;       /* it was streaming when the machine paused */
static _Atomic uint64_t frames_in;

static AVCaptureDevice *pick_device(void) {
#if TARGET_OS_IPHONE
    AVCaptureDevice *d = [AVCaptureDevice defaultDeviceWithDeviceType:AVCaptureDeviceTypeBuiltInWideAngleCamera
                                                            mediaType:AVMediaTypeVideo
                                                             position:AVCaptureDevicePositionFront];
    return d ? d : [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
#else
    return [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
#endif
}

@implementation HengeCamera

- (void)captureOutput:(AVCaptureOutput *)output
    didOutputSampleBuffer:(CMSampleBufferRef)sample
           fromConnection:(AVCaptureConnection *)connection {
    (void)output, (void)connection;
    CVPixelBufferRef px = CMSampleBufferGetImageBuffer(sample);
    if (!px || !atomic_load(&want)) return;
    if (CVPixelBufferLockBaseAddress(px, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return;
    OSType type = CVPixelBufferGetPixelFormatType(px);
    int w = (int)CVPixelBufferGetWidth(px), h = (int)CVPixelBufferGetHeight(px);
    if (type == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange && CVPixelBufferGetPlaneCount(px) == 2) {
        hv_usb_video_frame(self.video, HV_USB_VIDEO_NV12, w, h, CVPixelBufferGetBaseAddressOfPlane(px, 0),
                           CVPixelBufferGetBytesPerRowOfPlane(px, 0), CVPixelBufferGetBaseAddressOfPlane(px, 1),
                           CVPixelBufferGetBytesPerRowOfPlane(px, 1));
    } else if (type == kCVPixelFormatType_32BGRA) {
        hv_usb_video_frame(self.video, HV_USB_VIDEO_BGRA, w, h, CVPixelBufferGetBaseAddress(px),
                           CVPixelBufferGetBytesPerRow(px), NULL, 0);
    }
    CVPixelBufferUnlockBaseAddress(px, kCVPixelBufferLock_ReadOnly);
    if (atomic_fetch_add(&frames_in, 1) == 0) fprintf(stderr, "hengevm: camera: first frame, %dx%d\n", w, h);
}

#if TARGET_OS_IPHONE
- (void)observeValueForKeyPath:(NSString *)keyPath
                      ofObject:(id)object
                        change:(NSDictionary *)change
                       context:(void *)context {
    (void)keyPath, (void)object, (void)change, (void)context;
    dispatch_async(self.queue, ^{
      [self turnUpright];
    });
}

- (void)turnUpright {
    if (!self.rotation || !self.connection) return;
    CGFloat angle = self.rotation.videoRotationAngleForHorizonLevelCapture;
    if ([self.connection isVideoRotationAngleSupported:angle]) self.connection.videoRotationAngle = angle;
}
#endif

- (void)stopSession {
#if TARGET_OS_IPHONE
    if (self.rotation) [self.rotation removeObserver:self forKeyPath:@"videoRotationAngleForHorizonLevelCapture"];
    self.rotation = nil;
#endif
    if (self.session) {
        [self.session stopRunning];
        fprintf(stderr, "hengevm: camera: off\n");
    }
    self.session = nil;
    self.connection = nil;
}

- (NSString *)presetFor:(int)w height:(int)h device:(AVCaptureDevice *)device {
    NSArray<NSString *> *tries = w >= 1920 || h >= 1080 ? @[ AVCaptureSessionPreset1920x1080, AVCaptureSessionPreset1280x720 ]
                                 : w >= 1280 || h >= 720 ? @[ AVCaptureSessionPreset1280x720, AVCaptureSessionPreset1920x1080 ]
                                                         : @[ AVCaptureSessionPreset640x480, AVCaptureSessionPreset1280x720 ];
    for (NSString *p in tries)
        if ([device supportsAVCaptureSessionPreset:p]) return p;
    return AVCaptureSessionPresetHigh;
}

/* Bring the camera to what's wanted now (each call looks afresh, so a quick start and stop settles right). */
- (void)apply {
    if (!atomic_load(&want)) {
        [self stopSession];
        return;
    }
    if (self.session) {
        /* A new size from macOS: a preset nearer it (the USB camera scales whatever comes) */
        AVCaptureDeviceInput *in = (AVCaptureDeviceInput *)self.session.inputs.firstObject;
        NSString *p = [self presetFor:atomic_load(&want_w) height:atomic_load(&want_h) device:in.device];
        if (![self.session.sessionPreset isEqualToString:p] && [self.session canSetSessionPreset:p]) {
            [self.session beginConfiguration];
            self.session.sessionPreset = p;
            [self.session commitConfiguration];
        }
        return;
    }
    AVAuthorizationStatus auth = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
    if (auth == AVAuthorizationStatusNotDetermined) {
        /* The first time macOS uses the camera: iOS (or macOS) asks the person, then this runs again. */
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                                 completionHandler:^(BOOL granted) {
                                   fprintf(stderr, "hengevm: camera: permission %s\n", granted ? "given" : "refused");
                                   if (granted)
                                       dispatch_async(self.queue, ^{
                                         [self apply];
                                       });
                                 }];
        return;
    }
    if (auth != AVAuthorizationStatusAuthorized) {
        fprintf(stderr, "hengevm: camera: not allowed (Settings > Privacy > Camera); the Mac gets a black picture\n");
        return;
    }
    AVCaptureDevice *device = pick_device();
    if (!device) {
        fprintf(stderr, "hengevm: camera: none here; the Mac gets a black picture\n");
        return;
    }
    NSError *err = nil;
    AVCaptureDeviceInput *input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&err];
    AVCaptureSession *s = [[AVCaptureSession alloc] init];
    AVCaptureVideoDataOutput *out = [[AVCaptureVideoDataOutput alloc] init];
    out.videoSettings = @{(id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)};
    out.alwaysDiscardsLateVideoFrames = YES;
    [out setSampleBufferDelegate:self queue:self.frames];
    [s beginConfiguration];
    NSString *preset = [self presetFor:atomic_load(&want_w) height:atomic_load(&want_h) device:device];
    if ([s canSetSessionPreset:preset]) s.sessionPreset = preset;
    if (!input || ![s canAddInput:input] || ![s canAddOutput:out]) {
        [s commitConfiguration];
        fprintf(stderr, "hengevm: camera: can't use it (%s); the Mac gets a black picture\n",
                err ? err.localizedDescription.UTF8String : "the session refused it");
        return;
    }
    [s addInput:input];
    [s addOutput:out];
    [s commitConfiguration];
    self.connection = [out connectionWithMediaType:AVMediaTypeVideo];
    if (self.connection.isVideoMirroringSupported) {
        self.connection.automaticallyAdjustsVideoMirroring = NO;
        self.connection.videoMirrored = NO;
    }
#if TARGET_OS_IPHONE
    self.rotation = [[AVCaptureDeviceRotationCoordinator alloc] initWithDevice:device previewLayer:nil];
    [self.rotation addObserver:self forKeyPath:@"videoRotationAngleForHorizonLevelCapture" options:0 context:NULL];
    [self turnUpright];
#endif
    self.session = s;
    [s startRunning];
    fprintf(stderr, "hengevm: camera: on (%s, %s)\n", device.localizedName.UTF8String, s.sessionPreset.UTF8String);
}

@end

/* The USB camera's word, on the CPU thread: must not block. */
static void on_stream(void *opaque, int on, int width, int height, int fps) {
    (void)opaque, (void)fps;
    if (on) atomic_store(&want_w, width), atomic_store(&want_h, height);
    atomic_store(&want, on);
    HengeCamera *c = camera;
    dispatch_async(c.queue, ^{
      [c apply];
    });
}

void hv_camera_attach(HvUsbVideo *v) {
    if (!v) return;
    if (!camera) {
        camera = [[HengeCamera alloc] init];
        camera.queue = dispatch_queue_create("henge.camera", DISPATCH_QUEUE_SERIAL);
        camera.frames = dispatch_queue_create("henge.camera.frames", DISPATCH_QUEUE_SERIAL);
    }
    camera.video = v;
    hv_usb_video_set_callback(v, on_stream, NULL);
}

void hv_camera_pause(int pause) {
    HengeCamera *c = camera;
    if (!c) return;
    if (pause) {
        paused_want = atomic_exchange(&want, 0);
        if (!paused_want) return;
    } else {
        if (!paused_want) return;
        paused_want = 0;
        atomic_store(&want, 1);
    }
    dispatch_async(c.queue, ^{
      [c apply];
    });
}
