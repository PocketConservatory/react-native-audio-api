#include <audioapi/core/inputs/ActiveRecorderHandle.h>
#include <audioapi/core/inputs/AudioRecorder.h>
#include <audioapi/events/AudioEventPayload.h>
#include <gtest/gtest.h>
#include <test/src/MockAudioEventHandlerRegistry.h>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace audioapi;

// NOLINTBEGIN

namespace audioapi {

/// Grants the tests the private constructor, so each case can act on a handle of its
/// own instead of the process-global one they would otherwise share.
struct ActiveRecorderHandleTestPeer {
  static std::unique_ptr<ActiveRecorderHandle> createHandle() {
    return std::unique_ptr<ActiveRecorderHandle>(new ActiveRecorderHandle());
  }
};

} // namespace audioapi

namespace {

class FakeAudioRecorder : public AudioRecorder {
 public:
  explicit FakeAudioRecorder(
      const std::shared_ptr<IAudioEventHandlerRegistry> &audioEventHandlerRegistry = nullptr)
      : AudioRecorder(audioEventHandlerRegistry) {}

  static constexpr float kSampleRate = 48000.0F;
  static constexpr size_t kFramesPerBuffer = 128;

  std::vector<std::string> stopPaths{"file:///tmp/recording.m4a"};
  std::atomic<int> stopCount{0};

  Result<NoneType, std::string> start() override {
    if (state_ != RecorderState::Idle) {
      return Err(std::string("Recorder is already recording"));
    }
    state_ = RecorderState::Recording;
    return Ok(None);
  }

  // Mirrors AndroidAudioRecorder::stop(): under its locks exactly one caller
  // transitions out of a non-idle state and closes the file; the loser errs.
  Result<FileInfo, std::string> stop() override {
    if (state_.exchange(RecorderState::Idle) == RecorderState::Idle) {
      return Err(std::string("Recorder is not in recording state."));
    }
    stopCount += 1;
    return Ok(FileInfo{.paths = stopPaths, .size = 1.5, .duration = 10.0});
  }

  // Mirrors the platform recorders: pause() and resume() go through the shared state
  // transitions, and isRecording() also requires a live input stream, which a restart
  // (Android) or an interruption (iOS) takes away.
  bool streamDown = false;

  void pause() override {
    markPaused();
  }
  void resume() override {
    markResumed();
  }
  Result<NoneType, std::string> rerouteInput() override {
    return Result<NoneType, std::string>::Ok(None);
  }

  /// Stands in for the platform input callback (Oboe's onAudioReady, the iOS receiver block),
  /// which keeps delivering whenever the stream is up, paused or not.
  void deliverInput(const float *const *channels, int numFrames) {
    if (!streamDown) {
      onAudioFrames(channels, numFrames);
    }
  }

  bool isRecording() const override {
    return state_ == RecorderState::Recording && !streamDown;
  }
  bool isPaused() const override {
    return state_ == RecorderState::Paused;
  }
  bool isIdle() const override {
    return state_ == RecorderState::Idle;
  }

  [[nodiscard]] double getInputLatency() const override {
    return 0.0;
  }

 protected:
  [[nodiscard]] Result<StreamFormat, std::string> resolveStreamFormat() const override {
    return Ok(
        StreamFormat{
            .layout = AudioLayout{.sampleRate = kSampleRate, .channelCount = 1},
            .maxFramesPerBuffer = kFramesPerBuffer,
        });
  }
};

/// Counts the frames an onAudioReady callback receives while @p feed runs. The callback is
/// registered on a live session and cleared afterwards, which drains its worker and flushes
/// whatever it buffered, so every frame that got through has been counted on return.
template <typename Feed>
int framesDeliveredToCallback(
    MockAudioEventHandlerRegistry &registry,
    FakeAudioRecorder &recorder,
    Feed &&feed) {
  constexpr uint64_t kCallbackId = 7;
  std::atomic<int> deliveredFrames{0};
  ON_CALL(registry, dispatchEvent(AudioEvent::AUDIO_READY, kCallbackId, testing::_))
      .WillByDefault([&](AudioEvent, uint64_t, AudioEventPayload &&payload) {
        deliveredFrames += std::get<AudioReadyPayload>(payload).numFrames;
        return true;
      });

  EXPECT_TRUE(
      recorder
          .setOnAudioReadyCallback(
              FakeAudioRecorder::kSampleRate, FakeAudioRecorder::kFramesPerBuffer, 1, kCallbackId)
          .is_ok());
  feed();
  recorder.clearOnAudioReadyCallback();
  return deliveredFrames.load();
}

/// One full input buffer through the platform input callback.
void deliverOneBuffer(FakeAudioRecorder &recorder) {
  std::vector<float> samples(FakeAudioRecorder::kFramesPerBuffer, 0.5F);
  const float *channels[] = {samples.data()};
  recorder.deliverInput(channels, static_cast<int>(samples.size()));
}

using NiceRegistry = testing::NiceMock<MockAudioEventHandlerRegistry>;
constexpr int kOneBuffer = static_cast<int>(FakeAudioRecorder::kFramesPerBuffer);

} // namespace

