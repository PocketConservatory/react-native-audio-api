#include <audioapi/jsi/RuntimeLifecycleMonitor.h>
#include <audioapi/jsi/RuntimeListenerRegistry.h>
#include <memory>

namespace audioapi {

namespace {

// Leaked so that runtimes torn down during static destruction still find it.
RuntimeListenerRegistry &registry() {
  static auto *instance = new RuntimeListenerRegistry();
  return *instance;
}

struct RuntimeLifecycleMonitorObject : public jsi::HostObject {
  jsi::Runtime *rt_;
  explicit RuntimeLifecycleMonitorObject(jsi::Runtime *rt) : rt_(rt) {}
  ~RuntimeLifecycleMonitorObject() override {
    registry().notifyAndErase(rt_);
  }
};

} // namespace

void RuntimeLifecycleMonitor::addListener(jsi::Runtime &rt, RuntimeLifecycleListener *listener) {
  if (!registry().contains(&rt)) {
    // We install a global host object in the provided runtime, this way we can
    // use that host object destructor to get notified when the runtime is being
    // terminated. We use a unique name for the object as it gets saved with the
    // runtime's global object.
    // No registry lock is held here: setProperty can run a GC whose finalizers
    // call removeListener on this same thread.
    rt.global().setProperty(
        rt,
        "__rnaudioapi_runtime_lifecycle_monitor",
        jsi::Object::createFromHostObject(
            rt, std::make_shared<RuntimeLifecycleMonitorObject>(&rt)));
  }
  registry().add(&rt, listener);
}

void RuntimeLifecycleMonitor::removeListener(jsi::Runtime &rt, RuntimeLifecycleListener *listener) {
  registry().remove(&rt, listener);
}

void RuntimeLifecycleMonitor::removeListenerFromAll(RuntimeLifecycleListener *listener) {
  registry().removeFromAll(listener);
}

} // namespace audioapi
