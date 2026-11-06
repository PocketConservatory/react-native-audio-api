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
    const std::shared_ptr<AudioRecorder> &recorder,
    const std::string &fileNameOverride,
    RecorderInputPreset inputPreset) {
  if (!recorder) {
    return Result<NoneType, std::string>::Err("Cannot start a null recorder");
  }

  std::scoped_lock lock(mutex_);

  auto prevRecorder = recorder_.lock();
  if (prevRecorder && prevRecorder != recorder && !prevRecorder->isIdle()) {
    return Result<NoneType, std::string>::Err("Another recording is already in progress");
  }

  // Name the candidate before start() so a session id it publishes while starting is
  // already readable; the slot itself is assigned only once start() succeeds.
  setSessionSource(recorder);
  auto result = recorder->start(fileNameOverride, inputPreset);
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
  std::scoped_lock lock(sessionSourceMutex_);
  const std::shared_ptr<AudioRecorder> recorder = sessionSource_.lock();
  return recorder ? recorder->audioSessionId() : 0;
}

std::shared_ptr<AudioRecorder> ActiveRecorderHandle::sessionSource() const {
  std::scoped_lock lock(sessionSourceMutex_);
  return sessionSource_.lock();
}

void ActiveRecorderHandle::setInterruptionCause(InterruptionCause cause, bool active) {
  if (const auto recorder = sessionSource()) {
    recorder->setInterruptionCause(cause, active);
  }
}

void ActiveRecorderHandle::dropInterruptionCause(InterruptionCause cause) {
  if (const auto recorder = sessionSource()) {
    recorder->dropInterruptionCause(cause);
  }
}

bool ActiveRecorderHandle::hasInterruptionCause(InterruptionCause cause) const {
  const auto recorder = sessionSource();
  return recorder != nullptr && recorder->hasInterruptionCause(cause);
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

RecorderState ActiveRecorderHandle::resume() {
  std::scoped_lock lock(mutex_);
  const std::shared_ptr<AudioRecorder> recorder = recorder_.lock();
  if (recorder) {
    recorder->resume();
  }
  return stateOf(recorder);
}

void ActiveRecorderHandle::reopenInputStream(int32_t expectedSessionId) {
  std::scoped_lock lock(mutex_);
  const std::shared_ptr<AudioRecorder> recorder = recorder_.lock();
  if (recorder) {
    recorder->reopenInputStream(expectedSessionId);
  }
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

  auto [paths, size, duration] = result.unwrap();
  FileInfo recording{.paths = std::move(paths), .size = size, .duration = duration};
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
