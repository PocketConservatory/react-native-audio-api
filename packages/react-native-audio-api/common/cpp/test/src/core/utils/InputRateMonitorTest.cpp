#include <audioapi/core/utils/InputRateMonitor.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <vector>

using namespace audioapi;

namespace {

/// Delivers buffers as an input running at `realRate` would: each buffer's host time is the
/// time its first frame was captured.
class InputStream {
 public:
  InputStream(InputRateMonitor &monitor, double realRate)
      : monitor_(monitor), realRate_(realRate) {}

  /// Feeds @p seconds of input in buffers cycling through @p bufferSizes and returns every
  /// rate the monitor reported.
  std::vector<double> run(double seconds, const std::vector<uint32_t> &bufferSizes) {
    std::vector<double> reports;
    const auto end = nanos_ + static_cast<uint64_t>(seconds * 1e9);
    for (size_t index = 0; nanos_ < end; ++index) {
      const uint32_t frames = bufferSizes[index % bufferSizes.size()];
      if (auto report = monitor_.feed(nanos_, frames)) {
        reports.push_back(*report);
      }
      nanos_ += static_cast<uint64_t>(frames * 1e9 / realRate_);
    }
    return reports;
  }

  void setRealRate(double realRate) {
    realRate_ = realRate;
  }

  void skip(double seconds) {
    nanos_ += static_cast<uint64_t>(seconds * 1e9);
  }

 private:
  InputRateMonitor &monitor_;
  double realRate_;
  uint64_t nanos_ = 1'000'000'000;
};

} // namespace

TEST(InputRateMonitorTest, InputAtLabelledRateWithJitteredBuffersNeverReports) {
  InputRateMonitor monitor;
  monitor.setLabelledRate(48000);
  InputStream input(monitor, 48000);

  EXPECT_TRUE(input.run(10, {470, 471, 512, 341, 1024}).empty());
}

TEST(InputRateMonitorTest, ReportsStandardRateOfMislabelledInput) {
  InputRateMonitor monitor;
  monitor.setLabelledRate(16000);
  InputStream fast(monitor, 48000);
  EXPECT_EQ(fast.run(3, {1024}), std::vector<double>{48000});

  InputRateMonitor closeMonitor;
  closeMonitor.setLabelledRate(48000);
  InputStream close(closeMonitor, 44100);
  EXPECT_EQ(close.run(3, {470, 471}), std::vector<double>{44100});
}

TEST(InputRateMonitorTest, SwitchMidStreamReportsNewRateNotBlend) {
  InputRateMonitor monitor;
  monitor.setLabelledRate(16000);
  InputStream input(monitor, 16000);

  EXPECT_TRUE(input.run(2.5, {256}).empty());
  input.setRealRate(48000);
  EXPECT_EQ(input.run(4, {256}), std::vector<double>{48000});
}

TEST(InputRateMonitorTest, SlowInputWithLongBufferIntervalsIsDetected) {
  InputRateMonitor monitor;
  monitor.setLabelledRate(48000);
  // 4096 frames at 8 kHz arrive every 512 ms, past the minimum gap.
  InputStream input(monitor, 8000);

  EXPECT_EQ(input.run(5, {4096}), std::vector<double>{8000});
}

TEST(InputRateMonitorTest, HostTimeGapReanchorsWithoutReport) {
  InputRateMonitor monitor;
  monitor.setLabelledRate(48000);
  InputStream input(monitor, 48000);

  EXPECT_TRUE(input.run(0.9, {480}).empty());
  input.skip(0.5);
  EXPECT_TRUE(input.run(0.9, {480}).empty());
  input.skip(3);
  EXPECT_TRUE(input.run(3, {480}).empty());
}

TEST(InputRateMonitorTest, RateFarFromEveryStandardRateNeverReports) {
  InputRateMonitor monitor;
  monitor.setLabelledRate(48000);
  InputStream input(monitor, 28000);

  EXPECT_TRUE(input.run(5, {512}).empty());
}

TEST(InputRateMonitorTest, NewGenerationResetsAndRelabelledInputStaysSilent) {
  InputRateMonitor monitor;
  const uint32_t firstGeneration = monitor.setLabelledRate(16000);
  InputStream input(monitor, 48000);

  EXPECT_EQ(input.run(3, {1024}), std::vector<double>{48000});
  EXPECT_EQ(monitor.measuredGeneration(), firstGeneration);

  const uint32_t relabelledGeneration = monitor.setLabelledRate(48000);
  EXPECT_NE(relabelledGeneration, firstGeneration);
  EXPECT_TRUE(input.run(5, {1024}).empty());
  EXPECT_EQ(monitor.measuredGeneration(), relabelledGeneration);
}
