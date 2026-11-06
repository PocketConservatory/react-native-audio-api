#include <android/log.h>
#include <audioapi/android/core/AndroidAudioRecorder.h>
#include <audioapi/android/core/utils/AndroidFileWriterBackend.h>
#include <audioapi/android/core/utils/AndroidRecorderCallback.h>

#if !RN_AUDIO_API_FFMPEG_DISABLED
#include <audioapi/android/core/utils/ffmpegBackend/FFmpegFileWriter.h>
#endif // RN_AUDIO_API_FFMPEG_DISABLED

#include <audioapi/android/core/utils/AndroidRotatingFileWriter.h>
#include <audioapi/android/core/utils/miniaudioBackend/MiniAudioFileWriter.h>
#include <audioapi/core/sources/RecorderAdapterNode.h>
#include <audioapi/core/utils/Constants.h>
#include <audioapi/core/utils/Locker.h>
#include <audioapi/events/AudioEventHandlerRegistry.h>
#include <audioapi/utils/AudioFileProperties.h>
#include <audioapi/utils/CircularArray.hpp>
#include <audioapi/utils/CircularOverflowableAudioArray.h>

#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace audioapi {

namespace {

oboe::InputPreset toOboeInputPreset(RecorderInputPreset preset) {
  switch (preset) {
    case RecorderInputPreset::Generic:
      return oboe::InputPreset::Generic;
    case RecorderInputPreset::Camcorder:
      return oboe::InputPreset::Camcorder;
    case RecorderInputPreset::VoiceCommunication:
      return oboe::InputPreset::VoiceCommunication;
    case RecorderInputPreset::Unprocessed:
      return oboe::InputPreset::Unprocessed;
    case RecorderInputPreset::VoicePerformance:
      return oboe::InputPreset::VoicePerformance;
    case RecorderInputPreset::VoiceRecognition:
      return oboe::InputPreset::VoiceRecognition;
  }
  return oboe::InputPreset::VoiceRecognition;
}

} // namespace

AndroidAudioRecorder::AndroidAudioRecorder(
    const std::shared_ptr<AudioEventHandlerRegistry> &audioEventHandlerRegistry)
    : AudioRecorder(audioEventHandlerRegistry),
      streamSampleRate_(0.0),
      streamChannelCount_(0),
      streamMaxBufferSizeInFrames_(0) {}

/// @brief Destructor ensures that the audio stream and each output type are closed and flushed up remaining data.
/// callable from the JS thread or handled by audio thread (if js dropped recorder first).
AndroidAudioRecorder::~AndroidAudioRecorder() {
  // there is no need to lock here, as there could be two threads that can destruct js gc and audio thread one (or one created by it)
  // if we are on js:
  // audio thread dropped recorder so onAudioReady callback would not be called anymore
  //
  // if we are on audio thread:
  // js dropped recorder and oboe states that "callback object cannot be deleted before the stream is deleted"
  if (fileWriter_ != nullptr) {
    fileWriter_->closeFile();
  }
  if (dataCallback_ != nullptr) {
    dataCallback_->cleanup();
  }
  if (adapterNode_ != nullptr) {
    adapterNode_->adapterCleanup();
  }
  // oboe could be handling stopping and closing the stream, sanity check just in case
  if (mStream_ != nullptr) {
    mStream_->requestStop();
    mStream_->close();
    mStream_.reset();
  }
}

/// @brief Creates and opens the Oboe audio input stream for recording.
/// calculates the "native" or hardware stream parameters for other interfaces
/// to use.
/// Callable from the JS thread only.
/// @returns Success status or Error status with message.
Result<NoneType, std::string> AndroidAudioRecorder::openAudioStream() {
  if (mStream_ != nullptr) {
    return Result<NoneType, std::string>::Ok(None);
  }

  auto result = openStream(mStream_);
  if (result.is_ok()) {
    adoptStreamProperties();
  }
  return result;
}

