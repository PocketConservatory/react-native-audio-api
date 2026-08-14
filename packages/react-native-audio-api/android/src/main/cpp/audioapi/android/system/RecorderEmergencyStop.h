#pragma once

#include <fbjni/fbjni.h>

namespace audioapi {

using namespace facebook;

/// JNI bridge for stopping active recordings without an AudioAPIModule instance.
/// Registered from JNI_OnLoad so it survives React module invalidation.
class RecorderEmergencyStop : public jni::JavaClass<RecorderEmergencyStop> {
 public:
  static auto constexpr kJavaDescriptor = "Lcom/swmansion/audioapi/system/RecorderEmergencyStop;";

  static void registerNatives();

 private:
  static void stopActiveRecordings(jni::alias_ref<jclass> /*unused*/);
};

} // namespace audioapi
