#include <audioapi/core/inputs/ActiveRecorderHandle.h>

#include <audioapi/core/inputs/AudioRecorder.h>

#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

namespace audioapi {

ActiveRecorderHandle &ActiveRecorderHandle::global() {
  static ActiveRecorderHandle handle;
  return handle;
}

Result<NoneType, std::string> ActiveRecorderHandle::tryStart(
    const std::shared_ptr<AudioRecorder> &recorder) {
  if (!recorder) {
    return Result<NoneType, std::string>::Err("Cannot start a null recorder");
  }

  std::scoped_lock lock(mutex_);

  auto prevRecorder = recorder_.lock();
  if (prevRecorder && prevRecorder != recorder && !prevRecorder->isIdle()) {
    return Result<NoneType, std::string>::Err("Another recording is already in progress");
  }

  // The candidate is the session source before start() so the id it publishes while starting
  // is already readable; the slot itself is assigned only once start() succeeds.
  setSessionSource(recorder);
  auto result = recorder->start();
  if (result.is_ok()) {
    recorder_ = recorder;
  } else {
    setSessionSource(recorder_);
  }
  return result;
}

void ActiveRecorderHandle::clearRecorder() {
  std::scoped_lock lock(mutex_);
  recorder_.reset();
  setSessionSource({});
}

void ActiveRecorderHandle::setSessionSource(const std::weak_ptr<AudioRecorder> &recorder) {
  std::scoped_lock lock(sessionSourceMutex_);
  sessionSource_ = recorder;
}

int32_t ActiveRecorderHandle::audioSessionId() const {
  std::shared_ptr<AudioRecorder> recorder;
  {
    std::scoped_lock lock(sessionSourceMutex_);
    recorder = sessionSource_.lock();
  }
  return recorder ? recorder->audioSessionId() : 0;
}

RecorderState ActiveRecorderHandle::currentState() const {
  std::scoped_lock lock(mutex_);
  const std::shared_ptr<AudioRecorder> recorder = recorder_.lock();
  return stateOf(recorder);
}

bool ActiveRecorderHandle::isRecordingOngoing() const {
  return currentState() != RecorderState::Idle;
}

RecorderState ActiveRecorderHandle::pause() {
  std::scoped_lock lock(mutex_);
  const std::shared_ptr<AudioRecorder> recorder = recorder_.lock();
  if (recorder) {
    recorder->pause();
  }
  return stateOf(recorder);
}

RecorderState ActiveRecorderHandle::pause(const std::shared_ptr<AudioRecorder> &expected) {
  std::scoped_lock lock(mutex_);
  if (recorder_.lock() != expected) {
    return RecorderState::Idle;
  }
  return pause();
}

RecorderState ActiveRecorderHandle::resume() {
  std::scoped_lock lock(mutex_);
  const std::shared_ptr<AudioRecorder> recorder = recorder_.lock();
  if (recorder) {
    recorder->resume();
  }
  return stateOf(recorder);
}

RecorderState ActiveRecorderHandle::resume(const std::shared_ptr<AudioRecorder> &expected) {
  std::scoped_lock lock(mutex_);
  if (recorder_.lock() != expected) {
    return RecorderState::Idle;
  }
  return resume();
}

Result<NoneType, std::string> ActiveRecorderHandle::rerouteInput() {
  std::shared_ptr<AudioRecorder> recorder;
  {
    std::scoped_lock lock(mutex_);
    recorder = recorder_.lock();
  }

  if (!recorder) {
    return Result<NoneType, std::string>::Ok(None);
  }
  return recorder->rerouteInput();
}

Result<FileInfo, std::string> ActiveRecorderHandle::stopAndReturnInfo() {
  std::scoped_lock lock(mutex_);
  const std::shared_ptr<AudioRecorder> recorder = recorder_.lock();
  if (!recorder) {
    return Result<FileInfo, std::string>::Err("Recorder is not in recording state.");
  }

  auto result = recorder->stop();
  if (!result.is_ok()) {
    return Result<FileInfo, std::string>::Err(result.unwrap_err());
  }

  FileInfo recording = std::move(result).unwrap();
  if (!recording.paths.empty()) {
    lastResult_ = recording;
  }
  clearRecorder();
  return Result<FileInfo, std::string>::Ok(std::move(recording));
}

Result<FileInfo, std::string> ActiveRecorderHandle::stopAndReturnInfo(
    const std::shared_ptr<AudioRecorder> &expected) {
  std::scoped_lock lock(mutex_);
  if (recorder_.lock() != expected) {
    // if we do not store provided recorder, it meands that it cannot be started, so it is in idle state
    return Result<FileInfo, std::string>::Err("Recorder is not in recording state.");
  }
  return stopAndReturnInfo();
}

RecorderState ActiveRecorderHandle::stopAndReturnState() {
  std::scoped_lock lock(mutex_);
  stopAndReturnInfo();
  return currentState();
}

std::optional<FileInfo> ActiveRecorderHandle::consumeLastRecordingResult() {
  std::scoped_lock lock(mutex_);
  return std::exchange(lastResult_, std::nullopt);
}

RecorderState ActiveRecorderHandle::stateOf(const std::shared_ptr<AudioRecorder> &recorder) {
  return recorder ? recorder->getState() : RecorderState::Idle;
}

} // namespace audioapi
