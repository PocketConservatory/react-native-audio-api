#pragma once

#include <audioapi/android/core/NativeAudioRecorder.hpp>
#include <audioapi/core/inputs/AudioRecorder.h>
#include <audioapi/utils/AudioArray.hpp>
#include <audioapi/utils/AudioBuffer.hpp>
#include <audioapi/utils/Result.hpp>
#include <oboe/Oboe.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace audioapi {

class AudioFileProperties;
class AndroidRecorderCallback;
class AndroidFileWriterBackend;
class AudioEventHandlerRegistry;

class AndroidAudioRecorder : public oboe::AudioStreamCallback,
                             public AudioRecorder,
                             public std::enable_shared_from_this<AndroidAudioRecorder> {
 public:
  explicit AndroidAudioRecorder(
      const std::shared_ptr<AudioEventHandlerRegistry> &audioEventHandlerRegistry);
  ~AndroidAudioRecorder() override;
  void cleanup();

  Result<NoneType, std::string> start(
      const std::string &fileNameOverride,
      RecorderInputPreset inputPreset) override;
  StopResult stop() override;

  Result<NoneType, std::string> enableFileOutput(
      std::shared_ptr<AudioFileProperties> properties) override;

  void pause() override;
  void resume() override;
  bool isRecording() const override;
  bool isPaused() const override;

  Result<NoneType, std::string> setOnAudioReadyCallback(
      float sampleRate,
      size_t bufferLength,
      int channelCount,
      uint64_t callbackId) override;

  void connect(const std::shared_ptr<RecorderAdapterNode> &node) override;

  oboe::DataCallbackResult
  onAudioReady(oboe::AudioStream *oboeStream, void *audioData, int32_t numFrames) override;
  void onErrorAfterClose(oboe::AudioStream *oboeStream, oboe::Result error) override;

  /// @brief Matches `AudioRecordingConfiguration.getClientAudioSessionId()` of this recorder's
  /// input stream. Non-zero from just before the stream is started until the session stops; a
  /// disconnect reclaim keeps the old id until the reopened stream's id replaces it.
  [[nodiscard]] int32_t audioSessionId() const override;

  void reopenInputStream(int32_t expectedSessionId) override;

 protected:
  void clearAdapterScratchState() override;

 private:
  std::shared_ptr<AudioBuffer> deinterleavingBuffer_;

  float streamSampleRate_;
  int32_t streamChannelCount_;
  int32_t streamMaxBufferSizeInFrames_;

  facebook::jni::global_ref<NativeAudioRecorder> nativeAudioRecorder_;

  std::shared_ptr<oboe::AudioStream> mStream_;
  /// Input preset of the current session, set by start() and used by every stream it opens,
  /// including the disconnect reclaim and reopenInputStream(). Guarded by the recorder locks.
  RecorderInputPreset inputPreset_ = RecorderInputPreset::VoiceRecognition;
  /// Written under the recorder locks, read lock-free through audioSessionId().
  std::atomic<int32_t> sessionId_{0};
  void publishSessionId();

  std::vector<std::string> recordingSegmentPaths_;
  Result<NoneType, std::string> openAudioStream();
  Result<NoneType, std::string> openStream(std::shared_ptr<oboe::AudioStream> &stream);
  void adoptStreamProperties();
  std::shared_ptr<AudioFileWriter> createFileWriter(
      const std::shared_ptr<AudioFileProperties> &props);
  Result<NoneType, std::string> setupFileWriter(
      const std::shared_ptr<AudioFileProperties> &properties,
      const std::string &fileNameOverride = "");
};

} // namespace audioapi
