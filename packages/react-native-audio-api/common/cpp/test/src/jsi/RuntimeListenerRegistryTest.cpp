#include <audioapi/jsi/RuntimeLifecycleMonitor.h>
#include <audioapi/jsi/RuntimeListenerRegistry.h>
#include <gtest/gtest.h>
#include <atomic>
#include <latch>
#include <memory>
#include <thread>
#include <vector>

using namespace audioapi;

namespace {

/// The registry never dereferences runtime pointers, so any distinct address works as a key.
jsi::Runtime *fakeRuntime(int &storage) {
  return reinterpret_cast<jsi::Runtime *>(&storage);
}

class RecordingListener : public RuntimeLifecycleListener {
 public:
  void onRuntimeDestroyed(jsi::Runtime *rt) override {
    if (released_.load()) {
      callsAfterRelease_.fetch_add(1);
    }
    lastRuntime_.store(rt);
    calls_.fetch_add(1);
  }

  /// Marks the point after which the listener counts as freed and must not be called.
  void release() {
    released_.store(true);
  }

  int calls() const {
    return calls_.load();
  }
  int callsAfterRelease() const {
    return callsAfterRelease_.load();
  }
  jsi::Runtime *lastRuntime() const {
    return lastRuntime_.load();
  }

 private:
  std::atomic<bool> released_{false};
  std::atomic<int> calls_{0};
  std::atomic<int> callsAfterRelease_{0};
  std::atomic<jsi::Runtime *> lastRuntime_{nullptr};
};

} // namespace

TEST(RuntimeListenerRegistryBasicsTest, ContainsOnlyRuntimesWithAddedListeners) {
  RuntimeListenerRegistry registry;
  int storage1 = 0;
  int storage2 = 0;
  RecordingListener listener;

  EXPECT_FALSE(registry.contains(fakeRuntime(storage1)));
  registry.add(fakeRuntime(storage1), &listener);
  EXPECT_TRUE(registry.contains(fakeRuntime(storage1)));
  EXPECT_FALSE(registry.contains(fakeRuntime(storage2)));
}

TEST(RuntimeListenerRegistryBasicsTest, NotifyCallsEachListenerOnceAndForgetsRuntime) {
  RuntimeListenerRegistry registry;
  int storage = 0;
  auto *rt = fakeRuntime(storage);
  RecordingListener first;
  RecordingListener second;

  registry.add(rt, &first);
  registry.add(rt, &first);
  registry.add(rt, &second);
  registry.notifyAndErase(rt);

  EXPECT_EQ(first.calls(), 1);
  EXPECT_EQ(second.calls(), 1);
  EXPECT_EQ(first.lastRuntime(), rt);
  EXPECT_FALSE(registry.contains(rt));

  registry.notifyAndErase(rt);
  EXPECT_EQ(first.calls(), 1);
}

TEST(RuntimeListenerRegistryBasicsTest, NotifyOnlyReachesListenersOfThatRuntime) {
  RuntimeListenerRegistry registry;
  int storage1 = 0;
  int storage2 = 0;
  RecordingListener listener1;
  RecordingListener listener2;

  registry.add(fakeRuntime(storage1), &listener1);
  registry.add(fakeRuntime(storage2), &listener2);
  registry.notifyAndErase(fakeRuntime(storage1));

  EXPECT_EQ(listener1.calls(), 1);
  EXPECT_EQ(listener2.calls(), 0);
  EXPECT_TRUE(registry.contains(fakeRuntime(storage2)));
}

TEST(RuntimeListenerRegistryBasicsTest, RemovedListenerIsNotNotifiedButRuntimeStaysKnown) {
  RuntimeListenerRegistry registry;
  int storage = 0;
  auto *rt = fakeRuntime(storage);
  RecordingListener listener;

  registry.add(rt, &listener);
  registry.remove(rt, &listener);

  // The emptied entry still marks the runtime's monitor as installed.
  EXPECT_TRUE(registry.contains(rt));
  registry.notifyAndErase(rt);
  EXPECT_EQ(listener.calls(), 0);
}

TEST(RuntimeListenerRegistryBasicsTest, RemoveFromAllUnregistersFromEveryRuntime) {
  RuntimeListenerRegistry registry;
  int storage1 = 0;
  int storage2 = 0;
  RecordingListener removed;
  RecordingListener kept;

  registry.add(fakeRuntime(storage1), &removed);
  registry.add(fakeRuntime(storage2), &removed);
  registry.add(fakeRuntime(storage2), &kept);
  registry.removeFromAll(&removed);
  registry.notifyAndErase(fakeRuntime(storage1));
  registry.notifyAndErase(fakeRuntime(storage2));

  EXPECT_EQ(removed.calls(), 0);
  EXPECT_EQ(kept.calls(), 1);
}

TEST(RuntimeListenerRegistryConcurrencyTest, AddAndRemoveAcrossRuntimesFromManyThreads) {
  constexpr int kThreads = 4;
  constexpr int kIterations = 5000;
  RuntimeListenerRegistry registry;
  int storage1 = 0;
  int storage2 = 0;
  jsi::Runtime *runtimes[] = {fakeRuntime(storage1), fakeRuntime(storage2)};
  std::vector<std::unique_ptr<RecordingListener>> listeners;
  for (int i = 0; i < kThreads; ++i) {
    listeners.push_back(std::make_unique<RecordingListener>());
  }

  std::latch start(kThreads);
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      auto *listener = listeners[t].get();
      auto *rt = runtimes[t % 2];
      start.arrive_and_wait();
      for (int i = 0; i < kIterations; ++i) {
        registry.add(rt, listener);
        EXPECT_TRUE(registry.contains(rt));
        if (i % 3 == 0) {
          registry.removeFromAll(listener);
        } else {
          registry.remove(rt, listener);
        }
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }

  for (auto *rt : runtimes) {
    EXPECT_TRUE(registry.contains(rt));
    registry.notifyAndErase(rt);
  }
  for (auto &listener : listeners) {
    EXPECT_EQ(listener->calls(), 0);
  }
}

TEST(RuntimeListenerRegistryConcurrencyTest, ListenerIsNeverNotifiedAfterRemoveFromAllReturns) {
  constexpr int kRounds = 300;
  constexpr int kListeners = 32;

  for (int round = 0; round < kRounds; ++round) {
    RuntimeListenerRegistry registry;
    int storage = 0;
    auto *rt = fakeRuntime(storage);
    std::vector<std::unique_ptr<RecordingListener>> listeners;
    for (int i = 0; i < kListeners; ++i) {
      listeners.push_back(std::make_unique<RecordingListener>());
      registry.add(rt, listeners.back().get());
    }

    std::latch start(2);
    // Plays the listeners' destructors: unregister, then count as freed.
    std::thread finalizer([&] {
      start.arrive_and_wait();
      for (auto &listener : listeners) {
        registry.removeFromAll(listener.get());
        listener->release();
      }
    });
    // Plays the runtime teardown.
    std::thread teardown([&] {
      start.arrive_and_wait();
      registry.notifyAndErase(rt);
    });
    finalizer.join();
    teardown.join();

    for (auto &listener : listeners) {
      ASSERT_LE(listener->calls(), 1) << "round " << round;
      ASSERT_EQ(listener->callsAfterRelease(), 0) << "round " << round;
    }
  }
}
