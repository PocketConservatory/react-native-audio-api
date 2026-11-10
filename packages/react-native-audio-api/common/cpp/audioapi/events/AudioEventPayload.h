#pragma once

#include <audioapi/HostObjects/sources/AudioBufferHostObject.h>
#include <jsi/jsi.h>
#include <memory>
#include <string>
#include <variant>

namespace audioapi {

// broadcast sentinel — listenerId 0 means dispatch to all registered handlers.
static constexpr uint64_t kBroadcastListenerId = 0;

struct EmptyPayload {
  facebook::jsi::Object toJsiObject(facebook::jsi::Runtime &rt) const {
    return facebook::jsi::Object(rt);
  }
};

struct DoubleValuePayload {
  double value;

  facebook::jsi::Object toJsiObject(facebook::jsi::Runtime &rt) const {
    facebook::jsi::Object obj(rt);
    obj.setProperty(rt, "value", value);
    return obj;
  }
};

struct BoolValuePayload {
  bool value;

  facebook::jsi::Object toJsiObject(facebook::jsi::Runtime &rt) const {
    facebook::jsi::Object obj(rt);
    obj.setProperty(rt, "value", value);
    return obj;
  }
};

struct InterruptionPayload {
  std::string type;
  bool shouldResume;

  facebook::jsi::Object toJsiObject(facebook::jsi::Runtime &rt) const {
    facebook::jsi::Object obj(rt);
    obj.setProperty(rt, "type", facebook::jsi::String::createFromUtf8(rt, type));
    obj.setProperty(rt, "shouldResume", shouldResume);
    return obj;
  }
};

struct StringPayload {
  std::string name;
  std::string reason;

  facebook::jsi::Object toJsiObject(facebook::jsi::Runtime &rt) const {
    facebook::jsi::Object obj(rt);
    obj.setProperty(rt, name.c_str(), facebook::jsi::String::createFromUtf8(rt, reason));
    return obj;
  }
};

struct BufferEndedPayload {
  size_t bufferId;
  bool isLastBufferInQueue;

  facebook::jsi::Object toJsiObject(facebook::jsi::Runtime &rt) const {
    facebook::jsi::Object obj(rt);
    obj.setProperty(
        rt, "bufferId", facebook::jsi::String::createFromUtf8(rt, std::to_string(bufferId)));
    obj.setProperty(rt, "isLastBufferInQueue", isLastBufferInQueue);
    return obj;
  }
};

struct AudioReadyPayload {
  std::shared_ptr<AudioBufferHostObject> buffer;
  int numFrames;
  double when;

  facebook::jsi::Object toJsiObject(facebook::jsi::Runtime &rt) const {
    facebook::jsi::Object obj(rt);
    const bool hasBuffer = buffer != nullptr && buffer->sharedBuffer_ != nullptr;

    if (hasBuffer) {
      obj.setProperty(rt, "buffer", AudioBufferHostObject::createJsObject(rt, buffer));
      obj.setProperty(rt, "numFrames", numFrames);
      obj.setProperty(rt, "when", when);
    } else {
      obj.setProperty(rt, "buffer", facebook::jsi::Value::null());
      obj.setProperty(rt, "numFrames", 0);
      obj.setProperty(rt, "when", 0.0);
    }

    return obj;
  }
};

/// Why a recorder now reads its input at a new sample rate.
enum class InputRateChangeReason : uint8_t { Startup, Notification };

struct InputRateChangedPayload {
  double previousSampleRate;
  double sampleRate;
  InputRateChangeReason reason;

  static const char *reasonName(InputRateChangeReason reason) {
    switch (reason) {
      case InputRateChangeReason::Startup:
        return "startup";
      case InputRateChangeReason::Notification:
        return "notification";
    }
    return "";
  }

  facebook::jsi::Object toJsiObject(facebook::jsi::Runtime &rt) const {
    facebook::jsi::Object obj(rt);
    obj.setProperty(rt, "previousSampleRate", previousSampleRate);
    obj.setProperty(rt, "sampleRate", sampleRate);
    obj.setProperty(rt, "reason", facebook::jsi::String::createFromUtf8(rt, reasonName(reason)));
    return obj;
  }
};

using AudioEventPayload = std::variant<
    EmptyPayload,
    DoubleValuePayload,
    BoolValuePayload,
    InterruptionPayload,
    StringPayload,
    BufferEndedPayload,
    AudioReadyPayload,
    InputRateChangedPayload>;

} // namespace audioapi
