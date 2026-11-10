#pragma once

#import <audioapi/ios/core/NativeAudioRecorder.h>
#include <mach/mach_time.h>

#include <audioapi/core/inputs/AudioRecorder.h>
#include <audioapi/core/utils/InputRateMonitor.h>
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
  /// Serial queue the audio thread hands measured rate corrections to.
  dispatch_queue_t rateCorrectionQueue_;

 private:
  /// Re-points the live outputs at the format the rebuilt engine input now delivers, with the
  /// input disarmed meanwhile so no buffer in either format slips through.
  Result<NoneType, std::string> reprepareForLiveInput();
  void handleInputNotification(AudioEngineInputNotification notification);
  void handleHardwareChange();
  /// Re-points the outputs at the live input format and arms the input while Recording. This is
  /// the only place that arms the input outside a re-prepare, so it is armed only while the
  /// outputs match that format. Reports a sample-rate change with @p reason. The caller must hold
  /// `hardwareChangeMutex_`.
  void applyLiveInputFormatLocked(InputRateChangeReason reason);
  /// Reads the input at the standard rate the audio thread measured, unless the label it was
  /// measured against has since changed or the take has used up its corrections.
  void handleMeasuredRateMismatch(double sampleRate, uint32_t labelGeneration);
  /// Audio thread. Feeds the rate monitor and hands a mismatch to `rateCorrectionQueue_`.
  void monitorInputRate(const AudioTimeStamp *timestamp, int numFrames);

  static constexpr int MAX_MEASURED_CORRECTIONS_PER_TAKE = 3;

  int32_t inputChannelCount_{0};
  /// Serializes `handleHardwareChange()`, `handleMeasuredRateMismatch()`, `start()` and `stop()`.
  /// The engine may notify the same rebuilt graph from two threads and starts it once either
  /// call returns, so a returning call must find the outputs already re-prepared for the live
  /// format; a call for a session that has since stopped must finish before a new session
  /// prepares its outputs; and none may re-arm a session `stop()` has stopped. `start()` reads
  /// the live format again under it once the session is Recording, so a change the handler
  /// dropped while the session was still idle is not lost. Lock order: this mutex, then the
  /// consumer mutexes, then `_engineLock` (briefly). The handler takes it only after its
  /// `isIdle()` check, and `stop()` only after storing Idle, so the notification the engine
  /// sends on the same thread while `start()` or `stop()` runs returns before reaching it.
  std::mutex hardwareChangeMutex_;
  /// Owned by the audio thread; control threads only publish its label.
  InputRateMonitor inputRateMonitor_;
  /// Set by the audio thread while a correction is queued, so at most one is in flight.
  std::atomic<bool> rateCorrectionPending_{false};
  /// Guarded by `hardwareChangeMutex_`.
  int measuredCorrectionCount_{0};
  /// Cleared by the destructor; a queued correction that finds it cleared does not touch the
  /// recorder.
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
  mach_timebase_info_data_t hostTimebase_{};
};

} // namespace audioapi
