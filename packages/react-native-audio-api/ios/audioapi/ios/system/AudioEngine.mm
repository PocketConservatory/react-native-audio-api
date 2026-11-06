#import <audioapi/ios/system/AudioEngine.h>
#import <audioapi/ios/system/AudioSessionManager.h>

#include <mutex>

@interface AudioEngineSourceRegistration : NSObject

@property (nonatomic, copy) AVAudioSourceNodeRenderBlock renderBlock;
@property (nonatomic, assign) float sampleRate;
@property (nonatomic, assign) AVAudioChannelCount channelCount;

@end

@implementation AudioEngineSourceRegistration
@end

@interface AudioEngineInputRegistration : NSObject

@property (nonatomic, copy) AVAudioSinkNodeReceiverBlock receiverBlock;
@property (nonatomic, copy) void (^onInputNotification)(AudioEngineInputNotification);
@property (nonatomic, assign) BOOL inputPaused;

@end

@implementation AudioEngineInputRegistration
@end

@interface AudioEngine () {
  std::recursive_mutex _engineLock;
  BOOL _isRebuildingAudioEngine;
}

@property (nonatomic, strong)
    NSMutableDictionary<NSString *, AudioEngineSourceRegistration *> *sourceRegistrations;
@property (nonatomic, strong) AudioEngineInputRegistration *inputRegistration;

- (void)createAudioEngineIfNeeded;
- (void)destroyAudioEnginePreservingSessionDeactivationState:(BOOL)preserveSessionDeactivationState;
- (BOOL)hasInput;
- (BOOL)hasLiveInput;
- (BOOL)hasSources;
- (BOOL)hasTrackedGraph;
- (AVAudioFormat *)currentInputConnectionFormat;
- (void)materializeSourceNodeWithId:(NSString *)sourceNodeId;
- (BOOL)materializeInputNodeIfNeeded;
- (void)materializeTrackedNodesIfNeeded;

- (AVAudioFormat *)liveInputFormat;
- (void)resetInputNode;
- (void)rebuildAudioEngineAndResumeIfNeeded;
- (BOOL)startPreparedEngine:(NSError **)error;
- (void)notifyInput:(AudioEngineInputNotification)notification;

@end

@implementation AudioEngine

static AudioEngine *_sharedInstance = nil;

- (void)createAudioEngineIfNeeded
{
  if (self.audioEngine != nil) {
    return;
  }

  self.audioEngine = [[AVAudioEngine alloc] init];
}

- (void)destroyAudioEngine
{
  [self destroyAudioEnginePreservingSessionDeactivationState:NO];
}

- (BOOL)hasInput
{
  return self.inputRegistration != nil;
}

/// An input marked paused stays registered but is not part of the live graph: the engine
/// neither materializes it nor reads its format, so starting the engine for other consumers
/// never turns the microphone on.
- (BOOL)hasLiveInput
{
  return [self hasInput] && !self.inputRegistration.inputPaused;
}

- (BOOL)hasSources
{
  return [self.sourceRegistrations count] > 0;
}

- (BOOL)hasTrackedGraph
{
  return [self hasInput] || [self hasSources];
}

- (void)destroyAudioEnginePreservingSessionDeactivationState:(BOOL)preserveSessionDeactivationState
{
  BOOL hadGraph = [self hasTrackedGraph];

  if (self.audioEngine != nil) {
    if ([self.audioEngine isRunning]) {
      [self.audioEngine stop];
    }

    [self.audioEngine reset];
  }

  self.audioEngine = nil;
  self.sourceNodes = [[NSMutableDictionary alloc] init];
  self.sourceFormats = [[NSMutableDictionary alloc] init];
  self.inputNode = nil;
  self.graphNeedsRebuild = hadGraph;

  if (!preserveSessionDeactivationState) {
    self.sessionDeactivationInvalidatedGraph = false;
  }
}

