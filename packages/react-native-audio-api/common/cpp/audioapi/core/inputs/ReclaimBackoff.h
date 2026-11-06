#pragma once

#include <chrono>

namespace audioapi {

/// @brief How long to wait before reopening an input stream that disconnected.
///
/// Some devices disconnect a fresh input stream again right after it starts (a shared input
/// re-routed to another microphone when another client's higher-priority source becomes
/// active), so reopening at once turns into a tight open/disconnect loop. A disconnect is fast when its stream lived less than kSurvivalWindow; a failed open
/// counts as a fast disconnect with no lifetime. The first kImmediateReopens fast disconnects
/// in a row reopen at once; later ones wait kInitialDelay, doubling up to kMaxDelay. A slow
/// disconnect, or a stream that outlives the window (survived()), starts the count over.
/// Pure: the caller passes in the stream lifetimes. Not synchronized; the owner guards it.
class ReclaimBackoff {
 public:
  using Duration = std::chrono::milliseconds;

  static constexpr Duration kSurvivalWindow{1000};
  static constexpr Duration kInitialDelay{250};
  static constexpr Duration kMaxDelay{5000};
  static constexpr int kImmediateReopens = 2;

  /// @brief Records a disconnect of a stream that lived @p streamLifetime.
  /// @returns The delay before the next reopen; zero to reopen at once.
  Duration disconnected(Duration streamLifetime);

  /// @brief Records a failed open, as a fast disconnect. @returns as disconnected().
  Duration openFailed();

  /// @brief Records that the reopened stream outlived kSurvivalWindow.
  void survived();

  /// @brief True while reopens are delayed: from the first nonzero delay until survived(), a slow
  /// disconnect or reset().
  [[nodiscard]] bool isBackingOff() const;

  /// @brief Number of fast disconnects in a row.
  [[nodiscard]] int fastDisconnects() const;

  void reset();

 private:
  int fastDisconnects_ = 0;
};

} // namespace audioapi
