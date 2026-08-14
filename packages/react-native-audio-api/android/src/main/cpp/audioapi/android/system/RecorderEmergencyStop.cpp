#include <audioapi/android/system/RecorderEmergencyStop.h>
#include <audioapi/core/inputs/AudioRecorderRegistry.h>

namespace audioapi {

void RecorderEmergencyStop::stopActiveRecordings(jni::alias_ref<jclass> /*unused*/) {
  AudioRecorderRegistry::stopAllActiveRecordings();
}

void RecorderEmergencyStop::registerNatives() {
  javaClassStatic()->registerNatives({
      makeNativeMethod("stopActiveRecordings", RecorderEmergencyStop::stopActiveRecordings),
  });
}

} // namespace audioapi