TEST(ActiveRecorderHandleTest, EmptySlotReportsNoRecordingAndStopsNothing) {
  auto handleOwner = ActiveRecorderHandleTestPeer::createHandle();
  ActiveRecorderHandle &handle = *handleOwner;

  EXPECT_EQ(handle.currentState(), RecorderState::Idle);
  EXPECT_FALSE(handle.isRecordingOngoing());
  EXPECT_TRUE(handle.stopAndReturnInfo().is_err());
  EXPECT_EQ(handle.stopAndReturnState(), RecorderState::Idle);
  EXPECT_FALSE(handle.consumeLastRecordingResult().has_value());
}

TEST(ActiveRecorderHandleTest, IdleRecorderIsNotOngoing) {
  auto handleOwner = ActiveRecorderHandleTestPeer::createHandle();
  ActiveRecorderHandle &handle = *handleOwner;
  auto recorder = std::make_shared<FakeAudioRecorder>();
  ASSERT_TRUE(handle.tryStart(recorder).is_ok());
  ASSERT_TRUE(handle.stopAndReturnInfo().is_ok());

  EXPECT_FALSE(handle.isRecordingOngoing());
  EXPECT_TRUE(handle.stopAndReturnInfo().is_err());
  EXPECT_EQ(handle.stopAndReturnState(), RecorderState::Idle);
}

TEST(ActiveRecorderHandleTest, RecordingAndPausedCountAsOngoing) {
  auto handleOwner = ActiveRecorderHandleTestPeer::createHandle();
  ActiveRecorderHandle &handle = *handleOwner;
  auto recorder = std::make_shared<FakeAudioRecorder>();
  ASSERT_TRUE(handle.tryStart(recorder).is_ok());

  EXPECT_EQ(handle.currentState(), RecorderState::Recording);
  EXPECT_TRUE(handle.isRecordingOngoing());

  recorder->pause();
  EXPECT_EQ(handle.currentState(), RecorderState::Paused);
  EXPECT_TRUE(handle.isRecordingOngoing());
}

TEST(ActiveRecorderHandleTest, PauseAndResumeActOnlyInMatchingStates) {
  auto handleOwner = ActiveRecorderHandleTestPeer::createHandle();
  ActiveRecorderHandle &handle = *handleOwner;
  auto recorder = std::make_shared<FakeAudioRecorder>();

  EXPECT_EQ(handle.pause(), RecorderState::Idle);
  EXPECT_EQ(handle.resume(), RecorderState::Idle);

  ASSERT_TRUE(handle.tryStart(recorder).is_ok());
  EXPECT_EQ(handle.resume(), RecorderState::Recording);
  EXPECT_EQ(handle.pause(), RecorderState::Paused);
  EXPECT_TRUE(recorder->isPaused());

  EXPECT_EQ(handle.pause(), RecorderState::Paused);
  EXPECT_EQ(handle.resume(), RecorderState::Recording);
  EXPECT_TRUE(recorder->isRecording());
}

TEST(ActiveRecorderHandleTest, PauseAndResumeWithExpectedIgnoreForeignRecorder) {
  auto handleOwner = ActiveRecorderHandleTestPeer::createHandle();
  ActiveRecorderHandle &handle = *handleOwner;
  auto current = std::make_shared<FakeAudioRecorder>();
  auto other = std::make_shared<FakeAudioRecorder>();
  ASSERT_TRUE(handle.tryStart(current).is_ok());

  EXPECT_EQ(handle.pause(other), RecorderState::Idle);
  EXPECT_TRUE(current->isRecording());

  EXPECT_EQ(handle.pause(current), RecorderState::Paused);
  EXPECT_TRUE(current->isPaused());

  EXPECT_EQ(handle.resume(other), RecorderState::Idle);
  EXPECT_TRUE(current->isPaused());

  EXPECT_EQ(handle.resume(current), RecorderState::Recording);
  EXPECT_TRUE(current->isRecording());
}

