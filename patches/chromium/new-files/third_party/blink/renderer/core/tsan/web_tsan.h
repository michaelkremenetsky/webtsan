// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// WebTSan: Thread Sanitizer for SharedArrayBuffer-based threading in browsers.
//
// This implements a simplified TSan that lives in the renderer process (native
// C++ space) and intercepts SharedArrayBuffer accesses, Atomics operations,
// postMessage, and Worker lifecycle events. Shadow memory is allocated in the
// host's 64-bit address space, not inside the SAB itself.
//
// Architecture:
//   - Each Worker gets a logical thread ID (tid)
//   - Each tid has a vector clock (VClock)
//   - Each byte of SAB has shadow cells tracking recent accesses
//   - Synchronization points (Atomics, postMessage) propagate vector clocks
//   - Conflicting accesses with no happens-before ordering = data race

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_TSAN_WEB_TSAN_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_TSAN_WEB_TSAN_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "third_party/blink/renderer/core/core_export.h"

namespace blink {
namespace tsan {

// Maximum number of concurrent logical threads (workers + main thread).
static constexpr uint32_t kMaxThreads = 64;

// Number of shadow cells per SAB byte. More cells = less eviction = fewer
// missed races, but more memory. TSan uses 4; we use 2 to save memory.
static constexpr uint32_t kShadowCellsPerByte = 2;

// --------------------------------------------------------------------------
// Vector Clock
// --------------------------------------------------------------------------

class VClock {
 public:
  VClock() { clk_.fill(0); }

  uint64_t Get(uint32_t tid) const { return clk_[tid]; }
  void Set(uint32_t tid, uint64_t val) { clk_[tid] = val; }
  void Increment(uint32_t tid) { clk_[tid]++; }

  // Returns true if `this` happens-before `other` (every component <= other).
  bool HappensBefore(const VClock& other) const {
    for (uint32_t i = 0; i < kMaxThreads; i++) {
      if (clk_[i] > other.clk_[i])
        return false;
    }
    return true;
  }

  // Join: this = max(this, other) for all components.
  void Join(const VClock& other) {
    for (uint32_t i = 0; i < kMaxThreads; i++) {
      if (other.clk_[i] > clk_[i])
        clk_[i] = other.clk_[i];
    }
  }

 private:
  std::array<uint64_t, kMaxThreads> clk_;
};

// --------------------------------------------------------------------------
// Shadow Cell -- metadata about one recent access to a memory location
// --------------------------------------------------------------------------

struct ShadowCell {
  uint32_t tid = 0;          // thread that performed the access
  uint64_t epoch = 0;        // vector clock value of `tid` at access time
  bool is_write = false;     // read or write?
  bool valid = false;        // is this cell occupied?
};

// --------------------------------------------------------------------------
// Race Report
// --------------------------------------------------------------------------

struct RaceReport {
  size_t sab_offset;         // byte offset into the SharedArrayBuffer
  void* sab_backing_store;   // backing store pointer (identifies which SAB)
  size_t sab_byte_length;

  uint32_t tid1;
  bool is_write1;
  uint64_t epoch1;

  uint32_t tid2;
  bool is_write2;
  uint64_t epoch2;

  std::string ToString() const;
};

// --------------------------------------------------------------------------
// Per-SAB shadow state
// --------------------------------------------------------------------------

class ShadowBuffer {
 public:
  ShadowBuffer(void* backing_store, size_t byte_length);
  ~ShadowBuffer();

  // Record an access and check for races. Returns true if a race was detected.
  bool RecordAccess(size_t offset, size_t size, uint32_t tid,
                    bool is_write, const VClock& thread_clock,
                    RaceReport* out_report);

  // Clear shadow cells for a byte range (e.g. on free/realloc).
  // Prevents false positives from address reuse.
  void ClearRange(size_t offset, size_t size);

 private:
  void* backing_store_;
  size_t byte_length_;

  // Sparse shadow: allocate shadow pages on demand instead of a flat array.
  // Each page covers kPageBytes of SAB and holds shadow cells for those bytes.
  static constexpr size_t kPageBytes = 4096;
  static constexpr size_t kPageCells = kPageBytes * kShadowCellsPerByte;

