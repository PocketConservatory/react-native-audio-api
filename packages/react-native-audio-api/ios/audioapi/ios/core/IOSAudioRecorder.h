#pragma once

#import <audioapi/ios/core/NativeAudioRecorder.h>

#include <audioapi/core/inputs/AudioRecorder.h>
#include <audioapi/core/utils/graph/NodeHandle.h>
#include <audioapi/utils/AudioRecorderOptions.h>
#include <audioapi/utils/Macros.h>
#include <audioapi/utils/Result.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace audioapi {

class RecorderCallback;
class RecorderAdapterNode;
class AudioFileProperties;
class IAudioEventHandlerRegistry;
class AudioFileWriter;

class IOSAudioRecorder : public AudioRecorder {
 public:
  explicit IOSAudioRecorder(
      const std::shared_ptr<IAudioEventHandlerRegistry> &audioEventHandlerRegistry,
      const AudioRecorderOptions &options = {});
  ~IOSAudioRecorder() override;

  DELETE_COPY_AND_MOVE(IOSAudioRecorder);

  Result<NoneType, std::string> start() override;
  Result<FileInfo, std::string> stop() override;

  void pause() override;
  void resume() override;
  /// Nothing to do: the session reroutes capture itself and the recorder follows through
  /// the input notifications.
  Result<NoneType, std::string> rerouteInput() override;

  bool isRecording() const override;
  bool isPaused() const override;
  bool isIdle() const override;

  [[nodiscard]] double getInputLatency() const override;

 protected:
  /// The engine resolves the input format asynchronously, so it is read fresh on every call:
  /// between a route change and the engine settling there is no usable format at all.
  [[nodiscard]] Result<StreamFormat, std::string> resolveStreamFormat() const override;

  NativeAudioRecorder *nativeRecorder_;

 private:
  /// Re-points the live outputs at the format the rebuilt engine input now delivers, with the
  /// input disarmed meanwhile so no buffer in either format slips through.
  Result<NoneType, std::string> reprepareForLiveInput();
  void handleInputNotification(AudioEngineInputNotification notification);
  /// Re-points the outputs at the live input format and arms the input while Recording. Apart
  /// from `start()`, which prepares the outputs against the format it has just read, this is the
  /// only place that arms the input, so it is armed only while the outputs match that format.
  void handleHardwareChange();

  int32_t inputChannelCount_{0};
  /// Serializes `handleHardwareChange()`: the engine may notify the same rebuilt graph from two
  /// threads and starts it once either call returns, so a returning call must find the outputs
  /// already re-prepared for the live format. Taken after the `isIdle()` check, so `start()`,
  /// which holds the consumer mutexes while its engine start notifies the idle recorder, never
  /// waits on it.
  std::mutex hardwareChangeMutex_;
};

} // namespace audioapi