+ (instancetype)sharedInstance
{
  return _sharedInstance;
}

- (instancetype)init
{
  if (self = [super init]) {
    self.state = AudioEngineState::AudioEngineStateIdle;
    self.audioEngine = nil;
    self.inputNode = nil;
    self.graphNeedsRebuild = false;
    self.sessionDeactivationInvalidatedGraph = false;

    self.sourceRegistrations = [[NSMutableDictionary alloc] init];
    self.sourceNodes = [[NSMutableDictionary alloc] init];
    self.sourceFormats = [[NSMutableDictionary alloc] init];
    self.inputRegistration = nil;

    self.sessionManager = [AudioSessionManager sharedInstance];
    [self createAudioEngineIfNeeded];
  }

  _sharedInstance = self;
  return self;
}

- (void)cleanup
{
  std::scoped_lock lock(_engineLock);
  [self destroyAudioEngine];
  self.state = AudioEngineState::AudioEngineStateIdle;
  self.sourceRegistrations = nil;
  self.sourceNodes = nil;
  self.sourceFormats = nil;
  self.inputRegistration = nil;
  self.inputNode = nil;
  self.graphNeedsRebuild = false;
  self.sessionDeactivationInvalidatedGraph = false;

  [self.sessionManager setActive:false error:nil];
  self.sessionManager = nil;

  if (_sharedInstance == self) {
    _sharedInstance = nil;
  }
}

- (void)materializeSourceNodeWithId:(NSString *)sourceNodeId
{
  AudioEngineSourceRegistration *registration = self.sourceRegistrations[sourceNodeId];

  if (registration == nil || self.audioEngine == nil || self.sourceNodes[sourceNodeId] != nil) {
    return;
  }

  AVAudioFormat *format =
      [[AVAudioFormat alloc] initStandardFormatWithSampleRate:registration.sampleRate
                                                     channels:registration.channelCount];
  AVAudioSourceNode *sourceNode =
      [[AVAudioSourceNode alloc] initWithFormat:format renderBlock:registration.renderBlock];

  self.sourceNodes[sourceNodeId] = sourceNode;
  self.sourceFormats[sourceNodeId] = format;

  [self.audioEngine attachNode:sourceNode];
  [self.audioEngine connect:sourceNode to:self.audioEngine.mainMixerNode format:format];
}

- (AVAudioFormat *)liveInputFormat
{
  std::scoped_lock lock(_engineLock);

  if (self.audioEngine == nil || self.inputRegistration.inputPaused) {
    return nil;
  }

  AVAudioInputNode *engineInputNode = self.audioEngine.inputNode;

  if (engineInputNode == nil) {
    return nil;
  }

  AVAudioFormat *inputFormat = [engineInputNode outputFormatForBus:0];

  if (inputFormat == nil || inputFormat.sampleRate <= 0 || inputFormat.channelCount == 0) {
    return nil;
  }

  return inputFormat;
}

- (AVAudioFormat *)currentInputConnectionFormat
{
  AVAudioFormat *inputFormat = [self liveInputFormat];

  if (inputFormat == nil || inputFormat.sampleRate <= 0 || inputFormat.channelCount == 0) {
    return nil;
  }

  return inputFormat;
}

- (BOOL)materializeInputNodeIfNeeded
{
  if (![self hasLiveInput]) {
    return YES;
  }

  if (self.audioEngine == nil) {
    return NO;
  }

  if (self.inputNode != nil) {
    return YES;
  }

  NSError *sessionError = nil;
  if (![self.sessionManager ensureActiveWithError:&sessionError]) {
    NSLog(
        @"Error while activating audio session before input materialization: %@",
        [sessionError debugDescription]);
    return NO;
  }

  AVAudioFormat *inputFormat = [self currentInputConnectionFormat];

  if (inputFormat == nil) {
    return NO;
  }

  self.inputNode =
      [[AVAudioSinkNode alloc] initWithReceiverBlock:self.inputRegistration.receiverBlock];
  [self.audioEngine attachNode:self.inputNode];
  [self.audioEngine connect:self.audioEngine.inputNode to:self.inputNode format:inputFormat];
  return YES;
}

