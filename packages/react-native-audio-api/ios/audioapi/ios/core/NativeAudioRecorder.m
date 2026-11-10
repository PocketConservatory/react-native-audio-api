#import <audioapi/ios/core/NativeAudioRecorder.h>
#import <audioapi/ios/system/AudioEngine.h>

@interface NativeAudioRecorder ()

/// The rate the engine reports for input measured to run at `measuredSampleRate`; 0 when none.
@property (nonatomic, assign) double staleSampleRate;
@property (nonatomic, assign) double measuredSampleRate;

@end

@implementation NativeAudioRecorder

static inline uint32_t nextPowerOfTwo(uint32_t x)
{
  if (x == 0) {
    return 1;
  }

  x--;
  x |= x >> 1;
  x |= x >> 2;
  x |= x >> 4;
  x |= x >> 8;
  x |= x >> 16;
  x++;

  return x;
}

static AVAudioFormat *formatWithSampleRate(AVAudioFormat *format, double sampleRate)
{
  AudioStreamBasicDescription description = *format.streamDescription;
  description.mSampleRate = sampleRate;
  return [[AVAudioFormat alloc] initWithStreamDescription:&description
                                            channelLayout:format.channelLayout];
}

- (AVAudioFormat *)readLiveInputFormat
{
  AVAudioFormat *liveFormat = [[AudioEngine sharedInstance] getLiveInputFormat];
#if DEBUG
  // QA only: makes the engine's reported input rate look stale for a whole take.
  double forcedSampleRate =
      [[NSUserDefaults standardUserDefaults] doubleForKey:@"AudioAPIDebugForceInputSampleRate"];
  if (liveFormat != nil && forcedSampleRate > 0) {
    return formatWithSampleRate(liveFormat, forcedSampleRate);
  }
#endif
  return liveFormat;
}

- (instancetype)initWithReceiverBlock:(AudioReceiverBlock)receiverBlock
               voiceProcessingEnabled:(BOOL)voiceProcessingEnabled
{
  if (self = [super init]) {
    self.receiverBlock = [receiverBlock copy];
    self.voiceProcessingEnabled = voiceProcessingEnabled;
    self.inputArmed = NO;
    self.resolvedBufferSize = 0;

    __weak typeof(self) weakSelf = self;
    self.receiverSinkBlock = ^OSStatus(
        const AudioTimeStamp *_Nonnull timestamp,
        AVAudioFrameCount frameCount,
        const AudioBufferList *_Nonnull inputData) {
      if (!weakSelf.inputArmed || weakSelf.receiverBlock == nil ||
          inputData->mNumberBuffers != weakSelf.expectedBufferCount) {
        return kAudioServicesNoError;
      }

      weakSelf.receiverBlock(timestamp, inputData, frameCount);

      return kAudioServicesNoError;
    };
  }

  return self;
}

- (AVAudioFormat *)getResolvedInputFormat
{
  return self.resolvedInputFormat;
}

- (void)storeResolvedInputFormat:(AVAudioFormat *)format
{
  self.resolvedInputFormat = format;
  if (format == nil) {
    self.staleSampleRate = 0;
  }
  self.resolvedBufferSize = format == nil ? 0 : [self getBufferSize];
  self.expectedBufferCount = format == nil ? 0 : (format.isInterleaved ? 1 : format.channelCount);
}

- (int)getBufferSize
{
  AVAudioSession *audioSession = [AVAudioSession sharedInstance];
  double sampleRate = audioSession.sampleRate;

  if (self.resolvedInputFormat != nil && self.resolvedInputFormat.sampleRate > 0) {
    sampleRate = self.resolvedInputFormat.sampleRate;
  }

  float bufferDuration = MAX(audioSession.IOBufferDuration, 0.2);
  return nextPowerOfTwo(ceil(bufferDuration * sampleRate));
}

- (int)getResolvedBufferSize
{
  return self.resolvedBufferSize;
}