  struct ShadowPage {
    ShadowCell cells[kPageBytes * kShadowCellsPerByte] = {};
  };

  // Page table: page_index -> ShadowPage.  Only touched pages are allocated.
  std::unordered_map<size_t, std::unique_ptr<ShadowPage>> pages_;

  // Single lock for the page table (page creation).  Individual byte access
  // is further protected by stripe locks once the page exists.
  base::Lock page_lock_;

  // Stripe locks: one per kStripeBytes of SAB.  We cap the stripe count to
  // avoid huge allocations for large SABs.
  static constexpr size_t kStripeBytes = 64;
  static constexpr size_t kMaxStripes = 65536;  // 4MB coverage at 64B each
  std::vector<base::Lock> stripe_locks_;

  ShadowPage* GetOrCreatePage(size_t page_index);

  size_t PageIndex(size_t byte_offset) const {
    return byte_offset / kPageBytes;
  }
  size_t InPageOffset(size_t byte_offset) const {
    return byte_offset % kPageBytes;
  }
  size_t InPageShadowIndex(size_t in_page_offset, uint32_t cell) const {
    return in_page_offset * kShadowCellsPerByte + cell;
  }
  size_t StripeIndex(size_t byte_offset) const {
    return (byte_offset / kStripeBytes) % stripe_locks_.size();
  }
};

// --------------------------------------------------------------------------
// WebTSan -- the global singleton
// --------------------------------------------------------------------------

class CORE_EXPORT WebTSan {
 public:
  static WebTSan& Get();

  // Thread lifecycle.
  // ThreadCreate inherits the caller's vector clock so happens-before edges
  // the parent has acquired transitively apply to the child. Pass UINT32_MAX
  // for `parent_tid` to create with an empty clock (e.g. first thread ever).
  // The new thread also acquires the "orphan" clock — the union of clocks
  // from all detached threads that have exited. This matches real TSan's
  // behavior where detached threads' final clocks are preserved globally.
  uint32_t ThreadCreate(uint32_t parent_tid = UINT32_MAX);
  // Called on pthread_exit / thread finish. Merges the thread's final clock
  // into the global orphan clock so future threads can observe it.
  void ThreadFinish(uint32_t tid);
  void ThreadDestroy(uint32_t tid);

  // Memory access recording (call from SAB read/write interception)
  void MemoryAccess(void* backing_store, size_t sab_byte_length,
                    size_t offset, size_t size, uint32_t tid, bool is_write);

  // Shadow buffer cleanup -- call when a BackingStore is destroyed
  // to prevent stale shadow data from causing false positives if the
  // same host address is reused by a new SAB.
  void RemoveShadow(void* backing_store);

  // Clear shadow cells for a byte range within a SAB.
  // Called from emscripten's free() hook to prevent false positives
  // from malloc/free address reuse.
  void ClearShadow(void* backing_store, size_t offset, size_t size);

  // Synchronization events
  void Acquire(uint32_t tid, uintptr_t sync_addr);
  void Release(uint32_t tid, uintptr_t sync_addr);

  // Mutex primitives (LLVM TSan model). Attach sync clocks to user-level
  // addresses so lock/unlock works on any implementation, including the
  // emscripten futex-based pthread_mutex. PreLock is currently a no-op
  // (kept for API parity with real TSan's deadlock detector).
  void MutexCreate(uintptr_t addr);
  void MutexDestroy(uintptr_t addr);
  void MutexPreLock(uint32_t tid, uintptr_t addr);
  void MutexPostLock(uint32_t tid, uintptr_t addr);
  void MutexUnlock(uint32_t tid, uintptr_t addr);
  // Rwlock: reader-side acquires on post-lock; the release-side is a no-op
  // since readers don't advance the mutex clock.
  void MutexReadLock(uint32_t tid, uintptr_t addr);
  void MutexReadUnlock(uint32_t tid, uintptr_t addr);