TEST(ActiveRecorderHandleTest, PauseAndResumeReachARecorderWhoseStreamIsDown) {
  auto handleOwner = ActiveRecorderHandleTestPeer::createHandle();
  ActiveRecorderHandle &handle = *handleOwner;
  auto recorder = std::make_shared<FakeAudioRecorder>();
  ASSERT_TRUE(handle.tryStart(recorder).is_ok());
  recorder->streamDown = true;

  EXPECT_EQ(handle.pause(), RecorderState::Paused);
  EXPECT_EQ(handle.resume(), RecorderState::Recording);
}

TEST(AudioRecorderPauseTest, PauseLeavesAnIdleOrPausedRecorderUnchanged) {
  auto recorder = std::make_shared<FakeAudioRecorder>();

  recorder->pause();
  EXPECT_EQ(recorder->getState(), RecorderState::Idle);
  recorder->resume();
  EXPECT_EQ(recorder->getState(), RecorderState::Idle);

  ASSERT_TRUE(recorder->start().is_ok());
  recorder->pause();
  recorder->pause();
  EXPECT_EQ(recorder->getState(), RecorderState::Paused);
}

TEST(AudioRecorderPauseTest, InputIsDeliveredOnlyWhileRecording) {
  auto registry = std::make_shared<NiceRegistry>();
  auto recorder = std::make_shared<FakeAudioRecorder>(registry);
  ASSERT_TRUE(recorder->start().is_ok());

  EXPECT_EQ(
      framesDeliveredToCallback(*registry, *recorder, [&] { deliverOneBuffer(*recorder); }),
      kOneBuffer);

  recorder->pause();
  EXPECT_EQ(
      framesDeliveredToCallback(*registry, *recorder, [&] { deliverOneBuffer(*recorder); }), 0);
}

TEST(AudioRecorderPauseTest, PauseMadeWhileTheStreamIsDownHoldsWhenInputReturns) {
  auto handleOwner = ActiveRecorderHandleTestPeer::createHandle();
  ActiveRecorderHandle &handle = *handleOwner;
  auto registry = std::make_shared<NiceRegistry>();
  auto recorder = std::make_shared<FakeAudioRecorder>(registry);
  ASSERT_TRUE(handle.tryStart(recorder).is_ok());

  const int delivered = framesDeliveredToCallback(*registry, *recorder, [&] {
    recorder->streamDown = true;
    EXPECT_EQ(handle.pause(), RecorderState::Paused);
    recorder->streamDown = false;
    deliverOneBuffer(*recorder);
  });

  EXPECT_EQ(delivered, 0);
  EXPECT_EQ(handle.currentState(), RecorderState::Paused);
}

TEST(AudioRecorderPauseTest, ResumeAfterAPauseMadeWhileTheStreamWasDownDeliversInputAgain) {
  auto handleOwner = ActiveRecorderHandleTestPeer::createHandle();
  ActiveRecorderHandle &handle = *handleOwner;
  auto registry = std::make_shared<NiceRegistry>();
  auto recorder = std::make_shared<FakeAudioRecorder>(registry);
  ASSERT_TRUE(handle.tryStart(recorder).is_ok());
  recorder->streamDown = true;
  ASSERT_EQ(handle.pause(), RecorderState::Paused);
  recorder->streamDown = false;

  const int delivered = framesDeliveredToCallback(*registry, *recorder, [&] {
    EXPECT_EQ(handle.resume(), RecorderState::Recording);
    deliverOneBuffer(*recorder);
  });

  EXPECT_EQ(delivered, kOneBuffer);
}

TEST(ActiveRecorderHandleTest, StopStashesResultForSingleConsumption) {
  auto handleOwner = ActiveRecorderHandleTestPeer::createHandle();
  ActiveRecorderHandle &handle = *handleOwner;
  auto recorder = std::make_shared<FakeAudioRecorder>();
  ASSERT_TRUE(handle.tryStart(recorder).is_ok());

  ASSERT_TRUE(handle.stopAndReturnInfo().is_ok());
  EXPECT_FALSE(handle.isRecordingOngoing());

  auto result = handle.consumeLastRecordingResult();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->paths, recorder->stopPaths);
  EXPECT_DOUBLE_EQ(result->size, 1.5);
  EXPECT_DOUBLE_EQ(result->duration, 10.0);

  EXPECT_FALSE(handle.consumeLastRecordingResult().has_value());
}