/// @brief Builds and opens an input stream into @p stream without starting it, with the session's
/// input preset. Touches no other recorder state.
Result<NoneType, std::string> AndroidAudioRecorder::openStream(
    std::shared_ptr<oboe::AudioStream> &stream) {
  // Shared with no performance mode keeps the input off MMAP. On that path the system silences
  // this capture when another client takes the microphone and reports it through
  // AudioRecordingConfiguration.isClientSilenced() (RecordingSilenceListener); an MMAP stream is
  // disconnected instead. LowLatency can still be served by a shared MMAP stream, so Shared alone
  // isn't enough. The cost is higher input latency than LowLatency. The allocated session id
  // identifies this stream's recording configuration.
  oboe::AudioStreamBuilder builder;
  builder.setSharingMode(oboe::SharingMode::Shared)
      ->setDirection(oboe::Direction::Input)
      ->setFormat(oboe::AudioFormat::Float)
      ->setFormatConversionAllowed(true)
      ->setPerformanceMode(oboe::PerformanceMode::None)
      ->setSessionId(oboe::SessionId::Allocate)
      ->setInputPreset(toOboeInputPreset(inputPreset_))
      ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium)
      ->setDataCallback(shared_from_this())
      ->setErrorCallback(shared_from_this());

  auto result = builder.openStream(stream);

  if (result != oboe::Result::OK || stream == nullptr) {
    stream.reset();
    return Result<NoneType, std::string>::Err(
        "Failed to open audio stream: " + std::string(oboe::convertToText(result)));
  }

  return Result<NoneType, std::string>::Ok(None);
}

void AndroidAudioRecorder::adoptStreamProperties() {
  streamSampleRate_ = static_cast<float>(mStream_->getSampleRate());
  streamChannelCount_ = mStream_->getChannelCount();
  streamMaxBufferSizeInFrames_ = mStream_->getBufferSizeInFrames();
}

/// @brief prepares and starts the audio recording process.
/// If audio stream is opened correctly, it will set up any output configured
/// (file writing, callback, adapter node) and start the stream.
/// Invoked asynchronously from the PromiseVendor thread pool (not the JS thread);
/// all shared state is guarded by the recorder locks and atomics.
/// NOTE: I've noticed some possibly invalid file paths being returned on Android,
/// RN side requires their "file://" prefix, but sometimes it returned raw path.
/// Most likely this was due to alpha version mistakes, but in case of problems leaving this here. (ㆆ _ ㆆ)
/// @returns On success, returns the file URI where the recording is being saved (if file output is enabled).
Result<NoneType, std::string> AndroidAudioRecorder::start(
    const std::string &fileNameOverride,
    RecorderInputPreset inputPreset) {
  std::scoped_lock startLock(callbackMutex_, fileWriterMutex_, adapterNodeMutex_);

  if (!isIdle()) {
    return Result<NoneType, std::string>::Err("Recorder is already recording");
  }

  cancelPendingReclaim();
  reclaimBackoff_.reset();
  clearInterruptionCauses();
  inputPreset_ = inputPreset;
  auto streamResult = openAudioStream();

  if (!streamResult.is_ok()) {
    return Result<NoneType, std::string>::Err(streamResult.unwrap_err());
  }

  if (mStream_ == nullptr) {
    return Result<NoneType, std::string>::Err("Audio stream is not initialized.");
  }

  if (wantsFileOutput()) {
    recordingSegmentPaths_.clear();
    auto writerResult = setupFileWriter(fileProperties_, fileNameOverride);
    if (!writerResult.is_ok()) {
      return writerResult;
    }
    __android_log_print(
        ANDROID_LOG_INFO,
        "AndroidAudioRecorder",
        "File created successfully at path: %s",
        filePath_.c_str());
  }

  if (wantsCallback()) {
    if (dataCallback_ == nullptr) {
      dataCallback_ = std::make_shared<AndroidRecorderCallback>(
          audioEventHandlerRegistry_,
          dataCallbackMetadata_.sampleRate,
          dataCallbackMetadata_.bufferLength,
          dataCallbackMetadata_.channelCount,
          dataCallbackMetadata_.callbackId);

      dataCallback_->setOnErrorCallback(errorCallbackId_.load(std::memory_order_acquire));
    }

    std::static_pointer_cast<AndroidRecorderCallback>(dataCallback_)
        ->prepare(streamSampleRate_, streamChannelCount_, streamMaxBufferSizeInFrames_);
    callbackOutputConfigured_.store(true, std::memory_order_release);
  }

  if (wantsConnection() && adapterNode_ != nullptr) {
    deinterleavingBuffer_ = std::make_shared<AudioBuffer>(
        streamMaxBufferSizeInFrames_, streamChannelCount_, streamSampleRate_);
    adapterNode_->init(streamMaxBufferSizeInFrames_, streamChannelCount_, streamSampleRate_);
    connectedConfigured_.store(true, std::memory_order_release);
  }

  // Published before requestStart() so an observer of the recording configurations can match
  // a stream that starts already silenced.
  publishSessionId();
  auto result = mStream_->requestStart();

  if (result != oboe::Result::OK) {
    sessionId_.store(0, std::memory_order_release);
    return Result<NoneType, std::string>::Err(
        "Failed to start stream: " + std::string(oboe::convertToText(result)));
  }

  markStreamStarted();
  state_.store(RecorderState::Recording, std::memory_order_release);
  return Result<NoneType, std::string>::Ok(None);
}

