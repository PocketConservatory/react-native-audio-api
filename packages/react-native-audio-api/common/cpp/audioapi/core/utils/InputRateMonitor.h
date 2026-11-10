#pragma once

#include <atomic>
#include <cstdint>
#include <optional>

namespace audioapi {

/// Measures the rate input frames really arrive at, from host timestamps, and reports a standard
/// sample rate that disagrees with the rate the input is labelled with.
///
/// Each window of at least `WINDOW_NANOS` measures its own rate: the frames delivered before the
/// current buffer over the time since the window's first buffer. A report needs two consecutive
/// windows that agree with each other within `AGREEMENT_TOLERANCE`, both off the label by more
/// than `MISMATCH_TOLERANCE`, and a standard rate within `AGREEMENT_TOLERANCE` of the measurement;
/// a raw measured rate is never reported. A gap between buffers (pause, interruption, engine
/// rebuild) re-anchors the window without evaluating it. After a report the monitor stays silent
/// until the label is published again.
///
/// Control threads publish the label with `setLabelledRate()`; the audio thread owns everything
/// else and resets itself when it sees a new generation. `feed()` neither locks nor allocates.
class InputRateMonitor {
 public:
  static constexpr uint64_t WINDOW_NANOS = 1'000'000'000;
  static constexpr uint64_t MIN_GAP_NANOS = 250'000'000;
  static constexpr uint64_t GAP_INTERVAL_FACTOR = 4;
  static constexpr double MISMATCH_TOLERANCE = 0.05;
  static constexpr double AGREEMENT_TOLERANCE = 0.02;

  /// Any thread. Starts a new measurement against @p labelledRate; 0 turns reports off.
  /// @returns The generation of the new label.
  uint32_t setLabelledRate(float labelledRate);

  /// Any thread. Starts a new measurement against the current label.
  void restartMeasurement();

  /// Any thread. The generation of the label published last.
  [[nodiscard]] uint32_t generation() const;

  /// Audio thread only. @p hostNanos is the host time of the buffer's first frame.
  /// @returns The standard sample rate the input really runs at, when it disagrees with the label.
  std::optional<double> feed(uint64_t hostNanos, uint32_t frames);

  /// Audio thread only. The generation the last `feed()` measured against.
  [[nodiscard]] uint32_t measuredGeneration() const {
    return measuredGeneration_;
  }

 private:
  struct Label {
    uint32_t generation;
    float rate;
  };
  static_assert(std::atomic<Label>::is_always_lock_free);

  /// @p labelledRate of nullopt keeps the current rate.
  uint32_t publishLabel(std::optional<float> labelledRate);
  void anchorAt(uint64_t hostNanos, uint32_t frames);
  std::optional<double> evaluateWindow(double windowRate, double labelledRate);

  std::atomic<Label> label_{Label{.generation = 0, .rate = 0.0F}};

  uint32_t measuredGeneration_ = 0;
  bool hasAnchor_ = false;
  bool hasReported_ = false;
  uint64_t anchorNanos_ = 0;
  uint64_t framesSinceAnchor_ = 0;
  uint64_t lastBufferNanos_ = 0;
  /// 0 until two buffers in a row have arrived since the last anchor.
  uint64_t lastIntervalNanos_ = 0;
  /// 0 when the previous window was cut short or never measured.
  double previousWindowRate_ = 0.0;
};

} // namespace audioapi
