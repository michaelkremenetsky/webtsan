// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/tsan/web_tsan.h"
#include "third_party/blink/renderer/core/tsan/web_tsan_hooks.h"

#include <cstdio>
#include <cstring>

#include "base/check_op.h"
#include "base/logging.h"
#include "base/no_destructor.h"

namespace blink {
namespace tsan {

// --------------------------------------------------------------------------
// RaceReport
// --------------------------------------------------------------------------

std::string RaceReport::ToString() const {
  char buf[512];
  snprintf(buf, sizeof(buf),
           "DATA RACE detected at SAB offset 0x%zx (backing_store=%p, "
           "size=%zu):\n"
           "  Access 1: tid=%u %s epoch=%llu\n"
           "  Access 2: tid=%u %s epoch=%llu\n"
           "  (at least one is a write, no happens-before ordering)\n",
           sab_offset, sab_backing_store, sab_byte_length,
           tid1, is_write1 ? "WRITE" : "READ",
           static_cast<unsigned long long>(epoch1),
           tid2, is_write2 ? "WRITE" : "READ",
           static_cast<unsigned long long>(epoch2));
  return std::string(buf);
}

// --------------------------------------------------------------------------
// ShadowBuffer
// --------------------------------------------------------------------------

ShadowBuffer::ShadowBuffer(void* backing_store, size_t byte_length)
    : backing_store_(backing_store),
      byte_length_(byte_length),
      stripe_locks_(std::min((byte_length + kStripeBytes - 1) / kStripeBytes,
                             kMaxStripes)) {}

ShadowBuffer::~ShadowBuffer() = default;

ShadowBuffer::ShadowPage* ShadowBuffer::GetOrCreatePage(size_t page_index) {
  base::AutoLock lock(page_lock_);
  auto& slot = pages_[page_index];
  if (!slot)
    slot = std::make_unique<ShadowPage>();
  return slot.get();
}

bool ShadowBuffer::RecordAccess(size_t offset, size_t size, uint32_t tid,
                                bool is_write, const VClock& thread_clock,
                                RaceReport* out_report) {
  for (size_t i = 0; i < size && (offset + i) < byte_length_; i++) {
    size_t byte = offset + i;
    size_t stripe = StripeIndex(byte);
    base::AutoLock lock(stripe_locks_[stripe]);

    ShadowPage* page = GetOrCreatePage(PageIndex(byte));
    size_t in_page = InPageOffset(byte);
    bool found_race = false;

    for (uint32_t c = 0; c < kShadowCellsPerByte; c++) {
      ShadowCell& cell = page->cells[InPageShadowIndex(in_page, c)];

      if (!cell.valid)
        continue;
      if (cell.tid == tid)
        continue;
      if (!cell.is_write && !is_write)
        continue;
      if (thread_clock.Get(cell.tid) >= cell.epoch)
        continue;

      if (out_report) {
        out_report->sab_offset = byte;
        out_report->sab_backing_store = backing_store_;
        out_report->sab_byte_length = byte_length_;
        out_report->tid1 = cell.tid;
        out_report->is_write1 = cell.is_write;
        out_report->epoch1 = cell.epoch;
        out_report->tid2 = tid;
        out_report->is_write2 = is_write;
        out_report->epoch2 = thread_clock.Get(tid);
      }
      found_race = true;
      break;
    }

    uint32_t evict_slot = tid % kShadowCellsPerByte;
    ShadowCell& evict = page->cells[InPageShadowIndex(in_page, evict_slot)];
    evict.tid = tid;
    evict.epoch = thread_clock.Get(tid);
    evict.is_write = is_write;
    evict.valid = true;

    if (found_race)
      return true;
  }

  return false;
}

void ShadowBuffer::ClearRange(size_t offset, size_t size) {
  size_t end = offset + size;
  if (end > byte_length_)
    end = byte_length_;

  // Iterate by page to skip unallocated pages entirely.
  for (size_t byte = offset; byte < end;) {
    size_t page_idx = PageIndex(byte);
    ShadowPage* page;
    {
      base::AutoLock plock(page_lock_);
      auto it = pages_.find(page_idx);
      if (it == pages_.end()) {
        // Skip to next page boundary.
        byte = (page_idx + 1) * kPageBytes;
        continue;
      }
      page = it->second.get();
    }

    // Clear cells within this page, under stripe locks.
    size_t page_end = std::min(end, (page_idx + 1) * kPageBytes);
    for (; byte < page_end; byte++) {
      size_t stripe = StripeIndex(byte);
      base::AutoLock slock(stripe_locks_[stripe]);
      size_t in_page = InPageOffset(byte);
      for (uint32_t c = 0; c < kShadowCellsPerByte; c++) {
        page->cells[InPageShadowIndex(in_page, c)].valid = false;
      }
    }
  }
}

// --------------------------------------------------------------------------
// WebTSan singleton
// --------------------------------------------------------------------------

WebTSan& WebTSan::Get() {
  static base::NoDestructor<WebTSan> instance;
  return *instance;
}

WebTSan::WebTSan() {
  thread_alive_.fill(false);
}

void WebTSan::Reset() {
  base::AutoLock t(thread_lock_);
  base::AutoLock s(sync_lock_);
  base::AutoLock h(hb_lock_);
  base::AutoLock sh(shadow_lock_);

  for (uint32_t i = 0; i < kMaxThreads; i++) {
    thread_clocks_[i] = VClock();
    thread_alive_[i] = false;
  }
  next_tid_ = 0;
  sync_clocks_.clear();
  hb_edges_.clear();
  next_edge_id_ = 1;
  shadow_buffers_.clear();
  races_detected_ = 0;
  accesses_checked_ = 0;
  accesses_suppressed_ = 0;
  orphan_clock_ = VClock();
  {
    base::AutoLock rlock(reported_race_sites_lock_);
    reported_race_sites_.clear();
  }
}

// --------------------------------------------------------------------------
// Thread lifecycle
// --------------------------------------------------------------------------

uint32_t WebTSan::ThreadCreate(uint32_t parent_tid) {
  base::AutoLock lock(thread_lock_);
  uint32_t tid = next_tid_;

  // Find next free slot (wrap around).
  for (uint32_t i = 0; i < kMaxThreads; i++) {
    uint32_t candidate = (next_tid_ + i) % kMaxThreads;
    if (!thread_alive_[candidate]) {
      tid = candidate;
      break;
    }
  }

  CHECK_LT(tid, kMaxThreads) << "WebTSan: too many concurrent threads";
  thread_alive_[tid] = true;
  // Inherit parent's clock so HB edges the parent acquired transitively
  // propagate to the child. This is what real TSan does at pthread_create —
  // without it, every access by a child to memory the parent synchronized
  // with would look racy.
  if (parent_tid != UINT32_MAX && parent_tid < kMaxThreads &&
      thread_alive_[parent_tid]) {
    thread_clocks_[tid] = thread_clocks_[parent_tid];
  } else {
    thread_clocks_[tid] = VClock();
  }
  // Also join the orphan clock so we observe all detached-predecessor writes.
  thread_clocks_[tid].Join(orphan_clock_);
  thread_clocks_[tid].Increment(tid);  // tick for the child's own first event
  next_tid_ = (tid + 1) % kMaxThreads;

  LOG(INFO) << "WebTSan: thread created, tid=" << tid
            << " parent_tid=" << parent_tid;
  return tid;
}

void WebTSan::ThreadFinish(uint32_t tid) {
  if (!enabled_) return;
  base::AutoLock lock(thread_lock_);
  if (tid >= kMaxThreads) return;
  // Publish this thread's final clock:
  // 1. Into the global orphan clock so future thread creations acquire it.
  // 2. Into every currently-alive thread's clock — this is the crucial
  //    step for detached threads: without it, a long-running main thread
  //    that never performs another atomic op with this thread's address
  //    would never observe its final writes. Broadcasting here mirrors
  //    what pthread_join would have done if anyone had joined.
  const VClock& finishing = thread_clocks_[tid];
  orphan_clock_.Join(finishing);
  for (uint32_t i = 0; i < kMaxThreads; ++i) {
    if (i == tid || !thread_alive_[i]) continue;
    thread_clocks_[i].Join(finishing);
  }
}

void WebTSan::ThreadDestroy(uint32_t tid) {
  if (enabled_) {
    // Ensure ThreadFinish semantics even if caller didn't invoke it.
    base::AutoLock lock(thread_lock_);
    if (tid < kMaxThreads && thread_alive_[tid]) {
      orphan_clock_.Join(thread_clocks_[tid]);
    }
    thread_alive_[tid] = false;
  } else {
    base::AutoLock lock(thread_lock_);
    if (tid < kMaxThreads) thread_alive_[tid] = false;
  }
  LOG(INFO) << "WebTSan: thread destroyed, tid=" << tid;
}

// --------------------------------------------------------------------------
// Memory access
// --------------------------------------------------------------------------

void WebTSan::MemoryAccess(void* backing_store, size_t sab_byte_length,
                           size_t offset, size_t size, uint32_t tid,
                           bool is_write) {
  if (!enabled_)
    return;

  accesses_checked_++;

  // NOTE: we deliberately do NOT bump the thread's clock on every access.
  // Real TSan ticks per-event, not per-byte; per-byte ticking under a
  // global lock is catastrophically slow (15x overall slowdown observed).
  // The HB model only requires the clock to advance on SYNC ops (Release,
  // Acquire, MutexUnlock, ThreadCreate, Barrier, etc.), which we do.
  // Shadow cells record the thread's current epoch; same-tid writes get
  // the same epoch but are skipped by the race check anyway.

  // If byte_length is 0 (WASM path doesn't always know it), check if
  // we already have a shadow for this backing store. If not, skip --
  // the shadow will be created on the first JS-side access that knows
  // the length, or from the WASM instantiation hook.
  ShadowBuffer* shadow;
  if (sab_byte_length == 0) {
    base::AutoLock lock(shadow_lock_);
    auto it = shadow_buffers_.find(backing_store);
    if (it == shadow_buffers_.end())
      return;  // shadow not yet created, can't track this access
    shadow = it->second.get();
  } else {
    shadow = GetOrCreateShadow(backing_store, sab_byte_length);
  }

  VClock clock_snapshot;
  {
    base::AutoLock lock(thread_lock_);
    clock_snapshot = thread_clocks_[tid];
  }

  RaceReport report;
  if (shadow->RecordAccess(offset, size, tid, is_write, clock_snapshot,
                           &report)) {
    // Per-site dedup: a single genuine race on a shared global re-triggers
    // on every access. Without dedup, the console spam + JS-stack capture
    // overhead dwarfs the actual workload and breaks timing-sensitive
    // code (pipes, fetch timeouts).  Key by (backing_store, 8-byte bucket).
    uint64_t site_key =
        (reinterpret_cast<uintptr_t>(backing_store) ^
         (static_cast<uint64_t>(report.sab_offset) & ~7ull));
    {
      base::AutoLock lock(reported_race_sites_lock_);
      if (!reported_race_sites_.insert(site_key).second) {
        return;  // already reported this site
      }
    }

    races_detected_++;
    std::string msg = report.ToString();

    // Only log the very first race to prevent unbounded log spam.
    bool expected = false;
    if (first_race_logged_.compare_exchange_strong(expected, true)) {
      LOG(ERROR) << "WebTSan: " << msg;
    }

    // Store race info in thread-local for V8 to pick up and throw.
    g_race_detected = true;
    snprintf(g_race_message, sizeof(g_race_message), "%s", msg.c_str());

    if (race_callback_) {
      race_callback_(report);
    }
  }
}

// --------------------------------------------------------------------------
// Synchronization: acquire / release
// --------------------------------------------------------------------------

void WebTSan::Acquire(uint32_t tid, uintptr_t sync_addr) {
  if (!enabled_)
    return;

  // Lock ordering: thread_lock_ before sync_lock_ (must match Release).
  base::AutoLock tlock(thread_lock_);
  base::AutoLock slock(sync_lock_);
  auto it = sync_clocks_.find(sync_addr);
  if (it == sync_clocks_.end())
    return;  // no prior release on this address

  // Join the sync address's clock into the acquiring thread's clock.
  thread_clocks_[tid].Join(it->second);
}

void WebTSan::Release(uint32_t tid, uintptr_t sync_addr) {
  if (!enabled_)
    return;

  base::AutoLock tlock(thread_lock_);
  base::AutoLock slock(sync_lock_);
  // Store (join into) the thread's current clock at this sync address,
  // then tick so future accesses by this thread have a strictly later
  // epoch than what we just published. Without the tick, subsequent
  // writes would share the epoch of the release and a separate acquire
  // chain couldn't distinguish them.
  sync_clocks_[sync_addr].Join(thread_clocks_[tid]);
  thread_clocks_[tid].Increment(tid);
}

// --------------------------------------------------------------------------
// Mutex primitives (LLVM TSan-style: sync clocks keyed on user addresses)
// --------------------------------------------------------------------------

void WebTSan::MutexCreate(uintptr_t addr) {
  if (!enabled_) return;
  // Clear any stale sync clock from a previous mutex that lived at this
  // address (happens on malloc/free address reuse). A fresh mutex must not
  // inherit HB edges from a destroyed predecessor.
  base::AutoLock slock(sync_lock_);
  sync_clocks_.erase(addr);
}

void WebTSan::MutexDestroy(uintptr_t addr) {
  if (!enabled_) return;
  base::AutoLock slock(sync_lock_);
  sync_clocks_.erase(addr);
}

void WebTSan::MutexPreLock(uint32_t, uintptr_t) {
  // no-op (would host deadlock detection in real TSan)
}

void WebTSan::MutexPostLock(uint32_t tid, uintptr_t addr) {
  if (!enabled_ || tid >= kMaxThreads) return;
  base::AutoLock tlock(thread_lock_);
  base::AutoLock slock(sync_lock_);
  auto it = sync_clocks_.find(addr);
  if (it != sync_clocks_.end())
    thread_clocks_[tid].Join(it->second);
}

void WebTSan::MutexUnlock(uint32_t tid, uintptr_t addr) {
  if (!enabled_ || tid >= kMaxThreads) return;
  base::AutoLock tlock(thread_lock_);
  base::AutoLock slock(sync_lock_);
  sync_clocks_[addr].Join(thread_clocks_[tid]);
  thread_clocks_[tid].Increment(tid);
}

void WebTSan::MutexReadLock(uint32_t tid, uintptr_t addr) {
  if (!enabled_ || tid >= kMaxThreads) return;
  base::AutoLock tlock(thread_lock_);
  base::AutoLock slock(sync_lock_);
  auto it = sync_clocks_.find(addr);
  if (it != sync_clocks_.end())
    thread_clocks_[tid].Join(it->second);
}

void WebTSan::MutexReadUnlock(uint32_t, uintptr_t) {
  // reader release does NOT publish -- readers don't mutate the protected
  // state, so no HB edge out to future writers. Real TSan matches this.
}

// --------------------------------------------------------------------------
// Thread join / detach
// --------------------------------------------------------------------------

void WebTSan::ThreadJoin(uint32_t joiner_tid, uint32_t child_tid) {
  if (!enabled_) return;
  if (joiner_tid >= kMaxThreads || child_tid >= kMaxThreads) return;
  base::AutoLock lock(thread_lock_);
  thread_clocks_[joiner_tid].Join(thread_clocks_[child_tid]);
  thread_alive_[child_tid] = false;
}

void WebTSan::ThreadDetach(uint32_t child_tid) {
  if (!enabled_) return;
  if (child_tid >= kMaxThreads) return;
  base::AutoLock lock(thread_lock_);
  // Detached threads' final clocks reach future threads via orphan_clock_
  // at ThreadFinish time. Nothing else to do here.
}

void WebTSan::ThreadAbsorb(uint32_t dst_tid, uint32_t src_tid) {
  if (!enabled_) return;
  if (dst_tid >= kMaxThreads || src_tid >= kMaxThreads) return;
  if (dst_tid == src_tid) return;
  base::AutoLock lock(thread_lock_);
  thread_clocks_[dst_tid].Join(thread_clocks_[src_tid]);
}

// --------------------------------------------------------------------------
// Happens-before edges (for postMessage)
// --------------------------------------------------------------------------

uint64_t WebTSan::HappensBefore(uint32_t sender_tid) {
  if (!enabled_)
    return 0;

  // Lock ordering: thread_lock_ before hb_lock_.
  base::AutoLock tlock(thread_lock_);
  base::AutoLock hlock(hb_lock_);

  uint64_t id = next_edge_id_++;
  hb_edges_[id] = thread_clocks_[sender_tid];
  return id;
}

void WebTSan::HappensAfter(uint32_t receiver_tid, uint64_t edge_id) {
  if (!enabled_ || edge_id == 0)
    return;

  // Lock ordering: thread_lock_ before hb_lock_.
  base::AutoLock tlock(thread_lock_);
  base::AutoLock hlock(hb_lock_);
  auto it = hb_edges_.find(edge_id);
  if (it == hb_edges_.end())
    return;

  thread_clocks_[receiver_tid].Join(it->second);
  hb_edges_.erase(it);  // one-shot: each edge consumed once
}

// --------------------------------------------------------------------------
// Shadow buffer management
// --------------------------------------------------------------------------

void WebTSan::RemoveShadow(void* backing_store) {
  if (!enabled_)
    return;

  base::AutoLock lock(shadow_lock_);
  auto it = shadow_buffers_.find(backing_store);
  if (it != shadow_buffers_.end()) {
    LOG(INFO) << "WebTSan: removing shadow for SAB at " << backing_store;
    shadow_buffers_.erase(it);
  }
}

void WebTSan::ClearShadow(void* backing_store, size_t offset, size_t size) {
  if (!enabled_)
    return;

  base::AutoLock lock(shadow_lock_);
  auto it = shadow_buffers_.find(backing_store);
  if (it == shadow_buffers_.end())
    return;

  it->second->ClearRange(offset, size);
}

ShadowBuffer* WebTSan::GetOrCreateShadow(void* backing_store,
                                          size_t byte_length) {
  base::AutoLock lock(shadow_lock_);
  auto it = shadow_buffers_.find(backing_store);
  if (it != shadow_buffers_.end())
    return it->second.get();

  auto shadow = std::make_unique<ShadowBuffer>(backing_store, byte_length);
  ShadowBuffer* raw = shadow.get();
  shadow_buffers_[backing_store] = std::move(shadow);
  LOG(INFO) << "WebTSan: created shadow for SAB at " << backing_store
            << " (" << byte_length << " bytes, shadow="
            << (byte_length * kShadowCellsPerByte * sizeof(ShadowCell))
            << " bytes)";
  return raw;
}

}  // namespace tsan
}  // namespace blink
