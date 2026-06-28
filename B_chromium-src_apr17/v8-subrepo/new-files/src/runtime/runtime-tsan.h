// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_RUNTIME_RUNTIME_TSAN_H_
#define V8_RUNTIME_RUNTIME_TSAN_H_

#include <cstddef>
#include <cstdint>

// Forward declarations for V8 API types used in callbacks.
namespace v8 {
class Isolate;
class Context;
template <class T> class Local;
}  // namespace v8

// V8_EXPORT_PRIVATE equivalent for cross-component visibility.
// In component builds, this ensures the symbols are exported from V8's dylib.
#if defined(COMPONENT_BUILD)
#if defined(BUILDING_V8_SHARED_PRIVATE)
#define WEB_TSAN_EXPORT __attribute__((visibility("default")))
#else
#define WEB_TSAN_EXPORT __attribute__((visibility("default")))
#endif
#else
#define WEB_TSAN_EXPORT
#endif

namespace v8 {
namespace internal {
namespace web_tsan {

struct WebTSanCallbacks {
  uint32_t (*get_current_tid)() = nullptr;
  void (*on_sab_access)(void*, size_t, size_t, size_t, uint32_t,
                        uint32_t) = nullptr;
  void (*on_atomics_load)(uint32_t, void*) = nullptr;
  void (*on_atomics_store)(uint32_t, void*) = nullptr;
  void (*on_atomics_rmw)(uint32_t, void*) = nullptr;
  void (*on_atomics_wait_complete)(uint32_t, void*) = nullptr;
  void (*on_atomics_notify)(uint32_t, void*) = nullptr;
  void (*remove_shadow)(void*) = nullptr;
  void (*wasm_access)(void*, size_t, uintptr_t, uint32_t, uint32_t) = nullptr;
  // Clear shadow cells for a range within a SAB (called from dlmalloc free).
  void (*clear_shadow)(void* backing_store, size_t offset, size_t size) =
      nullptr;
  // Returns true if a race was detected on the current thread, and fills
  // msg_buf (up to msg_buf_size) with the message. Clears the flag.
  bool (*consume_race)(char* msg_buf, size_t msg_buf_size) = nullptr;
  // Install WebTSan globals on a V8 context:
  //   __webTSanClearShadow(buffer, offset, size)
  //   __webTSanPushSuppress()
  //   __webTSanPopSuppress()
  //   __webTSanRecordRange(buffer, offset, size, isWrite)
  // Called lazily on the first WASM access per context (and eagerly from
  // worker/iframe init sites).
  void (*install_webtsan_globals)(v8::Isolate* isolate,
                                  v8::Local<v8::Context> context) = nullptr;

  // Depth-counted suppression of non-atomic SAB access recording on the
  // current thread. Atomic operations (load/store/RMW/futex) are never
  // suppressed. Called from WASM interceptors around dlmalloc/__lock/etc.
  void (*push_suppress)(uint32_t tid) = nullptr;
  void (*pop_suppress)(uint32_t tid) = nullptr;

  // Record a (possibly-strided) memory range as an access. Bypasses suppress
  // (callers use it to record user-visible access *before* entering suppress).
  void (*record_range)(void* backing_store, size_t sab_byte_length,
                       size_t offset, size_t size, uint32_t tid,
                       uint32_t is_write) = nullptr;

  // Mutex primitives (LLVM TSan model): sync clocks keyed on user addresses.
  // `addr` is a uintptr_t carrying the wasm user-space pointer to the
  // pthread_mutex_t / rwlock object.
  void (*mutex_create)(uintptr_t addr) = nullptr;
  void (*mutex_destroy)(uintptr_t addr) = nullptr;
  void (*mutex_pre_lock)(uint32_t tid, uintptr_t addr) = nullptr;
  void (*mutex_post_lock)(uint32_t tid, uintptr_t addr) = nullptr;
  void (*mutex_unlock)(uint32_t tid, uintptr_t addr) = nullptr;
  void (*mutex_read_lock)(uint32_t tid, uintptr_t addr) = nullptr;
  void (*mutex_read_unlock)(uint32_t tid, uintptr_t addr) = nullptr;

  // Thread lifecycle driven by emscripten pthread_create/join/detach.
  // pthread_create returns a fresh WebTSan tid the caller will store on
  // the child pthread_t; the child calls set_current_tid with it at start.
  uint32_t (*pthread_create)(uint32_t parent_tid) = nullptr;
  void (*set_current_tid)(uint32_t tid) = nullptr;
  void (*pthread_join)(uint32_t joiner_tid, uint32_t child_tid) = nullptr;
  void (*pthread_detach)(uint32_t child_tid) = nullptr;
};

WEB_TSAN_EXPORT extern WebTSanCallbacks g_callbacks;
WEB_TSAN_EXPORT void RegisterCallbacks(const WebTSanCallbacks& cb);

}  // namespace web_tsan
}  // namespace internal
}  // namespace v8

#endif  // V8_RUNTIME_RUNTIME_TSAN_H_
