// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/tsan/web_tsan.h"

#include <cstdio>
#include <cstring>

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
      shadow_(byte_length * kShadowCellsPerByte),
      stripe_locks_((byte_length + kStripeBytes - 1) / kStripeBytes) {}

ShadowBuffer::~ShadowBuffer() = default;

bool ShadowBuffer::RecordAccess(size_t offset, size_t size, uint32_t tid,
                                bool is_write, const VClock& thread_clock,
                                RaceReport* out_report) {
  // Check each byte in the access range. For multi-byte accesses, any byte
  // conflict is a race. We only report the first one.
  for (size_t i = 0; i < size && (offset + i) < byte_length_; i++) {
    size_t byte = offset + i;
    size_t stripe = StripeIndex(byte);
    base::AutoLock lock(stripe_locks_[stripe]);

    bool found_race = false;

    // Check existing shadow cells for conflicts.
    for (uint32_t c = 0; c < kShadowCellsPerByte; c++) {
      size_t idx = ShadowIndex(byte, c);
      ShadowCell& cell = shadow_[idx];

      if (!cell.valid)
        continue;

      // Same thread — no conflict possible.
      if (cell.tid == tid)
        continue;

      // Two reads — no conflict.
      if (!cell.is_write && !is_write)
        continue;

      // Conflicting access. Check happens-before.
      // The stored epoch is the value of cell.tid's clock component at the
      // time of cell's access. If thread_clock[cell.tid] >= cell.epoch,
      // then the current thread has synchronized with (happens-after) the
      // stored access.
      if (thread_clock.Get(cell.tid) >= cell.epoch) {
        // Ordered. Not a race.
        continue;
      }

      // RACE DETECTED.
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

    // Store the new access in a shadow cell (evict oldest / empty slot).
    // Simple round-robin eviction based on tid to spread across cells.
    uint32_t evict_slot = tid % kShadowCellsPerByte;
    size_t evict_idx = ShadowIndex(byte, evict_slot);
    shadow_[evict_idx].tid = tid;
    shadow_[evict_idx].epoch = thread_clock.Get(tid);
    shadow_[evict_idx].is_write = is_write;
    shadow_[evict_idx].valid = true;

    if (found_race)
      return true;
  }

  return false;
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
}

// --------------------------------------------------------------------------
// Thread lifecycle
// --------------------------------------------------------------------------

uint32_t WebTSan::ThreadCreate() {
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
  thread_clocks_[tid] = VClock();
  thread_clocks_[tid].Increment(tid);  // epoch starts at 1
  next_tid_ = (tid + 1) % kMaxThreads;

  LOG(INFO) << "WebTSan: thread created, tid=" << tid;
  return tid;
}

void WebTSan::ThreadDestroy(uint32_t tid) {
  base::AutoLock lock(thread_lock_);
  CHECK_LT(tid, kMaxThreads);
  thread_alive_[tid] = false;
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

  // Tick the thread's clock on each access.
  {
    base::AutoLock lock(thread_lock_);
    thread_clocks_[tid].Increment(tid);
  }

  // If byte_length is 0 (WASM path doesn't always know it), check if
  // we already have a shadow for this backing store. If not, skip —
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
    races_detected_++;
    std::string msg = report.ToString();
    LOG(ERROR) << "WebTSan: " << msg;

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

  base::AutoLock slock(sync_lock_);
  auto it = sync_clocks_.find(sync_addr);
  if (it == sync_clocks_.end())
    return;  // no prior release on this address

  base::AutoLock tlock(thread_lock_);
  // Join the sync address's clock into the acquiring thread's clock.
  thread_clocks_[tid].Join(it->second);
}

void WebTSan::Release(uint32_t tid, uintptr_t sync_addr) {
  if (!enabled_)
    return;

  base::AutoLock tlock(thread_lock_);
  base::AutoLock slock(sync_lock_);
  // Store (join into) the thread's current clock at this sync address.
  sync_clocks_[sync_addr].Join(thread_clocks_[tid]);
}

// --------------------------------------------------------------------------
// Happens-before edges (for postMessage)
// --------------------------------------------------------------------------

uint64_t WebTSan::HappensBefore(uint32_t sender_tid) {
  if (!enabled_)
    return 0;

  base::AutoLock hlock(hb_lock_);
  base::AutoLock tlock(thread_lock_);

  uint64_t id = next_edge_id_++;
  hb_edges_[id] = thread_clocks_[sender_tid];
  return id;
}

void WebTSan::HappensAfter(uint32_t receiver_tid, uint64_t edge_id) {
  if (!enabled_ || edge_id == 0)
    return;

  base::AutoLock hlock(hb_lock_);
  auto it = hb_edges_.find(edge_id);
  if (it == hb_edges_.end())
    return;

  base::AutoLock tlock(thread_lock_);
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