TEST(ActiveRecorderHandleTest, StopLeavesResultForConsumeLastRecordingResult) {
  auto handleOwner = ActiveRecorderHandleTestPeer::createHandle();
  ActiveRecorderHandle &handle = *handleOwner;
  auto recorder = std::make_shared<FakeAudioRecorder>();
  ASSERT_TRUE(handle.tryStart(recorder).is_ok());

  auto stopResult = handle.stopAndReturnInfo();
  ASSERT_TRUE(stopResult.is_ok());
  EXPECT_EQ(stopResult.unwrap().paths, recorder->stopPaths);
  EXPECT_FALSE(handle.isRecordingOngoing());

  auto consumed = handle.consumeLastRecordingResult();
  ASSERT_TRUE(consumed.has_value());
  EXPECT_EQ(consumed->paths, recorder->stopPaths);
}

TEST(ActiveRecorderHandleTest, StopWithoutFileOutputStashesNothing) {
  auto handleOwner = ActiveRecorderHandleTestPeer::createHandle();
  ActiveRecorderHandle &handle = *handleOwner;
  auto recorder = std::make_shared<FakeAudioRecorder>();
  recorder->stopPaths.clear();
  ASSERT_TRUE(handle.tryStart(recorder).is_ok());

  ASSERT_TRUE(handle.stopAndReturnInfo().is_ok());
  EXPECT_FALSE(handle.consumeLastRecordingResult().has_value());
}

TEST(ActiveRecorderHandleTest, ExpiredRecorderReportsNoRecording) {
  auto handleOwner = ActiveRecorderHandleTestPeer::createHandle();
  ActiveRecorderHandle &handle = *handleOwner;
  {
    auto recorder = std::make_shared<FakeAudioRecorder>();
    ASSERT_TRUE(handle.tryStart(recorder).is_ok());
  }

  EXPECT_FALSE(handle.isRecordingOngoing());
  EXPECT_EQ(handle.pause(), RecorderState::Idle);
  EXPECT_EQ(handle.resume(), RecorderState::Idle);
  EXPECT_EQ(handle.stopAndReturnState(), RecorderState::Idle);
}

TEST(ActiveRecorderHandleTest, StopAndReturnInfoWithExpectedIgnoresForeignRecorder) {
  auto handleOwner = ActiveRecorderHandleTestPeer::createHandle();
  ActiveRecorderHandle &handle = *handleOwner;
  auto current = std::make_shared<FakeAudioRecorder>();
  auto other = std::make_shared<FakeAudioRecorder>();
  ASSERT_TRUE(handle.tryStart(current).is_ok());

  auto result = handle.stopAndReturnInfo(other);
  ASSERT_TRUE(result.is_err());
  EXPECT_EQ(result.unwrap_err(), "Recorder is not in recording state.");
  EXPECT_EQ(handle.currentState(), RecorderState::Recording);
  EXPECT_EQ(current->stopCount, 0);

  ASSERT_TRUE(handle.stopAndReturnInfo(current).is_ok());
  EXPECT_EQ(handle.currentState(), RecorderState::Idle);
  EXPECT_EQ(current->stopCount, 1);
}

TEST(ActiveRecorderHandleTest, TryStartFailsWhenAnotherSessionIsInProgress) {
  auto handleOwner = ActiveRecorderHandleTestPeer::createHandle();
  ActiveRecorderHandle &handle = *handleOwner;
  auto current = std::make_shared<FakeAudioRecorder>();
  auto other = std::make_shared<FakeAudioRecorder>();
  ASSERT_TRUE(handle.tryStart(current).is_ok());

  auto otherResult = handle.tryStart(other);
  ASSERT_TRUE(otherResult.is_err());
  EXPECT_EQ(otherResult.unwrap_err(), "Another recording is already in progress");
  EXPECT_EQ(handle.currentState(), RecorderState::Recording);

  current->pause();
  auto pausedOtherResult = handle.tryStart(other);
  ASSERT_TRUE(pausedOtherResult.is_err());
  EXPECT_EQ(handle.currentState(), RecorderState::Paused);

  ASSERT_TRUE(handle.stopAndReturnInfo(current).is_ok());
  ASSERT_TRUE(handle.tryStart(other).is_ok());
}