/// @brief Stops the audio stream and finalizes any output (file writing, callback, adapter node).
/// Invoked asynchronously from the PromiseVendor thread pool (not the JS thread);
/// all shared state is guarded by the recorder locks and atomics.
/// @returns On success, returns the file URI, size in MB and duration in seconds of the recorded file (if file output is enabled).
/// NOTE: due to the file access nature on Android, the size might sometimes be zeroed (really long files).
AudioRecorder::StopResult AndroidAudioRecorder::stop() {
  std::shared_ptr<AudioFileWriter> fileWriter;
  std::shared_ptr<AudioRecorderCallback> dataCallback;
  std::shared_ptr<RecorderAdapterNode> adapterNode;
  std::vector<std::string> outputPaths;

  {
    std::scoped_lock stopLock(callbackMutex_, fileWriterMutex_, adapterNodeMutex_);

    if (isIdle()) {
      return StopResult::Err("Recorder is not in recording state.");
    }

    state_.store(RecorderState::Idle, std::memory_order_release);
    cancelPendingReclaim();
    reclaimBackoff_.reset();
    clearInterruptionCauses();

    // Fully close the stream rather than just stopping it. A stopped-but-open
    // stream keeps the data/error callbacks registered, so a later device
    // disconnect fires onErrorAfterClose and resurrects the stream (the mic
    // stays hot after stop()), and the stream's shared_ptr to this recorder
    // keeps the recorder alive forever once JS drops it (reference cycle).
    // start() reopens the stream when mStream_ is null.
    closeStream();
    sessionId_.store(0, std::memory_order_release);

    if (usesFileOutput()) {
      fileOutputConfigured_.store(false, std::memory_order_release);
      fileWriter = std::move(fileWriter_);
      outputPaths = buildOutputPaths(recordingSegmentPaths_, filePath_);
    }

    if (usesCallback()) {
      callbackOutputConfigured_.store(false, std::memory_order_release);
      dataCallback = std::move(dataCallback_);
    }

    if (isConnected()) {
      connectedConfigured_.store(false, std::memory_order_release);
      isConnected_.store(false, std::memory_order_release);
      deinterleavingBuffer_ = nullptr;
      adapterNode = std::move(adapterNode_);
    }

    recordingSegmentPaths_.clear();
    filePath_ = "";
  }

  return finalizeStoppedOutputs(fileWriter, dataCallback, adapterNode, std::move(outputPaths));
}

/// @brief Enables file output for the recorder with the specified properties.
/// The file itself is created by the next start(). An active (recording or paused) session keeps
/// the output it started with, so calling this during a session fails and changes nothing.
/// This method should be called from the JS thread only.
/// @param properties Properties defining the audio file format and encoding options.
/// @returns Ok when the properties were stored, otherwise an error message.
Result<NoneType, std::string> AndroidAudioRecorder::enableFileOutput(
    std::shared_ptr<AudioFileProperties> properties) {
  // start() holds fileWriterMutex_ for its whole idle check and setup, so the
  // idle test below cannot interleave with a session that is just starting.
  std::scoped_lock lock(fileWriterMutex_);

  if (!isIdle()) {
    return Result<NoneType, std::string>::Err(
        "File output cannot be changed while a recording session is active");
  }

  fileProperties_ = properties;
  fileOutputEnabled_.store(true, std::memory_order_release);
  fileOutputConfigured_.store(false, std::memory_order_release);

  return Result<NoneType, std::string>::Ok(None);
}

