#include <audioapi/core/utils/AudioRecorderCallback.h>

// The test build excludes AudioRecorderCallback.cpp: it constructs
// AudioBufferHostObject, which the HostObjects exclusion removes from the
// library. AudioRecorder.cpp links only these two error-callback registrars,
// so mirror their real one-line bodies to keep recorder tests linkable
// without the JSI layer.
namespace audioapi {

void AudioRecorderCallback::setOnErrorCallback(uint64_t callbackId) {
  errorCallbackId_.store(callbackId, std::memory_order_release);
}

void AudioRecorderCallback::clearOnErrorCallback() {
  errorCallbackId_.store(0, std::memory_order_release);
}

} // namespace audioapi
