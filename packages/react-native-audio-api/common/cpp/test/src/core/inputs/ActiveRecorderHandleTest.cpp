#include <audioapi/core/inputs/ActiveRecorderHandle.h>
#include <audioapi/core/inputs/AudioRecorder.h>
#include <gtest/gtest.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
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
  FakeAudioRecorder() : AudioRecorder(nullptr) {}

  std::vector<std::string> stopPaths{"file:///tmp/recording.m4a"};
  std::atomic<int> stopCount{0};

  // Mirrors AndroidAudioRecorder: the id is published before the stream starts and
  // cleared when the start fails or the session stops.
  int32_t streamSessionId = 0;
  bool failStart = false;
  std::function<void()> onStreamStarting;
  std::atomic<int32_t> sessionId{0};

  Result<NoneType, std::string> start(const std::string &, RecorderInputPreset) override {
    if (state_ != RecorderState::Idle) {
      return Err(std::string("Recorder is already recording"));
    }
    sessionId = streamSessionId;
    if (onStreamStarting) {
      onStreamStarting();
    }
    if (failStart) {
      sessionId = 0;
      return Err(std::string("Failed to start stream"));
    }
    state_ = RecorderState::Recording;
    return Ok(None);
  }

  [[nodiscard]] int32_t audioSessionId() const override {
    return sessionId;
  }

  // Mirrors AndroidAudioRecorder::stop(): under its locks exactly one caller
  // transitions out of a non-idle state and closes the file; the loser errs.
  StopResult stop() override {
    if (state_.exchange(RecorderState::Idle) == RecorderState::Idle) {
      return Err(std::string("Recorder is not in recording state."));
    }
    sessionId = 0;
    stopCount += 1;
    return Ok(std::make_tuple(stopPaths, 1.5, 10.0));
  }

  Result<NoneType, std::string> enableFileOutput(std::shared_ptr<AudioFileProperties>) override {
    return Ok(None);
  }

  // Mirrors the platform recorders: pause() goes through the shared markPaused(), and
  // isRecording() also requires a live stream, which a reconnect takes away.
  bool streamReconnecting = false;
  std::vector<int32_t> reopenRequests;

  void pause() override {
    markPaused();
  }
  void resume() override {
    if (state_ == RecorderState::Paused) {
      state_ = RecorderState::Recording;
    }
  }

  void reopenInputStream(int32_t expectedSessionId) override {
    reopenRequests.push_back(expectedSessionId);
  }

  void connect(const std::shared_ptr<RecorderAdapterNode> &) override {}

  Result<NoneType, std::string> setOnAudioReadyCallback(float, size_t, int, uint64_t) override {
    return Ok(None);
  }

  bool isRecording() const override {
    return state_ == RecorderState::Recording && !streamReconnecting;
  }
  bool isPaused() const override {
    return state_ == RecorderState::Paused;
  }
};

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
  ASSERT_TRUE(handle.tryStart(recorder, "").is_ok());
  ASSERT_TRUE(handle.stopAndReturnInfo().is_ok());

  EXPECT_FALSE(handle.isRecordingOngoing());
  EXPECT_TRUE(handle.stopAndReturnInfo().is_err());
  EXPECT_EQ(handle.stopAndReturnState(), RecorderState::Idle);
}

TEST(ActiveRecorderHandleTest, RecordingAndPausedCountAsOngoing) {
  auto handleOwner = ActiveRecorderHandleTestPeer::createHandle();
  ActiveRecorderHandle &handle = *handleOwner;
  auto recorder = std::make_shared<FakeAudioRecorder>();
  ASSERT_TRUE(handle.tryStart(recorder, "").is_ok());

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

  ASSERT_TRUE(handle.tryStart(recorder, "").is_ok());
  EXPECT_EQ(handle.resume(), RecorderState::Recording);
  EXPECT_EQ(handle.pause(), RecorderState::Paused);
  EXPECT_TRUE(recorder->isPaused());

  EXPECT_EQ(handle.pause(), RecorderState::Paused);
  EXPECT_EQ(handle.resume(), RecorderState::Recording);
  EXPECT_TRUE(recorder->isRecording());
}