std::shared_ptr<AudioFileWriter> AndroidAudioRecorder::createFileWriter(
    const std::shared_ptr<AudioFileProperties> &props) {
  if (props->format == AudioFileProperties::Format::WAV) {
    return std::make_shared<MiniAudioFileWriter>(
        audioEventHandlerRegistry_,
        props,
        streamSampleRate_,
        streamChannelCount_,
        streamMaxBufferSizeInFrames_);
  }
#if !RN_AUDIO_API_FFMPEG_DISABLED
  return std::make_shared<android::ffmpeg::FFmpegAudioFileWriter>(
      audioEventHandlerRegistry_,
      props,
      streamSampleRate_,
      streamChannelCount_,
      streamMaxBufferSizeInFrames_);
#else
  return nullptr;
#endif
}

Result<NoneType, std::string> AndroidAudioRecorder::setupFileWriter(
    const std::shared_ptr<AudioFileProperties> &properties,
    const std::string &fileNameOverride) {
#if RN_AUDIO_API_FFMPEG_DISABLED
  if (properties->format != AudioFileProperties::Format::WAV) {
    return Result<NoneType, std::string>::Err(
        "FFmpeg backend is disabled. Cannot create file writer for the requested format. Use WAV format instead.");
  }
#endif

  if (properties->rotateIntervalBytes > 0) {
    fileWriter_ = std::make_shared<AndroidRotatingFileWriter>(
        audioEventHandlerRegistry_,
        properties,
        properties->rotateIntervalBytes,
        [this](const std::shared_ptr<AudioFileProperties> &p) { return createFileWriter(p); },
        [this](const std::string &path) {
          if (!path.empty()) {
            recordingSegmentPaths_.push_back(path);
          }
        });
  } else {
    fileWriter_ = createFileWriter(properties);
  }

  fileWriter_->assignOnErrorCallbackId(errorEvent().getCallbackId());

  auto backend = std::static_pointer_cast<AndroidFileWriterBackend>(fileWriter_);
  auto fileResult = backend->openFile(
      streamSampleRate_, streamChannelCount_, streamMaxBufferSizeInFrames_, fileNameOverride);

  if (!fileResult.is_ok()) {
    fileOutputConfigured_.store(false, std::memory_order_release);
    fileWriter_ = nullptr;
    return Result<NoneType, std::string>::Err(
        "Failed to open file for writing: " + fileResult.unwrap_err());
  }

  filePath_ = fileResult.unwrap();
  if (properties->rotateIntervalBytes == 0) {
    recordingSegmentPaths_.push_back(filePath_);
  }
  fileOutputConfigured_.store(true, std::memory_order_release);
  return Result<NoneType, std::string>::Ok(None);
}

/// @brief Pauses the recording session (see AudioRecorder::markPaused()). A session whose stream
/// is reconnecting or still starting is paused too; a stream reclaimed later keeps it paused.
/// Pausing the stream itself is best-effort: AAudio can't pause an input stream, and the data
/// callback drops buffers while paused either way.
/// This method should be called from the JS thread only.
void AndroidAudioRecorder::pause() {
  std::scoped_lock pauseLock(callbackMutex_, fileWriterMutex_, adapterNodeMutex_);

  if (!markPaused()) {
    return;
  }

  if (mStream_ != nullptr) {
    mStream_->pause(0);
  }
}

/// @brief Resumes a paused recording session. A session whose stream is waiting to be reclaimed
/// resumes too: the reclaimed stream records once it opens.
/// This method should be called from the JS thread only.
void AndroidAudioRecorder::resume() {
  std::scoped_lock resumeLock(callbackMutex_, fileWriterMutex_, adapterNodeMutex_);

  if (!isPaused()) {
    return;
  }

  if (mStream_ != nullptr) {
    mStream_->start(0);
  }
  state_.store(RecorderState::Recording, std::memory_order_release);
}

