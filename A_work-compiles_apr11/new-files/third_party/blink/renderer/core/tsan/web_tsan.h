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
#include <vector>

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
// Shadow Cell — metadata about one recent access to a memory location
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

 private:
  void* backing_store_;
  size_t byte_length_;

  // Shadow cells: 2 cells per byte of SAB.
  // Total shadow size = byte_length * kShadowCellsPerByte * sizeof(ShadowCell).
  std::vector<ShadowCell> shadow_;

  // Lock per 64-byte cache line of SAB. Avoids global contention.
  // One lock per 64 bytes = (byte_length / 64) locks.
  std::vector<base::Lock> stripe_locks_;

  static constexpr size_t kStripeBytes = 64;

  size_t ShadowIndex(size_t byte_offset, uint32_t cell) const {
    return byte_offset * kShadowCellsPerByte + cell;
  }

  size_t StripeIndex(size_t byte_offset) const {
    return byte_offset / kStripeBytes;
  }
};

// --------------------------------------------------------------------------
// WebTSan — the global singleton
// --------------------------------------------------------------------------

class CORE_EXPORT WebTSan {
 public:
  static WebTSan& Get();

  // Thread lifecycle
  uint32_t ThreadCreate();   // returns new tid
  void ThreadDestroy(uint32_t tid);

  // Memory access recording (call from SAB read/write interception)
  void MemoryAccess(void* backing_store, size_t sab_byte_length,
                    size_t offset, size_t size, uint32_t tid, bool is_write);

  // Shadow buffer cleanup — call when a BackingStore is destroyed
  // to prevent stale shadow data from causing false positives if the
  // same host address is reused by a new SAB.
  void RemoveShadow(void* backing_store);

  // Synchronization events
  void Acquire(uint32_t tid, uintptr_t sync_addr);
  void Release(uint32_t tid, uintptr_t sync_addr);

  // Happens-before edge (e.g. postMessage send -> receive)
  // Sender calls HappensBefore, receiver calls HappensAfter with same id.
  uint64_t HappensBefore(uint32_t sender_tid);
  void HappensAfter(uint32_t receiver_tid, uint64_t edge_id);

  // Query
  const VClock& GetThreadClock(uint32_t tid) const {
    return thread_clocks_[tid];
  }

  // Race callback — override for testing or DevTools integration.
  using RaceCallback = void(*)(const RaceReport&);
  void SetRaceCallback(RaceCallback cb) { race_callback_ = cb; }

  // Stats
  uint64_t races_detected() const { return races_detected_; }
  uint64_t accesses_checked() const { return accesses_checked_; }

  // Control
  void Enable() { enabled_.store(true, std::memory_order_release); }
  void Disable() { enabled_.store(false, std::memory_order_release); }
  bool IsEnabled() const { return enabled_.load(std::memory_order_acquire); }
  void Reset();  // clear all state

 private:
  WebTSan();
  ~WebTSan() = default;

  std::atomic<bool> enabled_{false};

  // Per-thread vector clocks
  std::array<VClock, kMaxThreads> thread_clocks_;
  std::array<bool, kMaxThreads> thread_alive_;
  uint32_t next_tid_ = 0;
  base::Lock thread_lock_;

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
  std::atomic<uint64_t> races_detected_{0};
  std::atomic<uint64_t> accesses_checked_{0};
};

}  // namespace tsan
}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_TSAN_WEB_TSAN_H_