TEST(ActiveRecorderHandleTest, TryStartOnSameRecorderReachesStart) {
  auto handleOwner = ActiveRecorderHandleTestPeer::createHandle();
  ActiveRecorderHandle &handle = *handleOwner;
  auto recorder = std::make_shared<FakeAudioRecorder>();
  ASSERT_TRUE(handle.tryStart(recorder).is_ok());

  auto second = handle.tryStart(recorder);
  ASSERT_TRUE(second.is_err());
  EXPECT_EQ(second.unwrap_err(), "Recorder is already recording");
}

TEST(ActiveRecorderHandleTest, TryStartSucceedsWhenPreviousRecorderExpired) {
  auto handleOwner = ActiveRecorderHandleTestPeer::createHandle();
  ActiveRecorderHandle &handle = *handleOwner;
  auto successor = std::make_shared<FakeAudioRecorder>();
  {
    auto expired = std::make_shared<FakeAudioRecorder>();
    ASSERT_TRUE(handle.tryStart(expired).is_ok());
  }

  ASSERT_TRUE(handle.tryStart(successor).is_ok());
}

// Thread startup skew usually serializes a single two-thread run, so the race
// tests below repeat with a fresh handle/recorder and release both threads at
// once through an atomic start flag to actually hit concurrent interleavings.
constexpr int RACE_TEST_ITERATIONS = 200;

TEST(ActiveRecorderHandleTest, ConcurrentStopsCloseTheFileExactlyOnce) {
  for (int iteration = 0; iteration < RACE_TEST_ITERATIONS; ++iteration) {
    auto handleOwner = ActiveRecorderHandleTestPeer::createHandle();
    ActiveRecorderHandle &handle = *handleOwner;
    auto recorder = std::make_shared<FakeAudioRecorder>();
    ASSERT_TRUE(handle.tryStart(recorder).is_ok());

    std::atomic<bool> startFlag{false};
    std::thread nativeStop([&] {
      while (!startFlag.load()) {}
      handle.stopAndReturnState();
    });
    std::thread jsStop([&] {
      while (!startFlag.load()) {}
      handle.stopAndReturnInfo();
    });
    startFlag.store(true);
    nativeStop.join();
    jsStop.join();

    EXPECT_EQ(recorder->stopCount, 1) << "iteration " << iteration;
  }
}

TEST(ActiveRecorderHandleTest, ConcurrentTargetedAndUntargetedStopsCloseTheFileOnce) {
  for (int iteration = 0; iteration < RACE_TEST_ITERATIONS; ++iteration) {
    auto handleOwner = ActiveRecorderHandleTestPeer::createHandle();
    ActiveRecorderHandle &handle = *handleOwner;
    auto recorder = std::make_shared<FakeAudioRecorder>();
    ASSERT_TRUE(handle.tryStart(recorder).is_ok());

    std::atomic<bool> startFlag{false};
    std::thread hostObjectStop([&] {
      while (!startFlag.load()) {}
      handle.stopAndReturnInfo(recorder);
    });
    std::thread nativeStop([&] {
      while (!startFlag.load()) {}
      handle.stopAndReturnState();
    });
    startFlag.store(true);
    hostObjectStop.join();
    nativeStop.join();

    EXPECT_EQ(recorder->stopCount, 1) << "iteration " << iteration;
    EXPECT_EQ(handle.currentState(), RecorderState::Idle) << "iteration " << iteration;
  }
}

TEST(ActiveRecorderHandleTest, ConcurrentTryStartAdmitsOnlyOneRecorder) {
  for (int iteration = 0; iteration < RACE_TEST_ITERATIONS; ++iteration) {
    auto handleOwner = ActiveRecorderHandleTestPeer::createHandle();
    ActiveRecorderHandle &handle = *handleOwner;
    auto first = std::make_shared<FakeAudioRecorder>();
    auto second = std::make_shared<FakeAudioRecorder>();

    std::atomic<bool> startFlag{false};
    std::atomic<int> successes{0};
    std::thread tryFirst([&] {
      while (!startFlag.load()) {}
      if (handle.tryStart(first).is_ok()) {
        successes.fetch_add(1);
      }
    });
    std::thread trySecond([&] {
      while (!startFlag.load()) {}
      if (handle.tryStart(second).is_ok()) {
        successes.fetch_add(1);
      }
    });
    startFlag.store(true);
    tryFirst.join();
    trySecond.join();

    EXPECT_EQ(successes.load(), 1) << "iteration " << iteration;
  }
}

// NOLINTEND
