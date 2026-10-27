#include <audioapi/jsi/RuntimeObserver.h>
#include <audioapi/jsi/RuntimeObserverRegistry.h>
#include <memory>

namespace audioapi {

namespace {

// Leaked so that runtimes torn down during static destruction still find it.
RuntimeObserverRegistry &registry() {
  static auto *instance = new RuntimeObserverRegistry();
  return *instance;
}

struct RuntimeObserverObject : public jsi::HostObject {
  jsi::Runtime *rt_;
  explicit RuntimeObserverObject(jsi::Runtime *rt) : rt_(rt) {}
  ~RuntimeObserverObject() override {
    registry().notifyAndErase(rt_);
  }
};

} // namespace

void RuntimeObserver::addListener(jsi::Runtime &rt, RuntimeObserverListener *listener) {
  if (!registry().contains(&rt)) {
    // We install a global host object in the provided runtime, this way we can
    // use that host object destructor to get notified when the runtime is being
    // terminated. We use a unique name for the object as it gets saved with the
    // runtime's global object.
    // No registry lock is held here: setProperty allocates on the JS heap, and
    // a GC it triggers can finalize host objects whose destructors call
    // removeListener.
    rt.global().setProperty(
        rt,
        "__rnaudioapi_runtime_observer",
        jsi::Object::createFromHostObject(rt, std::make_shared<RuntimeObserverObject>(&rt)));
  }
  registry().add(&rt, listener);
}

void RuntimeObserver::removeListener(jsi::Runtime &rt, RuntimeObserverListener *listener) {
  registry().remove(&rt, listener);
}

void RuntimeObserver::removeListenerFromAll(RuntimeObserverListener *listener) {
  registry().removeFromAll(listener);
}

} // namespace audioapi
