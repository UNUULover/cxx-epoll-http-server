#include "ws/timer.h"

#include <algorithm>

namespace ws {

TimerId TimerHeap::add(TimePoint when, TimerCallback cb) {
  const TimerId id = next_id_++;
  heap_.push(Entry{when, std::move(cb), id});
  return id;
}

TimerId TimerHeap::addAfter(int64_t ms, TimerCallback cb) {
  return add(now() + std::chrono::milliseconds(ms), std::move(cb));
}

void TimerHeap::cancel(TimerId id) {
  // id 是单调递增发出的，>= next_id_ 说明这个 id 根本不存在，
  // 直接忽略 —— 否则 cancelled_ 会被无意义地撑大。
  if (id == 0 || id >= next_id_) return;
  cancelled_.insert(id);
}

size_t TimerHeap::expire() {
  const TimePoint t = now();
  size_t fired = 0;
  while (!heap_.empty() && heap_.top().when <= t) {
    Entry e = heap_.top();
    heap_.pop();

    if (cancelled_.erase(e.id) > 0) continue;  // 已经被取消了，静默丢掉

    if (e.cb) e.cb();
    ++fired;
  }
  return fired;
}

int TimerHeap::nextTimeoutMs() const {
  if (heap_.empty()) return -1;
  const auto delta = heap_.top().when - now();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(delta).count();
  if (ms <= 0) return 0;
  return static_cast<int>(ms);
}

}  // namespace ws
