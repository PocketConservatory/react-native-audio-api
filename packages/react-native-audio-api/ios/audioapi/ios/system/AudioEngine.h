#pragma once

#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>

@class AudioSessionManager;

typedef NS_ENUM(NSInteger, AudioEngineState) {
  AudioEngineStateIdle = 0,
  AudioEngineStateRunning,
  AudioEngineStatePaused,
  AudioEngineStateInterrupted
};

typedef NS_ENUM(NSInteger, AudioEngineInputNotification) {
  AudioEngineInputNotificationHardwareChanged = 0,
};

/// Result of `onInterruptionEnd:`. Distinguishes a no-op from a failed resume that stays Interrupted.
typedef NS_ENUM(NSInteger, AudioEngineInterruptionEndOutcome) {
  AudioEngineInterruptionEndOutcomeNoOp = 0,
  AudioEngineInterruptionEndOutcomeRunning,
  AudioEngineInterruptionEndOutcomePaused,
  AudioEngineInterruptionEndOutcomeStillInterrupted
};

@interface AudioEngine : NSObject

@property (nonatomic, assign) AudioEngineState state;
@property (nonatomic, strong) AVAudioEngine *audioEngine;
@property (nonatomic, strong) NSMutableDictionary *sourceNodes;
@property (nonatomic, strong) NSMutableDictionary *sourceFormats;
@property (nonatomic, strong) AVAudioSinkNode *inputNode;
@property (nonatomic, weak) AudioSessionManager *sessionManager;
@property (nonatomic, assign) BOOL graphNeedsRebuild;
@property (nonatomic, assign) BOOL sessionDeactivationInvalidatedGraph;

- (instancetype)init;
+ (instancetype)sharedInstance;

- (void)cleanup;

- (NSString *)attachSourceNodeWithRenderBlock:(AVAudioSourceNodeRenderBlock)renderBlock
                                   sampleRate:(float)sampleRate
                                 channelCount:(AVAudioChannelCount)channelCount;
- (void)detachSourceNodeWithId:(NSString *)sourceNodeId;

- (void)attachInputNodeWithReceiverBlock:(AVAudioSinkNodeReceiverBlock)receiverBlock
                     onInputNotification:
                         (void (^)(AudioEngineInputNotification))onInputNotification;
- (void)detachInputNode;
- (AVAudioFormat *)getLiveInputFormat;
/// Marks the attached input as paused by its recorder and turns it off, leaving every other
/// consumer as it was: a running engine that plays sources is rebuilt without the input and keeps
/// running, one whose only consumer is the input is paused, and an engine that is not running
/// keeps its state. A paused input is left out of the live graph (a player starting the engine
/// does not turn the microphone on), gets no input notifications, and is never restarted by
/// interruption recovery; only `resumeInput` clears the mark.
- (void)pauseInput;
/// Clears the input's paused mark and starts the engine with the input live. Leaves the engine
/// state and the paused mark as they were unless the engine starts.
- (bool)resumeInput;

/// @return true if the engine transitioned from Running to Interrupted.
- (bool)onInterruptionBegin;
- (AudioEngineInterruptionEndOutcome)onInterruptionEnd:(bool)shouldResume;
- (void)onSessionDeactivated;
- (void)markSessionDeactivationInvalidatedGraph;
/// Records that hardware format may have changed while the engine must not rebuild
/// yet (`Interrupted`). The next start or interruption-end resume rebuilds the graph.
- (void)markGraphNeedsRebuild;

- (AudioEngineState)getState;
- (bool)isEngineRunning;

- (bool)startIfNecessary;
- (void)stopIfNecessary;

- (void)stopIfPossible;

- (void)restartAudioEngine;

- (void)logAudioEngineState;

@end
