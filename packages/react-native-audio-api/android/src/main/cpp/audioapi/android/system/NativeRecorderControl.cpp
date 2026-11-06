#include <audioapi/android/system/NativeRecorderControl.h>

#include <audioapi/core/inputs/ActiveRecorderHandle.h>
#include <audioapi/core/inputs/InterruptionCauses.h>
#include <audioapi/core/inputs/RecorderState.h>

namespace audioapi {

namespace {

jint toOrdinal(RecorderState state) {
  return static_cast<jint>(state);
}

} // namespace

void NativeRecorderControl::registerNatives() {
  javaClassStatic()->registerNatives({
      makeNativeMethod("stopActiveRecording", NativeRecorderControl::stopActiveRecording),
      makeNativeMethod("pauseActiveRecording", NativeRecorderControl::pauseActiveRecording),
      makeNativeMethod("resumeActiveRecording", NativeRecorderControl::resumeActiveRecording),
      makeNativeMethod("currentRecorderState", NativeRecorderControl::currentRecorderState),
      makeNativeMethod("activeAudioSessionId", NativeRecorderControl::activeAudioSessionId),
      makeNativeMethod("reopenActiveInputStream", NativeRecorderControl::reopenActiveInputStream),
      makeNativeMethod("setActiveCaptureSilenced", NativeRecorderControl::setActiveCaptureSilenced),
      makeNativeMethod(
          "dropActiveCaptureSilenced", NativeRecorderControl::dropActiveCaptureSilenced),
      makeNativeMethod(
          "isActiveReclaimBackingOff", NativeRecorderControl::isActiveReclaimBackingOff),
  });
}

jint NativeRecorderControl::stopActiveRecording(jni::alias_ref<jni::JClass> /*clazz*/) {
  return toOrdinal(ActiveRecorderHandle::global().stopAndReturnState());
}

jint NativeRecorderControl::pauseActiveRecording(jni::alias_ref<jni::JClass> /*clazz*/) {
  return toOrdinal(ActiveRecorderHandle::global().pause());
}

jint NativeRecorderControl::resumeActiveRecording(jni::alias_ref<jni::JClass> /*clazz*/) {
  return toOrdinal(ActiveRecorderHandle::global().resume());
}

jint NativeRecorderControl::currentRecorderState(jni::alias_ref<jni::JClass> /*clazz*/) {
  return toOrdinal(ActiveRecorderHandle::global().currentState());
}

jint NativeRecorderControl::activeAudioSessionId(jni::alias_ref<jni::JClass> /*clazz*/) {
  return ActiveRecorderHandle::global().audioSessionId();
}

void NativeRecorderControl::reopenActiveInputStream(
    jni::alias_ref<jni::JClass> /*clazz*/,
    jint expectedSessionId) {
  ActiveRecorderHandle::global().reopenInputStream(expectedSessionId);
}

void NativeRecorderControl::setActiveCaptureSilenced(
    jni::alias_ref<jni::JClass> /*clazz*/,
    jboolean silenced) {
  ActiveRecorderHandle::global().setInterruptionCause(
      InterruptionCause::CaptureSilenced, silenced == JNI_TRUE);
}

void NativeRecorderControl::dropActiveCaptureSilenced(jni::alias_ref<jni::JClass> /*clazz*/) {
  ActiveRecorderHandle::global().dropInterruptionCause(InterruptionCause::CaptureSilenced);
}

jboolean NativeRecorderControl::isActiveReclaimBackingOff(jni::alias_ref<jni::JClass> /*clazz*/) {
  return ActiveRecorderHandle::global().hasInterruptionCause(InterruptionCause::ReclaimBackoff)
      ? JNI_TRUE
      : JNI_FALSE;
}

} // namespace audioapi
