#include <audioapi/core/inputs/InterruptionCauses.h>

#include <cstdint>

namespace audioapi {

InterruptionTransition InterruptionCauses::set(InterruptionCause cause, bool active) {
  const bool wasInterrupted = any();
  const auto bit = static_cast<uint8_t>(cause);
  active_ = active ? (active_ | bit) : (active_ & ~bit);

  if (!wasInterrupted && any()) {
    return InterruptionTransition::Began;
  }
  if (wasInterrupted && !any()) {
    return InterruptionTransition::Ended;
  }
  return InterruptionTransition::None;
}

bool InterruptionCauses::has(InterruptionCause cause) const {
  return (active_ & static_cast<uint8_t>(cause)) != 0;
}

bool InterruptionCauses::any() const {
  return active_ != 0;
}

void InterruptionCauses::clear() {
  active_ = 0;
}

} // namespace audioapi
