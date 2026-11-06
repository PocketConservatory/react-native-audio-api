#include <audioapi/core/inputs/ReclaimBackoff.h>

#include <algorithm>

namespace audioapi {

ReclaimBackoff::Duration ReclaimBackoff::disconnected(Duration streamLifetime) {
  if (streamLifetime >= kSurvivalWindow) {
    fastDisconnects_ = 0;
    return Duration::zero();
  }

  fastDisconnects_ += 1;
  if (!isBackingOff()) {
    return Duration::zero();
  }

  // Doubling stops at the cap, so a long run of failures never overflows the delay.
  Duration delay = kInitialDelay;
  for (int i = kImmediateReopens + 1; i < fastDisconnects_ && delay < kMaxDelay; ++i) {
    delay *= 2;
  }
  return std::min(delay, kMaxDelay);
}

ReclaimBackoff::Duration ReclaimBackoff::openFailed() {
  return disconnected(Duration::zero());
}

void ReclaimBackoff::survived() {
  reset();
}

bool ReclaimBackoff::isBackingOff() const {
  return fastDisconnects_ > kImmediateReopens;
}

int ReclaimBackoff::fastDisconnects() const {
  return fastDisconnects_;
}

void ReclaimBackoff::reset() {
  fastDisconnects_ = 0;
}

} // namespace audioapi