- (void)materializeTrackedNodesIfNeeded
{
  NSArray<NSString *> *sourceNodeIds =
      [[self.sourceRegistrations allKeys] sortedArrayUsingSelector:@selector(compare:)];
  for (NSString *sourceNodeId in sourceNodeIds) {
    [self materializeSourceNodeWithId:sourceNodeId];
  }

  [self materializeInputNodeIfNeeded];
}

- (NSString *)attachSourceNodeWithRenderBlock:(AVAudioSourceNodeRenderBlock)renderBlock
                                   sampleRate:(float)sampleRate
                                 channelCount:(AVAudioChannelCount)channelCount
{
  std::scoped_lock lock(_engineLock);
  [self createAudioEngineIfNeeded];

  NSString *sourceNodeId = [[NSUUID UUID] UUIDString];
  AudioEngineSourceRegistration *registration = [[AudioEngineSourceRegistration alloc] init];
  registration.renderBlock = renderBlock;
  registration.sampleRate = sampleRate;
  registration.channelCount = channelCount;

  self.sourceRegistrations[sourceNodeId] = registration;
  [self materializeSourceNodeWithId:sourceNodeId];

  return sourceNodeId;
}

- (void)detachSourceNodeWithId:(NSString *)sourceNodeId
{
  std::scoped_lock lock(_engineLock);
  AVAudioSourceNode *sourceNode = self.sourceNodes[sourceNodeId];

  if (self.sourceRegistrations[sourceNodeId] == nil) {
    NSLog(@"[AudioEngine] No source node found with ID: %@", sourceNodeId);
    return;
  }

  if (sourceNode != nil && self.audioEngine != nil) {
    [self.audioEngine detachNode:sourceNode];
  }

  [self.sourceRegistrations removeObjectForKey:sourceNodeId];
  [self.sourceNodes removeObjectForKey:sourceNodeId];
  [self.sourceFormats removeObjectForKey:sourceNodeId];

  if (![self hasTrackedGraph]) {
    self.graphNeedsRebuild = false;
  }
}

- (void)attachInputNodeWithReceiverBlock:(AVAudioSinkNodeReceiverBlock)receiverBlock
                     onInputNotification:(void (^)(AudioEngineInputNotification))onInputNotification
{
  std::scoped_lock lock(_engineLock);
  [self createAudioEngineIfNeeded];

  if (self.inputRegistration != nil || self.inputNode != nil) {
    [self resetInputNode];
  }

  AudioEngineInputRegistration *registration = [[AudioEngineInputRegistration alloc] init];
  registration.receiverBlock = receiverBlock;
  registration.onInputNotification = onInputNotification;
  self.inputRegistration = registration;

  [self materializeInputNodeIfNeeded];
}

- (void)resetInputNode
{
  if (self.inputRegistration == nil && self.inputNode == nil) {
    return;
  }

  if (self.inputNode != nil && self.audioEngine != nil) {
    [self.audioEngine detachNode:self.inputNode];
  }

  self.inputRegistration = nil;
  self.inputNode = nil;

  if (![self hasTrackedGraph]) {
    self.graphNeedsRebuild = false;
  }
}

- (void)detachInputNode
{
  std::scoped_lock lock(_engineLock);
  [self resetInputNode];
}

- (AVAudioFormat *)getLiveInputFormat
{
  return [self liveInputFormat];
}

