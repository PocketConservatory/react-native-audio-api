#pragma once

#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace facebook::jsi {
class Runtime;
} // namespace facebook::jsi

namespace audioapi {

struct RuntimeLifecycleListener;

/**
 * Thread-safe map of jsi::Runtime to the listeners registered for it. Runtime
 * pointers are used only as keys and passed back to listeners; they are never
 * dereferenced. All methods may be called from any thread.
 */
class RuntimeListenerRegistry {
 public:
  bool contains(facebook::jsi::Runtime *rt);
  void add(facebook::jsi::Runtime *rt, RuntimeLifecycleListener *listener);
  void remove(facebook::jsi::Runtime *rt, RuntimeLifecycleListener *listener);
  void removeFromAll(RuntimeLifecycleListener *listener);

  /**
   * Calls `onRuntimeDestroyed(rt)` on every listener registered for `rt`, then
   * forgets `rt`. Listeners must not call back into the registry.
   */
  void notifyAndErase(facebook::jsi::Runtime *rt);

 private:
  std::mutex mutex_;
  std::unordered_map<facebook::jsi::Runtime *, std::unordered_set<RuntimeLifecycleListener *>>
      listeners_;
};

} // namespace audioapi