  // Thread join: merges `child_tid`'s final clock into `joiner_tid`. Also
  // marks the child no-longer-alive (so its slot may be reused) but does
  // NOT re-broadcast into orphan_clock_ (join already transferred the HB
  // edge to the specific joiner).
  void ThreadJoin(uint32_t joiner_tid, uint32_t child_tid);
  // Thread detach: no HB edge to any future joiner, but we still want the
  // final clock preserved for subsequent threads — same as ThreadFinish.
  void ThreadDetach(uint32_t child_tid);

  // Merge `src_tid`'s current clock into `dst_tid`. Used at pthread startup
  // on the child side: the pool Worker has absorbed the parent→worker
  // postMessage HB edge into its Worker-tid, and when we swap the OS
  // thread's current tid to the new pthread's webtsan_tid, we need the
  // pthread to inherit that HB. Without this the postMessage edge dead-ends
  // in the Worker-tid slot and the pthread starts with a stale clock.
  void ThreadAbsorb(uint32_t dst_tid, uint32_t src_tid);

  // Happens-before edge (e.g. postMessage send -> receive)
  // Sender calls HappensBefore, receiver calls HappensAfter with same id.
  uint64_t HappensBefore(uint32_t sender_tid);
  void HappensAfter(uint32_t receiver_tid, uint64_t edge_id);

  // Query
  const VClock& GetThreadClock(uint32_t tid) const {
    return thread_clocks_[tid];
  }

  // Race callback -- override for testing or DevTools integration.
  using RaceCallback = void(*)(const RaceReport&);
  void SetRaceCallback(RaceCallback cb) { race_callback_ = cb; }

  // Stats
  uint64_t races_detected() const { return races_detected_; }
  uint64_t accesses_checked() const { return accesses_checked_; }
  uint64_t accesses_suppressed() const { return accesses_suppressed_; }
  void IncrementSuppressedCount() {
    accesses_suppressed_.fetch_add(1, std::memory_order_relaxed);
  }

  // Control
  void Enable() { enabled_.store(true, std::memory_order_release); }
  void Disable() { enabled_.store(false, std::memory_order_release); }
  bool IsEnabled() const { return enabled_.load(std::memory_order_acquire); }
  void Reset();  // clear all state

 private:
  friend class base::NoDestructor<WebTSan>;
  WebTSan();
  ~WebTSan() = default;

  std::atomic<bool> enabled_{false};

  // Per-thread vector clocks
  std::array<VClock, kMaxThreads> thread_clocks_;
  std::array<bool, kMaxThreads> thread_alive_;
  uint32_t next_tid_ = 0;
  base::Lock thread_lock_;

  // Clock aggregating all detached-thread final states. Any new thread
  // joins this clock at creation so it observes writes from all finished
  // predecessors — handles the pthread_detach + pool-worker-reuse case
  // where there's no explicit join.
  VClock orphan_clock_;

  // Per-sync-address vector clocks (for mutex-like synchronization)
  std::unordered_map<uintptr_t, VClock> sync_clocks_;
  base::Lock sync_lock_;

  // Happens-before edges (for postMessage)
  std::unordered_map<uint64_t, VClock> hb_edges_;
  uint64_t next_edge_id_ = 1;
  base::Lock hb_lock_;

  // Per-SAB shadow buffers, keyed by backing store pointer
  std::unordered_map<void*, std::unique_ptr<ShadowBuffer>> shadow_buffers_;
  base::Lock shadow_lock_;

  ShadowBuffer* GetOrCreateShadow(void* backing_store, size_t byte_length);

  RaceCallback race_callback_ = nullptr;
  std::atomic<bool> first_race_logged_{false};
  std::atomic<uint64_t> races_detected_{0};
  std::atomic<uint64_t> accesses_checked_{0};
  std::atomic<uint64_t> accesses_suppressed_{0};

  // Per-site dedup: once we've reported a race at a (backing_store, 8-byte
  // bucket) address, suppress further reports from the same location.
  // Without this, a real race on a single global (e.g. __environ) fires
  // thousands of times per second as every read re-triggers it, swamps
  // the reporter, and breaks timing-sensitive code paths.
  std::unordered_set<uint64_t> reported_race_sites_;
  base::Lock reported_race_sites_lock_;
};

}  // namespace tsan
}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_TSAN_WEB_TSAN_H_
