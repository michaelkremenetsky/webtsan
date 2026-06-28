// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// WebTSan hooks -- lightweight inline functions that Chromium/V8 code calls
// at interception points. These forward to WebTSan::Get() when enabled.
//
// Designed to be zero-cost when disabled (the enabled check is a single
// atomic load that the branch predictor will learn quickly).

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_TSAN_WEB_TSAN_HOOKS_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_TSAN_WEB_TSAN_HOOKS_H_

#include "third_party/blink/renderer/core/tsan/web_tsan.h"
#include "v8/include/v8.h"

// Thread-local storage for the WebTSan thread ID.
// Each Worker runs on its own OS thread, so thread_local gives us
// correct per-worker identity without patching V8's Isolate class.
namespace blink {
namespace tsan {

inline thread_local uint32_t g_current_tsan_tid = UINT32_MAX;

// Set after a race is detected; read and cleared by V8 runtime functions
// to throw a JS error. Using thread_local avoids locking.
inline thread_local bool g_race_detected = false;
inline thread_local char g_race_message[512] = {};

// Depth-counted non-atomic-access suppression for the current thread.
// Incremented on entry to allocator/libc/internal critical sections, decremented
// on exit. Atomic operations ignore this counter.
inline thread_local uint32_t g_current_suppress_depth = 0;

// Thread-local reentrance guard for race reporting. Set while we're
// inside the console.error call path so recursive SAB accesses triggered
// by the reporter itself don't fire new race reports (which would recurse
// into the reporter again).
inline thread_local bool g_in_race_report = false;

inline uint32_t WebTSanGetCurrentTid() {
  return g_current_tsan_tid;
}

inline void WebTSanSetCurrentTid(uint32_t tid) {
  g_current_tsan_tid = tid;
}

inline void WebTSanPushSuppress() { ++g_current_suppress_depth; }
inline void WebTSanPopSuppress() {
  if (g_current_suppress_depth) --g_current_suppress_depth;
}
inline bool WebTSanIsSuppressed() { return g_current_suppress_depth > 0; }
inline void WebTSanResetSuppress() { g_current_suppress_depth = 0; }

// --------------------------------------------------------------------------
// SAB memory access hooks (called from V8 typed array / WASM code paths)
// --------------------------------------------------------------------------

inline void OnSABAccess(void* backing_store, size_t sab_byte_length,
                        size_t offset, size_t size, uint32_t tid,
                        bool is_write) {
  if (!WebTSan::Get().IsEnabled()) return;
  if (WebTSanIsSuppressed()) {
    WebTSan::Get().IncrementSuppressedCount();
    return;
  }
  // Reentrance guard: while we're inside console.error reporting a race,
  // any SAB access the reporter itself performs must not fire another race.
  if (g_in_race_report) return;
  WebTSan::Get().MemoryAccess(backing_store, sab_byte_length, offset, size,
                              tid, is_write);
}

// Unconditional range record (never suppressed). Used by memcpy/memset
// interceptors to record the user-visible range before entering a
// suppress-wrapped body.
inline void OnSABRangeUnsuppressed(void* backing_store, size_t sab_byte_length,
                                    size_t offset, size_t size, uint32_t tid,
                                    bool is_write) {
  if (!WebTSan::Get().IsEnabled()) return;
  WebTSan::Get().MemoryAccess(backing_store, sab_byte_length, offset, size,
                              tid, is_write);
}

// --------------------------------------------------------------------------
// Atomics hooks (called from futex-emulation.cc and builtins-sharedarraybuffer)
// --------------------------------------------------------------------------

// After Atomics.wait returns (thread was woken or value didn't match).
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

// Atomics.store = release semantics.
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
// These are both a load and a store -- acquire + release.
inline void OnAtomicsRMW(uint32_t tid, void* location) {
  if (WebTSan::Get().IsEnabled()) {
    WebTSan::Get().Acquire(tid, reinterpret_cast<uintptr_t>(location));
    WebTSan::Get().Release(tid, reinterpret_cast<uintptr_t>(location));
  }
}

// --------------------------------------------------------------------------
// postMessage hooks (called from message_port.cc)
// --------------------------------------------------------------------------

inline uint64_t OnPostMessageSend(uint32_t sender_tid) {
  if (WebTSan::Get().IsEnabled()) {
    return WebTSan::Get().HappensBefore(sender_tid);
  }
  return 0;
}

inline void OnPostMessageReceive(uint32_t receiver_tid, uint64_t edge_id) {
  if (WebTSan::Get().IsEnabled()) {
    WebTSan::Get().HappensAfter(receiver_tid, edge_id);
  }
}

// --------------------------------------------------------------------------
// Mutex hooks (called from emscripten musl pthread_mutex_*.c via JS shim)
// --------------------------------------------------------------------------

inline void OnMutexCreate(uintptr_t addr) {
  if (WebTSan::Get().IsEnabled()) WebTSan::Get().MutexCreate(addr);
}
inline void OnMutexDestroy(uintptr_t addr) {
  if (WebTSan::Get().IsEnabled()) WebTSan::Get().MutexDestroy(addr);
}
inline void OnMutexPreLock(uint32_t tid, uintptr_t addr) {
  if (WebTSan::Get().IsEnabled()) WebTSan::Get().MutexPreLock(tid, addr);
}
inline void OnMutexPostLock(uint32_t tid, uintptr_t addr) {
  if (WebTSan::Get().IsEnabled()) WebTSan::Get().MutexPostLock(tid, addr);
}
inline void OnMutexUnlock(uint32_t tid, uintptr_t addr) {
  if (WebTSan::Get().IsEnabled()) WebTSan::Get().MutexUnlock(tid, addr);
}
inline void OnMutexReadLock(uint32_t tid, uintptr_t addr) {
  if (WebTSan::Get().IsEnabled()) WebTSan::Get().MutexReadLock(tid, addr);
}
inline void OnMutexReadUnlock(uint32_t tid, uintptr_t addr) {
  if (WebTSan::Get().IsEnabled()) WebTSan::Get().MutexReadUnlock(tid, addr);
}

// Thread lifecycle: pthread_create / pthread_join / pthread_detach.
inline uint32_t OnPthreadCreate(uint32_t parent_tid) {
  return WebTSan::Get().ThreadCreate(parent_tid);
}
inline void OnPthreadJoin(uint32_t joiner_tid, uint32_t child_tid) {
  if (WebTSan::Get().IsEnabled())
    WebTSan::Get().ThreadJoin(joiner_tid, child_tid);
}
inline void OnPthreadDetach(uint32_t child_tid) {
  if (WebTSan::Get().IsEnabled()) WebTSan::Get().ThreadDetach(child_tid);
}

// --------------------------------------------------------------------------
// Worker lifecycle hooks (called from worker_thread.cc)
// --------------------------------------------------------------------------

inline uint32_t OnWorkerCreate() {
  return WebTSan::Get().ThreadCreate();
}

inline void OnWorkerDestroy(uint32_t tid) {
  WebTSan::Get().ThreadDestroy(tid);
}

// Install WebTSan globals on the given V8 context:
//   __webTSanClearShadow(buffer, offset, size)
//   __webTSanPushSuppress()
//   __webTSanPopSuppress()
//   __webTSanRecordRange(buffer, offset, size, isWrite)
// Called from worker/main thread/iframe init after the V8 context is created.
void InstallWebTSanGlobals(v8::Isolate* isolate,
                           v8::Local<v8::Context> context);

}  // namespace tsan
}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_TSAN_WEB_TSAN_HOOKS_H_