/// @brief Sets the callback to be invoked when audio data is ready.
/// If the recorder is already active, it will prepare the callback for receiving audio data immediately.
/// This method should be called from the JS thread only.
/// @param sampleRate Desired sample rate for the callback audio data.
/// @param bufferLength Desired buffer length in frames for the callback audio data.
/// @param channelCount Number of channels for the callback audio data.
/// @param callbackId Identifier for the JS callback to be invoked.
/// @returns Success status or Error status with message.
Result<NoneType, std::string> AndroidAudioRecorder::setOnAudioReadyCallback(
    float sampleRate,
    size_t bufferLength,
    int channelCount,
    uint64_t callbackId) {
  std::scoped_lock callbackLock(callbackMutex_, errorCallbackMutex_);

  dataCallbackMetadata_ = {
      .sampleRate = sampleRate,
      .bufferLength = bufferLength,
      .channelCount = channelCount,
      .callbackId = callbackId};

  dataCallback_ = std::make_shared<AndroidRecorderCallback>(
      audioEventHandlerRegistry_, sampleRate, bufferLength, channelCount, callbackId);
  dataCallback_->setOnErrorCallback(errorEvent().getCallbackId());
  callbackOutputEnabled_.store(true, std::memory_order_release);
  callbackOutputConfigured_.store(false, std::memory_order_release);

  if (!isIdle()) {
    std::static_pointer_cast<AndroidRecorderCallback>(dataCallback_)
        ->prepare(streamSampleRate_, streamChannelCount_, streamMaxBufferSizeInFrames_);
    callbackOutputConfigured_.store(true, std::memory_order_release);
  }

  return Result<NoneType, std::string>::Ok(None);
}

/// @brief Connects a RecorderAdapterNode to the recorder for audio data routing.
/// If the recorder is already active, it will initialize the adapter node immediately.
/// This method should be called from the JS thread only.
/// @param node Shared pointer to the RecorderAdapterNode to connect.
void AndroidAudioRecorder::connect(const std::shared_ptr<RecorderAdapterNode> &node) {
  std::scoped_lock adapterLock(adapterNodeMutex_);
  adapterNode_ = node;
  isConnected_.store(true, std::memory_order_release);
  connectedConfigured_.store(false, std::memory_order_release);

  if (!isIdle()) {
    deinterleavingBuffer_ = std::make_shared<AudioBuffer>(
        streamMaxBufferSizeInFrames_, streamChannelCount_, streamSampleRate_);
    adapterNode_->init(streamMaxBufferSizeInFrames_, streamChannelCount_, streamSampleRate_);
    connectedConfigured_.store(true, std::memory_order_release);
  }
}

/// @brief Releases the Android-specific deinterleaving scratch buffer when the
/// adapter node is disconnected. Invoked from the shared AudioRecorder::disconnect()
/// while holding adapterNodeMutex_.
void AndroidAudioRecorder::clearAdapterScratchState() {
  deinterleavingBuffer_ = nullptr;
}

