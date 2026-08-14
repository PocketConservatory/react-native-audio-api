#include <audioapi/core/inputs/AudioRecorder.h>
#include <audioapi/core/inputs/AudioRecorderRegistry.h>
#include <gtest/gtest.h>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using namespace audioapi;

// NOLINTBEGIN

namespace {

// Emulates the real recorders' single-flight stop contract the registry relies
// on: only the caller that transitions the state out of Recording/Paused
// finalizes; a concurrent loser gets an inert Err.
class FakeRecorder : public AudioRecorder {
 public:
  FakeRecorder() : AudioRecorder(nullptr) {}

  std::atomic<int> stopCalls{0};
  std::atomic<int> effectiveStops{0};

  Result<NoneType, std::string> start(const std::string & /*fileNameOverride*/) override {
    state_.store(RecorderState::Recording, std::memory_order_release);
    return Result<NoneType, std::string>::Ok(None);
  }

  Result<std::tuple<std::vector<std::string>, double, double>, std::string> stop() override {
    stopCalls.fetch_add(1, std::memory_order_relaxed);
    if (state_.exchange(RecorderState::Idle, std::memory_order_acq_rel) == RecorderState::Idle) {
      return Result<std::tuple<std::vector<std::string>, double, double>, std::string>::Err(
          "Recorder is not in recording state.");
    }
    effectiveStops.fetch_add(1, std::memory_order_relaxed);
    return Result<std::tuple<std::vector<std::string>, double, double>, std::string>::Ok(
        std::make_tuple(std::vector<std::string>{}, 0.0, 0.0));
  }

  Result<NoneType, std::string> enableFileOutput(
      std::shared_ptr<AudioFileProperties> /*properties*/) override {
    return Result<NoneType, std::string>::Ok(None);
  }
  void disableFileOutput() override {}

  void pause() override {
    state_.store(RecorderState::Paused, std::memory_order_release);
  }
  void resume() override {
    state_.store(RecorderState::Recording, std::memory_order_release);
  }

  void connect(const std::shared_ptr<RecorderAdapterNode> & /*node*/) override {}
  void disconnect() override {}

  Result<NoneType, std::string> setOnAudioReadyCallback(
      float /*sampleRate*/,
      size_t /*bufferLength*/,
      int /*channelCount*/,
      uint64_t /*callbackId*/) override {
    return Result<NoneType, std::string>::Ok(None);
  }
  void clearOnAudioReadyCallback() override {}

  bool isRecording() const override {
    return state_.load(std::memory_order_acquire) == RecorderState::Recording;
  }
  bool isPaused() const override {
    return state_.load(std::memory_order_acquire) == RecorderState::Paused;
  }
  bool isIdle() const override {
    return state_.load(std::memory_order_acquire) == RecorderState::Idle;
  }
};

} // namespace

TEST(AudioRecorderRegistryTest, StopsActiveAndSkipsIdleRecorders) {
  auto recording = std::make_shared<FakeRecorder>();
  auto paused = std::make_shared<FakeRecorder>();
  auto idle = std::make_shared<FakeRecorder>();
  (void)recording->start("");
  (void)paused->start("");
  paused->pause();

  AudioRecorderRegistry::registerRecorder(recording);
  AudioRecorderRegistry::registerRecorder(paused);
  AudioRecorderRegistry::registerRecorder(idle);

  AudioRecorderRegistry::stopAllActiveRecordings();

  EXPECT_EQ(recording->effectiveStops.load(), 1);
  EXPECT_EQ(paused->effectiveStops.load(), 1);
  EXPECT_EQ(idle->stopCalls.load(), 0);
  EXPECT_TRUE(recording->isIdle());
  EXPECT_TRUE(paused->isIdle());
}

TEST(AudioRecorderRegistryTest, StopAllIsIdempotent) {
  auto recorder = std::make_shared<FakeRecorder>();
  (void)recorder->start("");
  AudioRecorderRegistry::registerRecorder(recorder);

  AudioRecorderRegistry::stopAllActiveRecordings();
  AudioRecorderRegistry::stopAllActiveRecordings();

  EXPECT_EQ(recorder->effectiveStops.load(), 1);
}

TEST(AudioRecorderRegistryTest, SurvivesDestroyedRecorders) {
  {
    auto shortLived = std::make_shared<FakeRecorder>();
    (void)shortLived->start("");
    AudioRecorderRegistry::registerRecorder(shortLived);
  }
  auto alive = std::make_shared<FakeRecorder>();
  (void)alive->start("");
  AudioRecorderRegistry::registerRecorder(alive);

  AudioRecorderRegistry::stopAllActiveRecordings();

  EXPECT_EQ(alive->effectiveStops.load(), 1);
}

TEST(AudioRecorderRegistryTest, StopAllRacingConcurrentStopYieldsOneWinner) {
  constexpr int kIterations = 200;
  for (int i = 0; i < kIterations; ++i) {
    auto recorder = std::make_shared<FakeRecorder>();
    (void)recorder->start("");
    AudioRecorderRegistry::registerRecorder(recorder);

    std::thread jsStop([&recorder] { (void)recorder->stop(); });
    AudioRecorderRegistry::stopAllActiveRecordings();
    jsStop.join();

    EXPECT_EQ(recorder->effectiveStops.load(), 1);
    EXPECT_TRUE(recorder->isIdle());
  }
}

// NOLINTEND
