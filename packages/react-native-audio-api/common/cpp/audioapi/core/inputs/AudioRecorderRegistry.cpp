#include <audioapi/core/inputs/AudioRecorder.h>
#include <audioapi/core/inputs/AudioRecorderRegistry.h>

#ifdef ANDROID
#include <android/log.h>
#endif

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace audioapi {

std::mutex AudioRecorderRegistry::mutex_;
std::vector<std::weak_ptr<AudioRecorder>> AudioRecorderRegistry::recorders_;

/// @brief Registers a recorder for emergency stopping.
/// Entries of destroyed recorders are pruned here instead of on destruction.
/// @param recorder The recorder to register.
void AudioRecorderRegistry::registerRecorder(const std::shared_ptr<AudioRecorder> &recorder) {
  std::scoped_lock lock(mutex_);

  recorders_.erase(
      std::remove_if(
          recorders_.begin(),
          recorders_.end(),
          [](const std::weak_ptr<AudioRecorder> &entry) { return entry.expired(); }),
      recorders_.end());
  recorders_.emplace_back(recorder);
}

/// @brief Stops and finalizes every recorder that is not idle. Racing a concurrent JS-thread
/// stop() is safe: the recorder's internal lock picks one winner, the loser gets an inert Err.
void AudioRecorderRegistry::stopAllActiveRecordings() {
  std::vector<std::shared_ptr<AudioRecorder>> liveRecorders;

  {
    std::scoped_lock lock(mutex_);

    for (auto it = recorders_.begin(); it != recorders_.end();) {
      if (auto recorder = it->lock()) {
        liveRecorders.push_back(std::move(recorder));
        ++it;
      } else {
        it = recorders_.erase(it);
      }
    }
  }

  // stop() takes each recorder's internal locks and flushes file output, so it must never run
  // while the registry mutex is held.
  for (const auto &recorder : liveRecorders) {
    if (recorder->isIdle()) {
      continue;
    }

    auto result = recorder->stop();
#ifdef ANDROID
    if (result.is_err()) {
      __android_log_print(
          ANDROID_LOG_WARN,
          "AudioRecorderRegistry",
          "stopAllActiveRecordings: %s",
          result.unwrap_err().c_str());
    }
#else
    (void)result;
#endif
  }
}

} // namespace audioapi