/// @brief onAudioReady callback that is invoked by the Oboe stream when new audio data is available.
/// This method runs on the audio thread.
/// It routes the audio data to the enabled outputs: file writer, callback, and adapter node.
/// For safety measures (check note about RN of enableFileOutput), each output is protected by a lock
/// additionally to the enabled checks.
/// @param oboeStream Pointer to the Oboe audio stream.
/// @param audioData Pointer to the audio data buffer (interleaved float samples).
/// @param numFrames Number of audio frames in the data buffer.
/// @returns DataCallbackResult indicating whether to continue or stop the stream.
oboe::DataCallbackResult AndroidAudioRecorder::onAudioReady(
    oboe::AudioStream *oboeStream,
    void *audioData,
    int32_t numFrames) {
  if (!acceptsInput()) {
    return oboe::DataCallbackResult::Continue;
  }

  if (usesFileOutput()) {
    if (auto fileWriterLock = Locker::tryLock(fileWriterMutex_)) {
      auto fileWriter = fileWriter_;
      if (usesFileOutput() && fileWriter != nullptr) {
        fileWriter->writeAudioData(audioData, numFrames);
      }
    }
  }

  if (usesCallback()) {
    if (auto callbackLock = Locker::tryLock(callbackMutex_)) {
      auto dataCallback = std::static_pointer_cast<AndroidRecorderCallback>(dataCallback_);
      if (usesCallback() && dataCallback != nullptr) {
        dataCallback->receiveAudioData(audioData, numFrames);
      }
    }
  }

  if (isConnected()) {
    if (auto adapterLock = Locker::tryLock(adapterNodeMutex_)) {
      auto adapterNode = adapterNode_;
      auto deinterleavingBuffer = deinterleavingBuffer_;
      if (!isConnected() || adapterNode == nullptr || deinterleavingBuffer == nullptr) {
        return oboe::DataCallbackResult::Continue;
      }

      auto const data = static_cast<float *>(audioData);
      deinterleavingBuffer->deinterleaveFrom(data, numFrames);

      for (size_t ch = 0; ch < streamChannelCount_; ++ch) {
        adapterNode->buff_[ch]->write(*deinterleavingBuffer->getChannel(ch), numFrames);
      }
    }
  }

  return oboe::DataCallbackResult::Continue;
}

/// @brief Whether a session is recording on a started stream; false while a disconnected stream
/// waits to be reclaimed. mStream_ is replaced under all the recorder locks, so one is enough.
bool AndroidAudioRecorder::isRecording() const {
  std::scoped_lock lock(fileWriterMutex_);
  return state_.load(std::memory_order_acquire) == RecorderState::Recording &&
      mStream_ != nullptr && mStream_->getState() == oboe::StreamState::Started;
}

bool AndroidAudioRecorder::isPaused() const {
  return state_.load(std::memory_order_acquire) == RecorderState::Paused;
}

int32_t AndroidAudioRecorder::audioSessionId() const {
  return sessionId_.load(std::memory_order_acquire);
}

/// @brief AudioRecorder::reopenInputStream() for the Oboe input stream.
/// On Android 14 and earlier, a capture started while the app had no microphone access (not on
/// top, no microphone-type foreground service) stays silenced for its whole life: the system
/// checks the RECORD_AUDIO app op when the capture starts and, for an app targeting API 34+,
/// re-checks it only when the op's setting changes. RecordingSilenceListener asks for a fresh
/// stream when the app is back in the foreground and such a capture is still silenced.
/// The replacement is opened before the current stream is touched: when it can't be opened, the
/// current stream keeps running, the session keeps its state and outputs (so stop() still
/// finalizes the file), and the error event is dispatched. A stream whose session id is no longer
/// @p expectedSessionId (the session stopped or a new one started) is left alone.
/// Callable from any thread except the main looper, since it waits on the recorder locks and on
/// the old stream's stop.
void AndroidAudioRecorder::reopenInputStream(int32_t expectedSessionId) {
  std::scoped_lock reopenLock(callbackMutex_, fileWriterMutex_, adapterNodeMutex_);

  if (isIdle() || mStream_ == nullptr ||
      sessionId_.load(std::memory_order_acquire) != expectedSessionId) {
    return;
  }

  std::shared_ptr<oboe::AudioStream> replacement;
  auto openResult = openStream(replacement);
  if (!openResult.is_ok()) {
    std::string message = "Android recorder error: " + openResult.unwrap_err();
    errorEvent().dispatch(StringPayload{.name = "message", .reason = std::move(message)});
    return;
  }

  mStream_->requestStop();
  mStream_->close();
  mStream_ = std::move(replacement);
  adoptStreamProperties();

  // Started even while paused, as the disconnect reclaim does: the data callback drops buffers
  // while paused, and a stopped stream leaves the active recording configurations.
  publishSessionId();
  auto startResult = mStream_->requestStart();
  if (startResult != oboe::Result::OK) {
    std::string message = "Android recorder error: Failed to start stream: " +
        std::string(oboe::convertToText(startResult));
    errorEvent().dispatch(StringPayload{.name = "message", .reason = std::move(message)});
    return;
  }
  markStreamStarted();
}

