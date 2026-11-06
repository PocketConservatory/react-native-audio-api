#pragma once

#include <cstdint>

namespace audioapi {

/// @brief A reason the recorder's capture is interrupted.
enum class InterruptionCause : uint8_t {
  /// The system silences the capture (AudioRecordingConfiguration.isClientSilenced() on Android),
  /// including while a call that started after the interruption began still holds its end.
  CaptureSilenced = 1U << 0U,
  /// The input stream keeps disconnecting right after it opens, so its reopen is delayed
  /// (see ReclaimBackoff).
  ReclaimBackoff = 1U << 1U,
};

/// @brief The `interruption` event a change of the active causes calls for.
enum class InterruptionTransition : uint8_t { None, Began, Ended };

/// @brief The set of causes interrupting one recording session. The session's interruption
/// begins when the first cause becomes active and ends when the last one clears, so each
/// `began` is paired with one `ended` whatever order the causes come and go in.
/// Not synchronized; the owner guards it.
class InterruptionCauses {
 public:
  /// @brief Marks @p cause active or inactive.
  /// @returns Began when this made the first cause active, Ended when it cleared the last one,
  /// otherwise None.
  InterruptionTransition set(InterruptionCause cause, bool active);

  [[nodiscard]] bool has(InterruptionCause cause) const;
  [[nodiscard]] bool any() const;

  /// @brief Clears every cause, with no transition to report.
  void clear();

 private:
  uint8_t active_ = 0;
};

} // namespace audioapi
