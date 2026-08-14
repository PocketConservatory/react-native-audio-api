#pragma once

#include <memory>
#include <mutex>
#include <vector>

namespace audioapi {

class AudioRecorder;

/// Process-wide registry of recorders so platform code (e.g. the Android foreground service on
/// system-initiated teardown) can finalize active recordings without any JSI object at hand.
class AudioRecorderRegistry {
 public:
  static void registerRecorder(const std::shared_ptr<AudioRecorder> &recorder);
  static void stopAllActiveRecordings();

 private:
  static std::mutex mutex_;
  static std::vector<std::weak_ptr<AudioRecorder>> recorders_;
};

} // namespace audioapi