TEST(ActiveRecorderHandleTest, PauseAndResumeReachARecorderWhoseStreamIsReconnecting) {
  auto handleOwner = ActiveRecorderHandleTestPeer::createHandle();
  ActiveRecorderHandle &handle = *handleOwner;
  auto recorder = std::make_shared<FakeAudioRecorder>();
  ASSERT_TRUE(handle.tryStart(recorder, "").is_ok());
  recorder->streamReconnecting = true;

  EXPECT_EQ(handle.pause(), RecorderState::Paused);
  EXPECT_EQ(handle.resume(), RecorderState::Recording);
}

TEST(ActiveRecorderHandleTest, ReopenInputStreamPassesTheExpectedSessionToAnActiveSession) {
  auto handleOwner = ActiveRecorderHandleTestPeer::createHandle();
  ActiveRecorderHandle &handle = *handleOwner;
  auto recorder = std::make_shared<FakeAudioRecorder>();
  recorder->streamSessionId = 41;

  handle.reopenInputStream(41);
  ASSERT_TRUE(handle.tryStart(recorder, "").is_ok());
  handle.reopenInputStream(41);
  handle.pause();
  handle.reopenInputStream(7);
  EXPECT_EQ(recorder->reopenRequests, (std::vector<int32_t>{41, 7}));

  ASSERT_TRUE(handle.stopAndReturnInfo().is_ok());
  handle.reopenInputStream(41);
  EXPECT_EQ(recorder->reopenRequests.size(), 2u);
}

TEST(ActiveRecorderHandleTest, StopStashesResultForSingleConsumption) {
  auto handleOwner = ActiveRecorderHandleTestPeer::createHandle();
  ActiveRecorderHandle &handle = *handleOwner;
  auto recorder = std::make_shared<FakeAudioRecorder>();
  ASSERT_TRUE(handle.tryStart(recorder, "").is_ok());

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
  ASSERT_TRUE(handle.tryStart(recorder, "").is_ok());

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
  ASSERT_TRUE(handle.tryStart(recorder, "").is_ok());

  ASSERT_TRUE(handle.stopAndReturnInfo().is_ok());
  EXPECT_FALSE(handle.consumeLastRecordingResult().has_value());
}

