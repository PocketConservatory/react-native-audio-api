#include <audioapi/jsi/RuntimeObserver.h>
#include <audioapi/jsi/RuntimeObserverRegistry.h>

namespace audioapi {

bool RuntimeObserverRegistry::contains(jsi::Runtime *rt) {
  std::scoped_lock lock(mutex_);
  return listeners_.contains(rt);
}

void RuntimeObserverRegistry::add(jsi::Runtime *rt, RuntimeObserverListener *listener) {
  std::scoped_lock lock(mutex_);
  listeners_[rt].insert(listener);
}

void RuntimeObserverRegistry::remove(jsi::Runtime *rt, RuntimeObserverListener *listener) {
  std::scoped_lock lock(mutex_);
  // An emptied entry is kept: it records that the runtime's observer object is installed.
  if (auto it = listeners_.find(rt); it != listeners_.end()) {
    it->second.erase(listener);
  }
}

void RuntimeObserverRegistry::removeFromAll(RuntimeObserverListener *listener) {
  std::scoped_lock lock(mutex_);
  for (auto &[rt, set] : listeners_) {
    set.erase(listener);
  }
}

void RuntimeObserverRegistry::notifyAndErase(jsi::Runtime *rt) {
  // Notifying under the lock means a listener that a concurrent removeFromAll
  // (run from its destructor) has unregistered is never called after free.
  std::scoped_lock lock(mutex_);
  auto it = listeners_.find(rt);
  if (it == listeners_.end()) {
    return;
  }
  for (auto *listener : it->second) {
    listener->onRuntimeDestroyed(rt);
  }
  listeners_.erase(it);
}

} // namespace audioapi