- (BOOL)refreshResolvedInputFormatReturningChanged:(BOOL *)formatChanged
{
  AVAudioFormat *liveFormat = [self readLiveInputFormat];

  if (liveFormat == nil || liveFormat.sampleRate <= 0 || liveFormat.channelCount == 0) {
    return NO;
  }

  if (self.staleSampleRate > 0) {
    if (liveFormat.sampleRate == self.staleSampleRate) {
      liveFormat = formatWithSampleRate(liveFormat, self.measuredSampleRate);
    } else {
      self.staleSampleRate = 0;
    }
  }

  AVAudioFormat *previousFormat = self.resolvedInputFormat;
  BOOL changed = previousFormat == nil || previousFormat.sampleRate != liveFormat.sampleRate ||
      previousFormat.channelCount != liveFormat.channelCount ||
      previousFormat.isInterleaved != liveFormat.isInterleaved;

  [self storeResolvedInputFormat:liveFormat];

  if (formatChanged != nil) {
    *formatChanged = changed;
  }

  return YES;
}

- (void)overrideResolvedInputSampleRate:(double)sampleRate
{
  AVAudioFormat *format = self.resolvedInputFormat;
  if (format == nil) {
    return;
  }

  if (self.staleSampleRate == 0) {
    self.staleSampleRate = format.sampleRate;
  }
  self.measuredSampleRate = sampleRate;
  [self storeResolvedInputFormat:formatWithSampleRate(format, sampleRate)];
}

- (BOOL)start:(NSError **)error
{
  AudioEngine *audioEngine = [AudioEngine sharedInstance];
  assert(audioEngine != nil);

  // AudioEngine allows us to attach and connect nodes at runtime but with few
  // limitations in this case if it is the first recorder node and player
  // started the engine we need to restart. It can be optimized by tracking if
  // we haven't break rules of at runtime modifications from docs
  // https://developer.apple.com/documentation/avfaudio/avaudioengine?language=objc
  //
  // Currently we are restarting because we do not see any significant performance issue and case when
  // you will need to start and stop recorder very frequently
  self.inputArmed = NO;
  [self storeResolvedInputFormat:nil];

  [audioEngine stopIfNecessary];
  [audioEngine attachInputNodeWithReceiverBlock:self.receiverSinkBlock
                         voiceProcessingEnabled:self.voiceProcessingEnabled
                            onInputNotification:self.onInputNotification];

  if (![audioEngine startIfNecessary]) {
    [audioEngine detachInputNode];
    [audioEngine stopIfPossible];

    if (error != nil) {
      *error = [NSError
          errorWithDomain:@"NativeAudioRecorder"
                     code:1
                 userInfo:@{
                   NSLocalizedDescriptionKey : @"Failed to start audio engine for recording",
                 }];
    }

    return NO;
  }

  [self storeResolvedInputFormat:[self readLiveInputFormat]];

  if (error != nil) {
    *error = nil;
  }

  return YES;
}

- (void)stop
{
  AudioEngine *audioEngine = [AudioEngine sharedInstance];
  assert(audioEngine != nil);
  self.inputArmed = NO;
  [audioEngine detachInputNode];
  [audioEngine stopIfPossible];
  [audioEngine restartAudioEngine];
  [self storeResolvedInputFormat:nil];
}

- (void)pause
{
  AudioEngine *audioEngine = [AudioEngine sharedInstance];
  assert(audioEngine != nil);

  self.inputArmed = NO;
  [audioEngine pauseInput];
}

- (BOOL)resume
{
  AudioEngine *audioEngine = [AudioEngine sharedInstance];
  assert(audioEngine != nil);

  return [audioEngine resumeInput];
}

- (void)cleanup
{
  self.inputArmed = NO;
  [self storeResolvedInputFormat:nil];
  self.receiverBlock = nil;
  self.receiverSinkBlock = nil;
  self.onInputNotification = nil;
}

@end