TEST(ActiveRecorderHandleTest, ExpiredRecorderReportsNoRecording) {
  auto handleOwner = ActiveRecorderHandleTestPeer::createHandle();
  ActiveRecorderHandle &handle = *handleOwner;
  {
    auto recorder = std::make_shared<FakeAudioRecorder>();
    ASSERT_TRUE(handle.tryStart(recorder, "").is_ok());
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
  ASSERT_TRUE(handle.tryStart(current, "").is_ok());

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
  ASSERT_TRUE(handle.tryStart(current, "").is_ok());

  auto otherResult = handle.tryStart(other, "");
  ASSERT_TRUE(otherResult.is_err());
  EXPECT_EQ(otherResult.unwrap_err(), "Another recording is already in progress");
  EXPECT_EQ(handle.currentState(), RecorderState::Recording);

  current->pause();
  auto pausedOtherResult = handle.tryStart(other, "");
  ASSERT_TRUE(pausedOtherResult.is_err());
  EXPECT_EQ(handle.currentState(), RecorderState::Paused);

  ASSERT_TRUE(handle.stopAndReturnInfo(current).is_ok());
  ASSERT_TRUE(handle.tryStart(other, "").is_ok());
}

TEST(ActiveRecorderHandleTest, TryStartOnSameRecorderReachesStart) {
  auto handleOwner = ActiveRecorderHandleTestPeer::createHandle();
  ActiveRecorderHandle &handle = *handleOwner;
  auto recorder = std::make_shared<FakeAudioRecorder>();
  ASSERT_TRUE(handle.tryStart(recorder, "").is_ok());

  auto second = handle.tryStart(recorder, "");
  ASSERT_TRUE(second.is_err());
  EXPECT_EQ(second.unwrap_err(), "Recorder is already recording");
}

TEST(ActiveRecorderHandleTest, TryStartSucceedsWhenPreviousRecorderExpired) {
  auto handleOwner = ActiveRecorderHandleTestPeer::createHandle();
  ActiveRecorderHandle &handle = *handleOwner;
  auto successor = std::make_shared<FakeAudioRecorder>();
  {
    auto expired = std::make_shared<FakeAudioRecorder>();
    ASSERT_TRUE(handle.tryStart(expired, "").is_ok());
  }

  ASSERT_TRUE(handle.tryStart(successor, "").is_ok());
}

TEST(ActiveRecorderHandleTest, SessionIdIsReadableWhileTryStartIsStarting) {
  auto handleOwner = ActiveRecorderHandleTestPeer::createHandle();
  ActiveRecorderHandle &handle = *handleOwner;
  auto recorder = std::make_shared<FakeAudioRecorder>();
  recorder->streamSessionId = 42;
  int32_t idSeenWhileStarting = 0;
  // The slot is assigned only after start() returns; an observer reacting to the stream
  // starting must already find its id.
  recorder->onStreamStarting = [&] {
    idSeenWhileStarting = handle.audioSessionId();
  };

  ASSERT_TRUE(handle.tryStart(recorder, "").is_ok());
  EXPECT_EQ(idSeenWhileStarting, 42);
  EXPECT_EQ(handle.audioSessionId(), 42);

  ASSERT_TRUE(handle.stopAndReturnInfo().is_ok());
  EXPECT_EQ(handle.audioSessionId(), 0);
}

TEST(ActiveRecorderHandleTest, SessionIdReadDoesNotWaitOnTheHandleMutex) {
  auto handleOwner = ActiveRecorderHandleTestPeer::createHandle();
  ActiveRecorderHandle &handle = *handleOwner;
  auto recorder = std::make_shared<FakeAudioRecorder>();
  recorder->streamSessionId = 7;
  int32_t idSeenFromAnotherThread = 0;
  // tryStart() holds the handle mutex across start(); a reader on another thread
  // (the Android main looper) must not block on it.
  recorder->onStreamStarting = [&] {
    std::thread reader([&] { idSeenFromAnotherThread = handle.audioSessionId(); });
    reader.join();
  };

  ASSERT_TRUE(handle.tryStart(recorder, "").is_ok());
  EXPECT_EQ(idSeenFromAnotherThread, 7);
}

TEST(ActiveRecorderHandleTest, FailedTryStartRestoresThePreviousSessionSource) {
  auto handleOwner = ActiveRecorderHandleTestPeer::createHandle();
  ActiveRecorderHandle &handle = *handleOwner;
  auto current = std::make_shared<FakeAudioRecorder>();
  current->streamSessionId = 5;
  ASSERT_TRUE(handle.tryStart(current, "").is_ok());
  current->pause();
  current->stop();

  auto failing = std::make_shared<FakeAudioRecorder>();
  failing->streamSessionId = 9;
  failing->failStart = true;
  ASSERT_TRUE(handle.tryStart(failing, "").is_err());
  // A failed candidate that still reports an id must not stay the source.
  failing->sessionId = 9;
  EXPECT_EQ(handle.audioSessionId(), 0);

  current->streamSessionId = 6;
  ASSERT_TRUE(handle.tryStart(current, "").is_ok());
  EXPECT_EQ(handle.audioSessionId(), 6);
}

TEST(ActiveRecorderHandleTest, ExpiredRecorderReportsNoSessionId) {
  auto handleOwner = ActiveRecorderHandleTestPeer::createHandle();
  ActiveRecorderHandle &handle = *handleOwner;
  {
    auto expired = std::make_shared<FakeAudioRecorder>();
    expired->streamSessionId = 3;
    ASSERT_TRUE(handle.tryStart(expired, "").is_ok());
    EXPECT_EQ(handle.audioSessionId(), 3);
  }

  EXPECT_EQ(handle.audioSessionId(), 0);
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
    ASSERT_TRUE(handle.tryStart(recorder, "").is_ok());

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
    ASSERT_TRUE(handle.tryStart(recorder, "").is_ok());

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
      if (handle.tryStart(first, "").is_ok()) {
        successes.fetch_add(1);
      }
    });
    std::thread trySecond([&] {
      while (!startFlag.load()) {}
      if (handle.tryStart(second, "").is_ok()) {
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
