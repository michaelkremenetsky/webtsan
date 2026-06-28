// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// WebTSan hooks — lightweight inline functions that Chromium/V8 code calls
// at interception points. These forward to WebTSan::Get() when enabled.
//
// Designed to be zero-cost when disabled (the enabled check is a single
// atomic load that the branch predictor will learn quickly).

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_TSAN_WEB_TSAN_HOOKS_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_TSAN_WEB_TSAN_HOOKS_H_

#include "third_party/blink/renderer/core/tsan/web_tsan.h"

// Thread-local storage for the WebTSan thread ID.
// Each Worker runs on its own OS thread, so thread_local gives us
// correct per-worker identity without patching V8's Isolate class.
namespace blink {
namespace tsan {

inline thread_local uint32_t g_current_tsan_tid = UINT32_MAX;

inline uint32_t WebTSanGetCurrentTid() {
  return g_current_tsan_tid;
}

inline void WebTSanSetCurrentTid(uint32_t tid) {
  g_current_tsan_tid = tid;
}

// --------------------------------------------------------------------------
// SAB memory access hooks (called from V8 typed array / WASM code paths)
// --------------------------------------------------------------------------

// `backing_store` = the SAB's raw host pointer (BackingStore::buffer_start())
// `offset`        = byte offset into the SAB
// `size`          = access size in bytes (1, 2, 4, 8)
// `tid`           = the calling thread's WebTSan tid
// `is_write`      = true for stores, false for loads
inline void OnSABAccess(void* backing_store, size_t sab_byte_length,
                        size_t offset, size_t size, uint32_t tid,
                        bool is_write) {
  if (WebTSan::Get().IsEnabled()) {
    WebTSan::Get().MemoryAccess(backing_store, sab_byte_length, offset, size,
                                tid, is_write);
  }
}

// --------------------------------------------------------------------------
// Atomics hooks (called from futex-emulation.cc and builtins-sharedarraybuffer)
// --------------------------------------------------------------------------

// After Atomics.wait returns (thread was woken or value didn't match).
// sync_addr = the SAB byte address that was waited on.
inline void OnAtomicsWaitComplete(uint32_t tid, void* wait_location) {
  if (WebTSan::Get().IsEnabled()) {
    WebTSan::Get().Acquire(tid, reinterpret_cast<uintptr_t>(wait_location));
  }
}

// Before Atomics.notify wakes waiters.
inline void OnAtomicsNotify(uint32_t tid, void* wait_location) {
  if (WebTSan::Get().IsEnabled()) {
    WebTSan::Get().Release(tid, reinterpret_cast<uintptr_t>(wait_location));
  }
}

// Atomics.store = release semantics (the stored value is visible to a
// subsequent Atomics.load on another thread).
inline void OnAtomicsStore(uint32_t tid, void* location) {
  if (WebTSan::Get().IsEnabled()) {
    WebTSan::Get().Release(tid, reinterpret_cast<uintptr_t>(location));
  }
}

// Atomics.load = acquire semantics.
inline void OnAtomicsLoad(uint32_t tid, void* location) {
  if (WebTSan::Get().IsEnabled()) {
    WebTSan::Get().Acquire(tid, reinterpret_cast<uintptr_t>(location));
  }
}

// Atomics.compareExchange, Atomics.exchange, Atomics.add, etc.
// These are both a load and a store — acquire + release.
inline void OnAtomicsRMW(uint32_t tid, void* location) {
  if (WebTSan::Get().IsEnabled()) {
    WebTSan::Get().Acquire(tid, reinterpret_cast<uintptr_t>(location));
    WebTSan::Get().Release(tid, reinterpret_cast<uintptr_t>(location));
  }
}

// --------------------------------------------------------------------------
// postMessage hooks (called from message_port.cc)
// --------------------------------------------------------------------------

// Called on the sender side before posting. Returns an edge_id to embed in
// the message.
inline uint64_t OnPostMessageSend(uint32_t sender_tid) {
  if (WebTSan::Get().IsEnabled()) {
    return WebTSan::Get().HappensBefore(sender_tid);
  }
  return 0;
}

// Called on the receiver side when the message is dispatched.
inline void OnPostMessageReceive(uint32_t receiver_tid, uint64_t edge_id) {
  if (WebTSan::Get().IsEnabled()) {
    WebTSan::Get().HappensAfter(receiver_tid, edge_id);
  }
}

// --------------------------------------------------------------------------
// Worker lifecycle hooks (called from dedicated_worker.cc)
// --------------------------------------------------------------------------

inline uint32_t OnWorkerCreate() {
  return WebTSan::Get().ThreadCreate();
}

inline void OnWorkerDestroy(uint32_t tid) {
  WebTSan::Get().ThreadDestroy(tid);
}

}  // namespace tsan
}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_TSAN_WEB_TSAN_HOOKS_H_