/// @brief Publishes the current stream's session id. Called under the recorder locks right
/// before the stream is started.
void AndroidAudioRecorder::publishSessionId() {
  sessionId_.store(static_cast<int32_t>(mStream_->getSessionId()), std::memory_order_release);
}

void AndroidAudioRecorder::closeStream() {
  if (mStream_ != nullptr) {
    mStream_->requestStop();
    mStream_->close();
    mStream_.reset();
  }
}

/// @brief Records that the current stream just started, for the reclaim's survival window.
/// Called under the recorder locks.
void AndroidAudioRecorder::markStreamStarted() {
  std::scoped_lock lock(reclaimMutex_);
  streamStartTime_ = Clock::now();
}

/// @brief Ends any reclaim in progress: a reclaim waiting for its delay or survival window
/// gives up. Called under the recorder locks.
/// @returns The generation a reclaim started from now owns.
uint64_t AndroidAudioRecorder::cancelPendingReclaim() {
  std::scoped_lock lock(reclaimMutex_);
  reclaimGeneration_ += 1;
  reclaimCondition_.notify_all();
  return reclaimGeneration_;
}

/// @brief Sets the ReclaimBackoff interruption cause from the back-off policy. Called under the
/// recorder locks.
void AndroidAudioRecorder::setReclaimBackingOff() {
  setInterruptionCause(InterruptionCause::ReclaimBackoff, reclaimBackoff_.isBackingOff());
}

/// @brief onError callback that is invoked by the Oboe stream when an error occurs.
/// Runs on a thread oboe creates for the error, so it may block.
/// A disconnect of an active session's stream is reclaimed: the stream is closed and reopened,
/// after a delay when it keeps disconnecting right after it opens (ReclaimBackoff). Until the
/// reopen, the session keeps its state, outputs and published session id, with no stream; a
/// back-off is reported as an interruption (InterruptionCause::ReclaimBackoff) that ends once
/// the reopened stream outlives the survival window. stop() or start() cancels the reclaim.
/// @param oboeStream Pointer to the Oboe audio stream.
/// @param error The oboe::Result error code.
void AndroidAudioRecorder::onErrorAfterClose(oboe::AudioStream *stream, oboe::Result error) {
  if (error != oboe::Result::ErrorDisconnected) {
    std::string message = "Android recorder error: " + std::string(oboe::convertToText(error));
    errorEvent().dispatch(StringPayload{.name = "message", .reason = std::move(message)});
    return;
  }

  // Only reclaim the stream if the recorder still owns a session. Reopening
  // unconditionally resurrected stopped recorders, so any audio route change
  // (e.g. a Bluetooth headset connecting or disconnecting) turned the mic
  // back on after stop().
  if (isIdle()) {
    return;
  }

  // Keeps the recorder alive while this thread waits. When JS drops the recorder, its host
  // object stops it, which cancels the wait.
  const auto self = weak_from_this().lock();
  if (self == nullptr) {
    return;
  }

  uint64_t generation = 0;
  ReclaimBackoff::Duration delay{};
  {
    // Serialize with start()/stop(): they mutate mStream_ under these locks on
    // the JS thread while this runs on oboe's (detached) error thread.
    std::scoped_lock restartLock(callbackMutex_, fileWriterMutex_, adapterNodeMutex_);

    // The error thread is detached and can arrive late: if stop()/start() have
    // already replaced the stream this error belongs to, don't tear down the
    // healthy new stream.
    if (stream != mStream_.get() || isIdle()) {
      return;
    }

    closeStream();
    const auto lifetime =
        std::chrono::duration_cast<ReclaimBackoff::Duration>(Clock::now() - streamStartTime_);
    delay = reclaimBackoff_.disconnected(lifetime);
    generation = cancelPendingReclaim();
    setReclaimBackingOff();
    __android_log_print(
        ANDROID_LOG_INFO,
        "AndroidAudioRecorder",
        "reclaim n=%d lifetime=%" PRId64 "ms delay=%" PRId64 "ms",
        reclaimBackoff_.fastDisconnects(),
        static_cast<int64_t>(lifetime.count()),
        static_cast<int64_t>(delay.count()));
  }

  reclaimStream(generation, delay);
}

