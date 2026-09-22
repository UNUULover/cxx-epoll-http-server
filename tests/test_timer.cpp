#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <vector>

#include "ws/timer.h"

using namespace ws;
using namespace std::chrono_literals;

TEST(TimerHeap, FiresInOrder) {
  TimerHeap h;
  std::vector<int> order;

  h.addAfter(30, [&] { order.push_back(3); });
  h.addAfter(10, [&] { order.push_back(1); });
  h.addAfter(20, [&] { order.push_back(2); });

  EXPECT_EQ(h.size(), 3u);
  EXPECT_EQ(h.expire(), 0u);  // 都还没到点

  std::this_thread::sleep_for(80ms);
  EXPECT_EQ(h.expire(), 3u);
  EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
  EXPECT_TRUE(h.empty());
}

// 懒删除：cancel 之后定时器仍留在堆里，但到点被弹出时会跳过。
TEST(TimerHeap, CancelPreventsFiring) {
  TimerHeap h;
  int fired = 0;
  const TimerId t = h.addAfter(10, [&] { ++fired; });
  h.cancel(t);

  std::this_thread::sleep_for(40ms);
  EXPECT_EQ(h.expire(), 0u);
  EXPECT_EQ(fired, 0);
  EXPECT_TRUE(h.empty());
}

TEST(TimerHeap, CancelOneOfSeveral) {
  TimerHeap h;
  int a = 0;
  int b = 0;
  h.addAfter(10, [&] { ++a; });
  const TimerId tb = h.addAfter(10, [&] { ++b; });
  h.cancel(tb);

  std::this_thread::sleep_for(40ms);
  EXPECT_EQ(h.expire(), 1u);
  EXPECT_EQ(a, 1);
  EXPECT_EQ(b, 0);
}

// 不存在（或已作废）的 id 被取消时不能影响其他定时器。
TEST(TimerHeap, CancelUnknownIdIsHarmless) {
  TimerHeap h;
  h.cancel(0);
  h.cancel(999999);

  int fired = 0;
  h.addAfter(5, [&] { ++fired; });
  std::this_thread::sleep_for(40ms);
  EXPECT_EQ(h.expire(), 1u);
  EXPECT_EQ(fired, 1);
}

// nextTimeoutMs 直接当 epoll_wait 的超时用，所以要能表达「没有定时器」和「已经到点」。
TEST(TimerHeap, NextTimeoutMs) {
  TimerHeap h;
  EXPECT_EQ(h.nextTimeoutMs(), -1);  // 空堆

  h.addAfter(50, [] {});
  const int t = h.nextTimeoutMs();
  EXPECT_GT(t, 30);
  EXPECT_LE(t, 50);

  h.addAfter(0, [] {});
  EXPECT_EQ(h.nextTimeoutMs(), 0);  // 已到点，不该睡
}

TEST(TimerHeap, ExpireRunsCallbackOnce) {
  TimerHeap h;
  int fired = 0;
  h.addAfter(5, [&] { ++fired; });

  std::this_thread::sleep_for(40ms);
  EXPECT_EQ(h.expire(), 1u);
  EXPECT_EQ(h.expire(), 0u);  // 弹过了就没了
  EXPECT_EQ(fired, 1);
}