// Pausing an input turns the microphone off and leaves every other consumer as it was. A
// running engine with sources is rebuilt without the input and keeps playing; one whose only
// consumer is the input is paused; an engine that is not running keeps its state, so an
// Interrupted one stays Interrupted for interruption-end and foreground recovery. If the rebuilt
// engine fails to start, the engine reports Paused rather than Running over stopped playback;
// the failure is logged and the next start (a player or resumeInput) rebuilds and restarts it.
- (void)pauseInput
{
  std::scoped_lock lock(_engineLock);
  AudioEngineInputRegistration *registration = self.inputRegistration;

  if (registration == nil || registration.inputPaused) {
    return;
  }

  registration.inputPaused = YES;
  // The live graph loses the input on the next build.
  self.graphNeedsRebuild = true;

  if (self.state != AudioEngineState::AudioEngineStateRunning) {
    return;
  }

  if (![self hasSources]) {
    if (self.audioEngine != nil) {
      [self.audioEngine pause];
    }

    self.state = AudioEngineState::AudioEngineStatePaused;
    return;
  }

  [self rebuildAudioEngineAndResumeIfNeeded];

  if (self.audioEngine == nil || ![self.audioEngine isRunning]) {
    NSLog(@"Error while restarting the audio engine without the paused input");
    self.state = AudioEngineState::AudioEngineStatePaused;
  }
}

// Resuming an input leaves the engine state and the input's paused mark exactly as they were
// unless the engine starts: an Interrupted engine stays Interrupted (its interruption-end and
// foreground recovery still apply), a Paused or Idle one stays so, and an input that was never
// paused is never marked paused. Only a Running engine whose graph left the paused input out is
// stopped, to be rebuilt with it.
- (bool)resumeInput
{
  std::scoped_lock lock(_engineLock);
  AudioEngineInputRegistration *registration = self.inputRegistration;

  if (registration == nil) {
    return false;
  }

  const BOOL wasPaused = registration.inputPaused;
  registration.inputPaused = NO;

  if (wasPaused) {
    self.graphNeedsRebuild = true;

    if (self.state == AudioEngineState::AudioEngineStateRunning) {
      [self stopEngine];
    }
  }

  if ([self startIfNecessary]) {
    return true;
  }

  registration.inputPaused = wasPaused;
  return false;
}

- (bool)onInterruptionBegin
{
  std::scoped_lock lock(_engineLock);
  if (self.state != AudioEngineState::AudioEngineStateRunning) {
    return false;
  }

  self.state = AudioEngineState::AudioEngineStateInterrupted;
  return true;
}

- (void)onSessionDeactivated
{
  std::scoped_lock lock(_engineLock);
  BOOL hadTrackedGraph = [self hasTrackedGraph];
  BOOL hadActiveState = self.state != AudioEngineState::AudioEngineStateIdle;

  if (!hadActiveState && !hadTrackedGraph) {
    return;
  }

  self.sessionDeactivationInvalidatedGraph = true;

  if (hadTrackedGraph) {
    self.graphNeedsRebuild = true;
  }

  if (self.audioEngine != nil && ![self.audioEngine isRunning]) {
    self.state = AudioEngineState::AudioEngineStatePaused;
    return;
  }

  if (self.audioEngine != nil) {
    [self.audioEngine pause];
  }

  self.state = AudioEngineState::AudioEngineStatePaused;
}

- (void)markSessionDeactivationInvalidatedGraph
{
  std::scoped_lock lock(_engineLock);
  self.sessionDeactivationInvalidatedGraph = YES;
}

- (void)markGraphNeedsRebuild
{
  std::scoped_lock lock(_engineLock);
  self.graphNeedsRebuild = true;
}

// Interruption recovery restores engine and session state only, never recorder state: the
// recorder alone moves between recording and paused. A paused input is not part of the live
// graph (see hasLiveInput), so recovery treats it as no input and never restarts it; the
// recorder refreshes its format on resume. According to
// <https://developer.apple.com/documentation/avfaudio/avaudiosession/interruptionoptions/shouldresume>
// `shouldResume` only refers to resuming playback, so a live input resumes unconditionally. An
// engine that also plays sources resumes only when playback may, since it cannot resume one
// without the other.
static BOOL
shouldEngineResumeAfterInterruption(BOOL shouldPlaybackResume, BOOL hasLiveInput, BOOL hasSources)
{
  if (hasLiveInput) {
    return !hasSources || shouldPlaybackResume;
  }

  return hasSources && shouldPlaybackResume;
}

