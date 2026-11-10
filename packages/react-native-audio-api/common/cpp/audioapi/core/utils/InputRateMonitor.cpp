#include <audioapi/core/utils/InputRateMonitor.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace audioapi {

namespace {

constexpr std::array<double, 9> STANDARD_SAMPLE_RATES =
    {8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000, 96000};

double relativeDifference(double value, double reference) {
  return std::abs(value - reference) / reference;
}

std::optional<double> snapToStandardRate(double rate) {
  for (double standardRate : STANDARD_SAMPLE_RATES) {
    if (relativeDifference(rate, standardRate) <= InputRateMonitor::AGREEMENT_TOLERANCE) {
      return standardRate;
    }
  }
  return std::nullopt;
}

} // namespace

uint32_t InputRateMonitor::setLabelledRate(float labelledRate) {
  return publishLabel(labelledRate);
}

void InputRateMonitor::restartMeasurement() {
  publishLabel(std::nullopt);
}

uint32_t InputRateMonitor::publishLabel(std::optional<float> labelledRate) {
  Label current = label_.load(std::memory_order_relaxed);
  Label next{};
  do {
    next = Label{.generation = current.generation + 1, .rate = labelledRate.value_or(current.rate)};
  } while (!label_.compare_exchange_weak(
      current, next, std::memory_order_acq_rel, std::memory_order_relaxed));
  return next.generation;
}

uint32_t InputRateMonitor::generation() const {
  return label_.load(std::memory_order_acquire).generation;
}

std::optional<double> InputRateMonitor::feed(uint64_t hostNanos, uint32_t frames) {
  const Label label = label_.load(std::memory_order_acquire);
  if (label.generation != measuredGeneration_) {
    measuredGeneration_ = label.generation;
    hasAnchor_ = false;
    hasReported_ = false;
  }

  if (label.rate <= 0.0F || hasReported_) {
    return std::nullopt;
  }

  if (!hasAnchor_ || hostNanos <= lastBufferNanos_) {
    anchorAt(hostNanos, frames);
    previousWindowRate_ = 0.0;
    return std::nullopt;
  }

  const uint64_t interval = hostNanos - lastBufferNanos_;
  const bool isGap = lastIntervalNanos_ > 0 &&
      interval > std::max(GAP_INTERVAL_FACTOR * lastIntervalNanos_, MIN_GAP_NANOS);
  if (isGap) {
    anchorAt(hostNanos, frames);
    previousWindowRate_ = 0.0;
    return std::nullopt;
  }

  lastIntervalNanos_ = interval;
  lastBufferNanos_ = hostNanos;

  const uint64_t windowNanos = hostNanos - anchorNanos_;
  if (windowNanos < WINDOW_NANOS) {
    framesSinceAnchor_ += frames;
    return std::nullopt;
  }

  const double windowRate =
      static_cast<double>(framesSinceAnchor_) * 1e9 / static_cast<double>(windowNanos);
  anchorNanos_ = hostNanos;
  framesSinceAnchor_ = frames;

  auto report = evaluateWindow(windowRate, label.rate);
  previousWindowRate_ = windowRate;
  hasReported_ = report.has_value();
  return report;
}

void InputRateMonitor::anchorAt(uint64_t hostNanos, uint32_t frames) {
  hasAnchor_ = true;
  anchorNanos_ = hostNanos;
  framesSinceAnchor_ = frames;
  lastBufferNanos_ = hostNanos;
  lastIntervalNanos_ = 0;
}

std::optional<double> InputRateMonitor::evaluateWindow(double windowRate, double labelledRate) {
  if (previousWindowRate_ <= 0.0 ||
      relativeDifference(windowRate, previousWindowRate_) > AGREEMENT_TOLERANCE ||
      relativeDifference(windowRate, labelledRate) <= MISMATCH_TOLERANCE ||
      relativeDifference(previousWindowRate_, labelledRate) <= MISMATCH_TOLERANCE) {
    return std::nullopt;
  }

  return snapToStandardRate(windowRate);
}

} // namespace audioapi
