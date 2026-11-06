#pragma once

#include <cstdint>
#include <string_view>

namespace audioapi {

/// @brief Capture source the recorder asks the platform for (JS `AudioRecorderStartOptions.inputPreset`).
///
/// Android maps each value to the Oboe input preset of the same name (AAudio input preset /
/// MediaRecorder.AudioSource; Generic is MIC). The preset decides which microphone and which
/// processing Android routes to the stream, and a shared input device is routed for the
/// highest-priority source among its active clients. iOS ignores it.
enum class RecorderInputPreset : uint8_t {
  VoiceRecognition = 0,
  Generic,
  Camcorder,
  VoiceCommunication,
  Unprocessed,
  VoicePerformance,
};

/// @brief Parses the JS name of an input preset (e.g. "generic"); an unknown name yields
/// VoiceRecognition, the default.
inline RecorderInputPreset recorderInputPresetFromString(std::string_view name) {
  if (name == "generic") {
    return RecorderInputPreset::Generic;
  }
  if (name == "camcorder") {
    return RecorderInputPreset::Camcorder;
  }
  if (name == "voiceCommunication") {
    return RecorderInputPreset::VoiceCommunication;
  }
  if (name == "unprocessed") {
    return RecorderInputPreset::Unprocessed;
  }
  if (name == "voicePerformance") {
    return RecorderInputPreset::VoicePerformance;
  }
  return RecorderInputPreset::VoiceRecognition;
}

} // namespace audioapi