- (AudioEngineInterruptionEndOutcome)onInterruptionEnd:(bool)shouldResume
{
  std::scoped_lock lock(_engineLock);
  NSError *error = nil;

  if (self.state != AudioEngineState::AudioEngineStateInterrupted) {
    return AudioEngineInterruptionEndOutcomeNoOp;
  }

  const BOOL resumeEngine =
      shouldEngineResumeAfterInterruption(shouldResume, [self hasLiveInput], [self hasSources]);

  if (!resumeEngine) {
    [self stopEngine];
    [self rebuildAudioEngine];
    self.state = AudioEngineState::AudioEngineStatePaused;
    [self notifyInput:AudioEngineInputNotificationHardwareChanged];
    return AudioEngineInterruptionEndOutcomePaused;
  }

  if (![self.sessionManager ensureActiveWithError:&error]) {
    NSLog(@"Error while activating audio session after interruption: %@", [error debugDescription]);
    return AudioEngineInterruptionEndOutcomeStillInterrupted;
  }

  [self stopEngine];
  [self rebuildAudioEngine];

  if ([self hasLiveInput] && self.inputNode == nil) {
    NSLog(
        @"Error while materializing the audio input node after interruption: missing live input format");
    self.state = AudioEngineState::AudioEngineStateInterrupted;
    return AudioEngineInterruptionEndOutcomeStillInterrupted;
  }

  if (![self startPreparedEngine:&error]) {
    NSLog(
        @"Error while restarting the audio engine after interruption: %@",
        [error debugDescription]);
    self.state = AudioEngineState::AudioEngineStateInterrupted;
    return AudioEngineInterruptionEndOutcomeStillInterrupted;
  }

  self.state = AudioEngineState::AudioEngineStateRunning;
  self.sessionDeactivationInvalidatedGraph = false;
  return AudioEngineInterruptionEndOutcomeRunning;
}

/// Every engine start goes through here, so the input consumers match the live input format
/// before the first render callback arrives on it.
- (BOOL)startPreparedEngine:(NSError **)error
{
  [self notifyInput:AudioEngineInputNotificationHardwareChanged];
  [self.audioEngine prepare];
  return [self.audioEngine startAndReturnError:error];
}

- (void)notifyInput:(AudioEngineInputNotification)notification
{
  if ([self hasLiveInput] && self.inputRegistration.onInputNotification != nil) {
    self.inputRegistration.onInputNotification(notification);
  }
}

- (AudioEngineState)getState
{
  std::scoped_lock lock(_engineLock);
  return self.state;
}

- (bool)isEngineRunning
{
  std::scoped_lock lock(_engineLock);
  return self.audioEngine != nil && [self.audioEngine isRunning];
}

- (void)rebuildAudioEngineAndResumeIfNeeded
{
  if (_isRebuildingAudioEngine) {
    return;
  }

  _isRebuildingAudioEngine = YES;

  if ([self.audioEngine isRunning]) {
    [self.audioEngine stop];
  }

  [self rebuildAudioEngine];
  self.sessionDeactivationInvalidatedGraph = false;

  if (self.state == AudioEngineState::AudioEngineStateRunning) {
    [self startEngine];
  }

  _isRebuildingAudioEngine = NO;
}

- (void)rebuildAudioEngine
{
  [self destroyAudioEnginePreservingSessionDeactivationState:YES];
  [self createAudioEngineIfNeeded];

  [self materializeTrackedNodesIfNeeded];
  self.graphNeedsRebuild = false;
}

