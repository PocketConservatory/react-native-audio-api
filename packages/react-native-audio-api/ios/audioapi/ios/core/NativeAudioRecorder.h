#pragma once

#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>
#import <audioapi/ios/system/AudioEngine.h>

typedef void (^AudioReceiverBlock)(
    const AudioTimeStamp *timestamp,
    const AudioBufferList *inputBuffer,
    int numFrames);

@interface NativeAudioRecorder : NSObject

@property (nonatomic, copy) AVAudioSinkNodeReceiverBlock receiverSinkBlock;
@property (nonatomic, copy) AudioReceiverBlock receiverBlock;
@property (nonatomic, strong) AVAudioFormat *resolvedInputFormat;
@property (nonatomic, assign) int resolvedBufferSize;
@property (atomic, assign) BOOL inputArmed;
/// Buffers per callback for resolvedInputFormat; a callback with another count is dropped.
@property (atomic, assign) UInt32 expectedBufferCount;
@property (nonatomic, assign) BOOL voiceProcessingEnabled;
@property (nonatomic, copy) void (^onInputNotification)(AudioEngineInputNotification);

- (instancetype)initWithReceiverBlock:(AudioReceiverBlock)receiverBlock
               voiceProcessingEnabled:(BOOL)voiceProcessingEnabled;

- (int)getBufferSize;
- (AVAudioFormat *)getResolvedInputFormat;
- (int)getResolvedBufferSize;
- (BOOL)refreshResolvedInputFormatReturningChanged:(BOOL *)formatChanged;
/// Keeps the resolved layout and reads the input at @p sampleRate instead, for frames that arrive
/// at a rate the engine does not report. Later refreshes keep @p sampleRate while the engine still
/// reports the rate it replaced, until the recorder starts or stops.
- (void)overrideResolvedInputSampleRate:(double)sampleRate;
- (BOOL)start:(NSError **)error;

- (void)stop;

- (void)pause;

/// Returns NO when the audio engine could not be started, in which case input stays disarmed.
- (BOOL)resume;

- (void)cleanup;

@end
