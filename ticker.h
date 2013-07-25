#pragma once

#include <cstdint>
#include <atomic>
#include <thread>

#include "core.h"
#include "macros.h"
#include "spinlock.h"
#include "lockguard.h"

class ticker {
public:
  //static const uint64_t tick_us = 10 * 1000; /* 10 ms */
  static const uint64_t tick_us = 1 * 1000; /* 1 ms */

  ticker()
    : current_tick_(1), last_tick_inclusive_(0)
  {
    std::thread thd(&ticker::tickerloop, this);
    thd.detach();
  }

  inline uint64_t
  global_current_tick() const
  {
    return current_tick_.load(std::memory_order_acquire);
  }

  inline uint64_t
  global_last_tick_inclusive() const
  {
    return last_tick_inclusive_.load(std::memory_order_acquire);
  }

  inline uint64_t
  global_last_tick_exclusive() const
  {
    return global_last_tick_inclusive() + 1;
  }

  // returns true if guard is currently active, along with filling
  // cur_epoch out
  inline bool
  is_locally_guarded(uint64_t &cur_epoch) const
  {
    const uint64_t core_id = coreid::core_id();
    const uint64_t current_tick =
      ticks_[core_id].current_tick_.load(std::memory_order_acquire);
    const uint64_t current_depth =
      ticks_[core_id].depth_.load(std::memory_order_acquire);
    if (current_depth)
      cur_epoch = current_tick;
    return current_depth;
  }

  inline bool
  is_locally_guarded() const
  {
    uint64_t c;
    return is_locally_guarded(c);
  }

  inline spinlock &
  lock_for(uint64_t core_id)
  {
    INVARIANT(core_id < ticks_.size());
    return ticks_[core_id].lock_;
  }

  // a guard is re-entrant within a single thread
  class guard {
  public:

    guard() : impl_(nullptr), core_(0), tick_(0), depth_(0) {}
    guard(ticker &impl)
      : impl_(&impl), core_(coreid::core_id())
    {
      tickinfo &ti = impl_->ticks_[core_];
      // bump the depth first
      const uint64_t prev_depth = util::non_atomic_fetch_add(ti.depth_, 1UL);
      // grab the lock
      if (!prev_depth) {
        ti.lock_.lock();
        // read epoch # (try to advance forward)
        tick_ = impl_->global_current_tick();
        INVARIANT(ti.current_tick_.load(std::memory_order_acquire) <= tick_);
        ti.current_tick_.store(tick_, std::memory_order_release);
      } else {
        tick_ = ti.current_tick_.load(std::memory_order_acquire);
      }
      INVARIANT(ti.lock_.is_locked());
      depth_ = prev_depth + 1;
    }

    guard(guard &&) = default;
    guard(const guard &) = delete;
    guard &operator=(const guard &) = delete;

    ~guard()
    {
      if (!impl_)
        return;
      INVARIANT(core_ == coreid::core_id());
      tickinfo &ti = impl_->ticks_[core_];
      INVARIANT(ti.lock_.is_locked());
      INVARIANT(tick_ > impl_->global_last_tick_inclusive());
      const uint64_t prev_depth = util::non_atomic_fetch_sub(ti.depth_, 1UL);
      INVARIANT(prev_depth == depth_);
      if (!prev_depth)
        INVARIANT(false);
      // unlock
      if (prev_depth == 1)
        ti.lock_.unlock();
    }

    inline uint64_t
    tick() const
    {
      INVARIANT(impl_);
      return tick_;
    }

    inline uint64_t
    core() const
    {
      INVARIANT(impl_);
      return core_;
    }

    inline uint64_t
    depth() const
    {
      INVARIANT(impl_);
      return depth_;
    }

    inline const ticker &
    impl() const
    {
      INVARIANT(impl_);
      return *impl_;
    }

  private:
    ticker *impl_;
    uint64_t core_;
    uint64_t tick_;
    uint64_t depth_;
  };

  static ticker s_instance; // system wide ticker

private:

  void
  tickerloop()
  {
    // runs as daemon
    util::timer loop_timer;
    struct timespec t;
    for (;;) {

      const uint64_t last_loop_usec = loop_timer.lap();
      const uint64_t delay_time_usec = tick_us;
      if (last_loop_usec < delay_time_usec) {
        const uint64_t sleep_ns = (delay_time_usec - last_loop_usec) * 1000;
        t.tv_sec  = sleep_ns / ONE_SECOND_NS;
        t.tv_nsec = sleep_ns % ONE_SECOND_NS;
        nanosleep(&t, nullptr);
      }

      // bump the current tick
      // XXX: ignore overflow
      const uint64_t last_tick = util::non_atomic_fetch_add(current_tick_, 1UL);
      const uint64_t cur_tick  = last_tick + 1;

      // wait for all threads to finish the last tick
      for (size_t i = 0; i < ticks_.size(); i++) {
        tickinfo &ti = ticks_[i];
        const uint64_t thread_cur_tick =
          ti.current_tick_.load(std::memory_order_acquire);
        INVARIANT(thread_cur_tick == last_tick ||
                  thread_cur_tick == cur_tick);
        if (thread_cur_tick == cur_tick)
          continue;
        lock_guard<spinlock> lg(ti.lock_);
        ti.current_tick_.store(cur_tick, std::memory_order_release);
      }

      last_tick_inclusive_.store(last_tick, std::memory_order_release);
    }
  }

  struct tickinfo {
    spinlock lock_; // guards current_tick_ and depth_
    std::atomic<uint64_t> current_tick_; // last RCU epoch this thread has seen
                                         // (implies completion through current_tick_ - 1)
    std::atomic<uint64_t> depth_; // 0 if not in RCU section

    tickinfo() : current_tick_(1), depth_(0) {}
  };

  percore<tickinfo> ticks_;

  std::atomic<uint64_t> current_tick_; // which tick are we currenlty on?
  std::atomic<uint64_t> last_tick_inclusive_;
    // all threads have *completed* ticks <= last_tick_inclusive_
    // (< current_tick_)
};