- (bool)startEngine
{
  NSError *error = nil;

  if (self.audioEngine != nil && [self.audioEngine isRunning] &&
      self.state == AudioEngineState::AudioEngineStateRunning) {
    return true;
  }

  [self createAudioEngineIfNeeded];

  if (![self.sessionManager ensureActiveWithError:&error]) {
    NSLog(@"Error while activating audio session: %@", [error debugDescription]);
    return false;
  }

  if (self.state == AudioEngineState::AudioEngineStateInterrupted || self.graphNeedsRebuild ||
      self.sessionDeactivationInvalidatedGraph) {
    [self rebuildAudioEngineAndResumeIfNeeded];
  } else {
    [self materializeTrackedNodesIfNeeded];
  }

  if ([self hasLiveInput] && self.inputNode == nil) {
    NSLog(@"Error while materializing the audio input node: missing live input format");
    return false;
  }

  if (![self startPreparedEngine:&error]) {
    NSLog(@"Error while starting the audio engine: %@", [error debugDescription]);
    return false;
  }

  self.state = AudioEngineState::AudioEngineStateRunning;
  self.sessionDeactivationInvalidatedGraph = false;
  return true;
}

- (void)stopEngine
{
  if (self.state == AudioEngineState::AudioEngineStateIdle) {
    return;
  }

  if (self.audioEngine != nil && [self.audioEngine isRunning]) {
    [self.audioEngine stop];
  }

  self.state = AudioEngineState::AudioEngineStateIdle;
}

- (bool)startIfNecessary
{
  std::scoped_lock lock(_engineLock);
  if (self.state == AudioEngineState::AudioEngineStateRunning && self.audioEngine != nil &&
      [self.audioEngine isRunning]) {
    [self materializeTrackedNodesIfNeeded];
    return true;
  }

  if ([self hasTrackedGraph]) {
    return [self startEngine];
  }

  return false;
}

- (void)stopIfNecessary
{
  std::scoped_lock lock(_engineLock);
  [self stopEngine];
}

- (void)stopIfPossible
{
  std::scoped_lock lock(_engineLock);
  if ([self hasInput] || [self hasSources]) {
    return;
  }

  if (self.state != AudioEngineState::AudioEngineStateIdle) {
    [self stopEngine];
  }
}

- (void)restartAudioEngine
{
  std::scoped_lock lock(_engineLock);
  [self rebuildAudioEngineAndResumeIfNeeded];
}

- (void)logAudioEngineState
{
  std::scoped_lock lock(_engineLock);
  AVAudioSession *session = [AVAudioSession sharedInstance];

  NSLog(@"================ 🎧 AVAudioEngine STATE ================");

  NSLog(@"➡️ engine.isRunning: %@", self.audioEngine.isRunning ? @"true" : @"false");
  NSLog(
      @"➡️ engine.isInManualRenderingMode: %@",
      self.audioEngine.isInManualRenderingMode ? @"true" : @"false");

  NSLog(@"🎚️ Session category: %@", session.category);
  NSLog(@"🎚️ Session mode: %@", session.mode);
  NSLog(@"🎚️ Session sampleRate: %f Hz", session.sampleRate);
  NSLog(@"🎚️ Session IO buffer duration: %f s", session.IOBufferDuration);

  AVAudioSessionRouteDescription *route = session.currentRoute;

  NSLog(@"🔊 Current audio route outputs:");
  for (AVAudioSessionPortDescription *output in route.outputs) {
    NSLog(@"  Output: %@ (%@)", output.portType, output.portName);
  }

  NSLog(@"🎤 Current audio route inputs:");
  for (AVAudioSessionPortDescription *input in route.inputs) {
    NSLog(@"  Input: %@ (%@)", input.portType, input.portName);
  }

  AVAudioFormat *format = [self.audioEngine.outputNode inputFormatForBus:0];
  NSLog(@"📐 Engine output format: %.0f Hz, %u channels", format.sampleRate, format.channelCount);

  NSLog(@"=======================================================");
}

@end