/// @brief Reopens the stream after @p delay, retrying a failed open with the back-off's delays,
/// until a stream starts or the reclaim is cancelled. Called without the recorder locks.
void AndroidAudioRecorder::reclaimStream(uint64_t generation, ReclaimBackoff::Duration delay) {
  auto step = ReclaimStep::Retry;
  while (step == ReclaimStep::Retry) {
    if (!waitForReclaim(generation, Clock::now() + delay)) {
      return;
    }
    step = reopenReclaimedStream(generation, delay);
  }

  if (step == ReclaimStep::AwaitSurvival) {
    awaitReclaimSurvival(generation);
  }
}

/// @returns False when the reclaim of @p generation was cancelled before @p deadline.
bool AndroidAudioRecorder::waitForReclaim(uint64_t generation, Clock::time_point deadline) {
  std::unique_lock lock(reclaimMutex_);
  return !reclaimCondition_.wait_until(
      lock, deadline, [&] { return reclaimGeneration_ != generation; });
}

/// @brief One reopen attempt of the reclaim of @p generation. A stream that fails to open or
/// start counts as a fast disconnect: @p delay is set to the wait before the next attempt, with
/// a warning and no error event, since the attempt repeats while the device refuses the input.
AndroidAudioRecorder::ReclaimStep AndroidAudioRecorder::reopenReclaimedStream(
    uint64_t generation,
    ReclaimBackoff::Duration &delay) {
  std::scoped_lock reclaimLock(callbackMutex_, fileWriterMutex_, adapterNodeMutex_);

  if (reclaimGeneration_ != generation || isIdle() || mStream_ != nullptr) {
    return ReclaimStep::Finished;
  }

  auto openResult = openAudioStream();
  std::string failure;
  if (openResult.is_ok()) {
    // Started even while paused: AAudio can't pause an input stream, and the data callback
    // drops buffers while paused. A stopped stream drops out of the active recording
    // configurations, so the end of whatever silenced it (a call) would never be reported.
    publishSessionId();
    const auto startResult = mStream_->requestStart();
    if (startResult == oboe::Result::OK) {
      markStreamStarted();
      return reclaimBackoff_.isBackingOff() ? ReclaimStep::AwaitSurvival : ReclaimStep::Finished;
    }
    closeStream();
    failure = "Failed to start stream: " + std::string(oboe::convertToText(startResult));
  } else {
    failure = openResult.unwrap_err();
  }

  delay = reclaimBackoff_.openFailed();
  setReclaimBackingOff();
  __android_log_print(
      ANDROID_LOG_WARN,
      "AndroidAudioRecorder",
      "reclaim n=%d failed: %s; retry in %" PRId64 "ms",
      reclaimBackoff_.fastDisconnects(),
      failure.c_str(),
      static_cast<int64_t>(delay.count()));
  return ReclaimStep::Retry;
}

/// @brief Waits until the reclaimed stream has run for the survival window, then ends the
/// back-off and its interruption. The window is measured from the current stream's start, so a
/// reopenInputStream() in between restarts it; a disconnect, start() or stop() cancels it.
void AndroidAudioRecorder::awaitReclaimSurvival(uint64_t generation) {
  {
    std::unique_lock lock(reclaimMutex_);
    while (true) {
      if (reclaimGeneration_ != generation) {
        return;
      }
      const auto deadline = streamStartTime_ + ReclaimBackoff::kSurvivalWindow;
      if (Clock::now() >= deadline) {
        break;
      }
      reclaimCondition_.wait_until(lock, deadline);
    }
  }

  std::scoped_lock survivalLock(callbackMutex_, fileWriterMutex_, adapterNodeMutex_);
  if (reclaimGeneration_ != generation || isIdle() || mStream_ == nullptr) {
    return;
  }
  reclaimBackoff_.survived();
  setReclaimBackingOff();
}

} // namespace audioapi
