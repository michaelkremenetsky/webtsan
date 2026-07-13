// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/core/tsan/web_tsan.h"
#include "third_party/blink/renderer/core/tsan/web_tsan_hooks.h"

#include "base/command_line.h"
#include "base/logging.h"
#include "v8/src/runtime/runtime-tsan.h"

namespace blink {
namespace tsan {

const char kEnableWebTSan[] = "enable-web-tsan";

CORE_EXPORT void InitializeWebTSan() {
  if (!base::CommandLine::ForCurrentProcess()->HasSwitch(kEnableWebTSan)) {
    return;
  }

  // Register callback table with V8 so it can call our hooks across the
  // component-build dylib boundary.
  v8::internal::web_tsan::WebTSanCallbacks cb;
  cb.get_current_tid = []() -> uint32_t { return WebTSanGetCurrentTid(); };
  cb.on_sab_access = [](void* bs, size_t len, size_t off, size_t sz,
                        uint32_t tid, uint32_t is_write) {
    OnSABAccess(bs, len, off, sz, tid, is_write != 0);
  };
  cb.on_atomics_load = [](uint32_t tid, void* loc) {
    OnAtomicsLoad(tid, loc);
  };
  cb.on_atomics_store = [](uint32_t tid, void* loc) {
    OnAtomicsStore(tid, loc);
  };
  cb.on_atomics_rmw = [](uint32_t tid, void* loc) {
    OnAtomicsRMW(tid, loc);
  };
  cb.on_atomics_wait_complete = [](uint32_t tid, void* loc) {
    OnAtomicsWaitComplete(tid, loc);
  };
  cb.on_atomics_notify = [](uint32_t tid, void* loc) {
    OnAtomicsNotify(tid, loc);
  };
  cb.remove_shadow = [](void* bs) { WebTSan::Get().RemoveShadow(bs); };
  cb.clear_shadow = [](void* bs, size_t offset, size_t size) {
    WebTSan::Get().ClearShadow(bs, offset, size);
  };
  cb.wasm_access = [](void* bs, size_t len, uintptr_t off, uint32_t sz,
                      uint32_t is_store) {
    uint32_t tid = WebTSanGetCurrentTid();
    if (tid == UINT32_MAX) return;
    OnSABAccess(bs, len, off, sz, tid, is_store != 0);
  };
  cb.install_webtsan_globals =
      [](v8::Isolate* iso, v8::Local<v8::Context> ctx) {
        InstallWebTSanGlobals(iso, ctx);
      };
  cb.push_suppress = [](uint32_t /*tid*/) { WebTSanPushSuppress(); };
  cb.pop_suppress = [](uint32_t /*tid*/) { WebTSanPopSuppress(); };
  cb.record_range = [](void* bs, size_t len, size_t off, size_t sz,
                       uint32_t tid, uint32_t is_write) {
    OnSABRangeUnsuppressed(bs, len, off, sz, tid, is_write != 0);
  };
  cb.consume_race = [](char* buf, size_t buf_size) -> bool {
    if (!g_race_detected) return false;
    g_race_detected = false;
    snprintf(buf, buf_size, "%s", g_race_message);
    g_race_message[0] = '\0';
    return true;
  };
  cb.mutex_create = [](uintptr_t a) { OnMutexCreate(a); };
  cb.mutex_destroy = [](uintptr_t a) { OnMutexDestroy(a); };
  cb.mutex_pre_lock = [](uint32_t t, uintptr_t a) { OnMutexPreLock(t, a); };
  cb.mutex_post_lock = [](uint32_t t, uintptr_t a) { OnMutexPostLock(t, a); };
  cb.mutex_unlock = [](uint32_t t, uintptr_t a) { OnMutexUnlock(t, a); };
  cb.mutex_read_lock = [](uint32_t t, uintptr_t a) { OnMutexReadLock(t, a); };
  cb.mutex_read_unlock = [](uint32_t t, uintptr_t a) {
    OnMutexReadUnlock(t, a);
  };
  cb.pthread_create = [](uint32_t parent_tid) -> uint32_t {
    return OnPthreadCreate(parent_tid);
  };
  cb.set_current_tid = [](uint32_t tid) { WebTSanSetCurrentTid(tid); };
  cb.pthread_join = [](uint32_t j, uint32_t c) { OnPthreadJoin(j, c); };
  cb.pthread_detach = [](uint32_t c) { OnPthreadDetach(c); };
  v8::internal::web_tsan::RegisterCallbacks(cb);

  WebTSan::Get().Enable();

  uint32_t main_tid = WebTSan::Get().ThreadCreate();
  WebTSanSetCurrentTid(main_tid);

  LOG(WARNING) << "WebTSan: ENABLED. Main thread tid=" << main_tid
               << ". Race reports will appear in LOG(ERROR). "
               << "Use --jitless for full coverage of non-atomic SAB access.";
}

namespace {

void InstallFunction(
    v8::Isolate* isolate, v8::Local<v8::Context> context, const char* name,
    void (*cb)(const v8::FunctionCallbackInfo<v8::Value>&)) {
  v8::Local<v8::String> v8name =
      v8::String::NewFromUtf8(isolate, name).ToLocalChecked();
  v8::Local<v8::Value> existing;
  if (context->Global()->Get(context, v8name).ToLocal(&existing) &&
      existing->IsFunction()) {
    return;
  }
  v8::Local<v8::FunctionTemplate> tmpl = v8::FunctionTemplate::New(isolate, cb);
  v8::Local<v8::Function> func = tmpl->GetFunction(context).ToLocalChecked();
  context->Global()->Set(context, v8name, func).Check();
}

void ClearShadowCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (info.Length() < 3) return;
  v8::Isolate* iso = info.GetIsolate();
  if (!info[0]->IsSharedArrayBuffer()) return;
  v8::Local<v8::SharedArrayBuffer> sab = info[0].As<v8::SharedArrayBuffer>();
  void* backing_store = sab->GetBackingStore()->Data();
  uint32_t offset =
      info[1]->Uint32Value(iso->GetCurrentContext()).FromMaybe(0);
  uint32_t size =
      info[2]->Uint32Value(iso->GetCurrentContext()).FromMaybe(0);
  if (size > 0) {
    WebTSan::Get().ClearShadow(backing_store, offset, size);
  }
}

void PushSuppressCallback(const v8::FunctionCallbackInfo<v8::Value>&) {
  WebTSanPushSuppress();
}

void PopSuppressCallback(const v8::FunctionCallbackInfo<v8::Value>&) {
  WebTSanPopSuppress();
}

// Flush any pending race report to globalThis.console.error. Called from
// Blink V8 callbacks that can detect a race but are not followed by a
// Runtime_WasmTraceMemory (which would otherwise drain consume_race).
// Uses a thread-local reentrance guard because console.error itself runs
// JS/wasm that may do SAB access and come back through here.
void FlushPendingRaceToConsole(v8::Isolate* iso) {
  static thread_local bool s_in_flush = false;
  if (s_in_flush) return;
  if (!g_race_detected) return;
  s_in_flush = true;
  // Copy + clear the TLS race state first so the reporter's own SAB
  // accesses don't race with a second race being queued.
  std::string msg(g_race_message);
  g_race_detected = false;
  g_race_message[0] = '\0';
  v8::HandleScope handle_scope(iso);
  v8::Local<v8::Context> ctx = iso->GetCurrentContext();
  if (ctx.IsEmpty()) {
    s_in_flush = false;
    return;
  }
  v8::Context::Scope context_scope(ctx);
  v8::Local<v8::String> msg_v8 =
      v8::String::NewFromUtf8(iso, msg.c_str()).ToLocalChecked();
  v8::Local<v8::Object> global = ctx->Global();
  v8::Local<v8::Value> console_val;
  v8::Local<v8::String> console_key =
      v8::String::NewFromUtf8Literal(iso, "console");
  if (!global->Get(ctx, console_key).ToLocal(&console_val) ||
      !console_val->IsObject()) {
    s_in_flush = false;
    return;
  }
  v8::Local<v8::Object> console = console_val.As<v8::Object>();
  v8::Local<v8::String> error_key =
      v8::String::NewFromUtf8Literal(iso, "error");
  v8::Local<v8::Value> error_fn;
  if (!console->Get(ctx, error_key).ToLocal(&error_fn) ||
      !error_fn->IsFunction()) {
    s_in_flush = false;
    return;
  }
  v8::Local<v8::Value> argv[1] = {msg_v8};
  v8::TryCatch tc(iso);
  (void)error_fn.As<v8::Function>()->Call(ctx, console, 1, argv);
  // Swallow any exception from console.error — races must be non-fatal.
  if (tc.HasCaught()) tc.Reset();
  s_in_flush = false;
}

void RecordRangeCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (info.Length() < 4) return;
  v8::Isolate* iso = info.GetIsolate();
  if (!info[0]->IsSharedArrayBuffer()) return;
  v8::Local<v8::SharedArrayBuffer> sab = info[0].As<v8::SharedArrayBuffer>();
  auto backing = sab->GetBackingStore();
  void* backing_store = backing->Data();
  size_t byte_length = backing->ByteLength();
  uint32_t offset =
      info[1]->Uint32Value(iso->GetCurrentContext()).FromMaybe(0);
  uint32_t size =
      info[2]->Uint32Value(iso->GetCurrentContext()).FromMaybe(0);
  bool is_write = info[3]->BooleanValue(iso);
  if (size == 0) return;
  uint32_t tid = WebTSanGetCurrentTid();
  if (tid == UINT32_MAX) return;
  OnSABRangeUnsuppressed(backing_store, byte_length, offset, size, tid,
                         is_write);
  FlushPendingRaceToConsole(iso);
}

void ThreadFinishCallback(const v8::FunctionCallbackInfo<v8::Value>&) {
  uint32_t tid = WebTSanGetCurrentTid();
  if (tid == UINT32_MAX) return;
  WebTSan::Get().ThreadFinish(tid);
}

uintptr_t ArgToAddr(v8::Isolate* iso, v8::Local<v8::Value> v) {
  return static_cast<uintptr_t>(
      v->IntegerValue(iso->GetCurrentContext()).FromMaybe(0));
}

void MutexCreateCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (info.Length() < 1) return;
  OnMutexCreate(ArgToAddr(info.GetIsolate(), info[0]));
}
void MutexDestroyCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (info.Length() < 1) return;
  OnMutexDestroy(ArgToAddr(info.GetIsolate(), info[0]));
}
void MutexPreLockCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (info.Length() < 1) return;
  uint32_t tid = WebTSanGetCurrentTid();
  if (tid == UINT32_MAX) return;
  OnMutexPreLock(tid, ArgToAddr(info.GetIsolate(), info[0]));
}
void MutexPostLockCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (info.Length() < 1) return;
  uint32_t tid = WebTSanGetCurrentTid();
  if (tid == UINT32_MAX) return;
  OnMutexPostLock(tid, ArgToAddr(info.GetIsolate(), info[0]));
}
void MutexUnlockCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (info.Length() < 1) return;
  uint32_t tid = WebTSanGetCurrentTid();
  if (tid == UINT32_MAX) return;
  OnMutexUnlock(tid, ArgToAddr(info.GetIsolate(), info[0]));
}
void MutexReadLockCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (info.Length() < 1) return;
  uint32_t tid = WebTSanGetCurrentTid();
  if (tid == UINT32_MAX) return;
  OnMutexReadLock(tid, ArgToAddr(info.GetIsolate(), info[0]));
}
void MutexReadUnlockCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (info.Length() < 1) return;
  uint32_t tid = WebTSanGetCurrentTid();
  if (tid == UINT32_MAX) return;
  OnMutexReadUnlock(tid, ArgToAddr(info.GetIsolate(), info[0]));
}

void PthreadCreateCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
  uint32_t parent = WebTSanGetCurrentTid();
  uint32_t child = OnPthreadCreate(parent);
  info.GetReturnValue().Set(child);
}
void SetCurrentTidCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (info.Length() < 1) return;
  uint32_t new_tid = info[0]
      ->Uint32Value(info.GetIsolate()->GetCurrentContext())
      .FromMaybe(UINT32_MAX);
  // Worker-tid held every postMessage HB edge the Worker has absorbed
  // (parent → worker dispatch). Merge those into the pthread's webtsan_tid
  // so the pthread observes the parent's writes-before-spawn.
  uint32_t prev_tid = WebTSanGetCurrentTid();
  if (prev_tid != UINT32_MAX && new_tid != UINT32_MAX && prev_tid != new_tid) {
    WebTSan::Get().ThreadAbsorb(new_tid, prev_tid);
  }
  WebTSanSetCurrentTid(new_tid);
}
void PthreadJoinCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (info.Length() < 1) return;
  uint32_t joiner = WebTSanGetCurrentTid();
  if (joiner == UINT32_MAX) return;
  uint32_t child = info[0]
      ->Uint32Value(info.GetIsolate()->GetCurrentContext())
      .FromMaybe(UINT32_MAX);
  if (child == UINT32_MAX) return;
  OnPthreadJoin(joiner, child);
}
void PthreadDetachCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (info.Length() < 1) return;
  uint32_t child = info[0]
      ->Uint32Value(info.GetIsolate()->GetCurrentContext())
      .FromMaybe(UINT32_MAX);
  if (child == UINT32_MAX) return;
  OnPthreadDetach(child);
}

}  // namespace

void InstallWebTSanGlobals(v8::Isolate* isolate,
                           v8::Local<v8::Context> context) {
  if (!WebTSan::Get().IsEnabled())
    return;

  v8::HandleScope handle_scope(isolate);
  v8::Context::Scope context_scope(context);
  v8::MicrotasksScope microtasks_scope(
      isolate, context->GetMicrotaskQueue(),
      v8::MicrotasksScope::kDoNotRunMicrotasks);

  InstallFunction(isolate, context, "__webTSanClearShadow",
                  &ClearShadowCallback);
  InstallFunction(isolate, context, "__webTSanPushSuppress",
                  &PushSuppressCallback);
  InstallFunction(isolate, context, "__webTSanPopSuppress",
                  &PopSuppressCallback);
  InstallFunction(isolate, context, "__webTSanRecordRange",
                  &RecordRangeCallback);
  InstallFunction(isolate, context, "__webTSanThreadFinish",
                  &ThreadFinishCallback);
  InstallFunction(isolate, context, "__webTSanMutexCreate",
                  &MutexCreateCallback);
  InstallFunction(isolate, context, "__webTSanMutexDestroy",
                  &MutexDestroyCallback);
  InstallFunction(isolate, context, "__webTSanMutexPreLock",
                  &MutexPreLockCallback);
  InstallFunction(isolate, context, "__webTSanMutexPostLock",
                  &MutexPostLockCallback);
  InstallFunction(isolate, context, "__webTSanMutexUnlock",
                  &MutexUnlockCallback);
  InstallFunction(isolate, context, "__webTSanMutexReadLock",
                  &MutexReadLockCallback);
  InstallFunction(isolate, context, "__webTSanMutexReadUnlock",
                  &MutexReadUnlockCallback);
  InstallFunction(isolate, context, "__webTSanPthreadCreate",
                  &PthreadCreateCallback);
  InstallFunction(isolate, context, "__webTSanSetCurrentTid",
                  &SetCurrentTidCallback);
  InstallFunction(isolate, context, "__webTSanPthreadJoin",
                  &PthreadJoinCallback);
  InstallFunction(isolate, context, "__webTSanPthreadDetach",
                  &PthreadDetachCallback);
}

}  // namespace tsan
}  // namespace blink

// C entry point callable from content's renderer init (crosses dylib boundary).
extern "C" __attribute__((visibility("default"), used)) void WebTSanInitialize() {
  blink::tsan::InitializeWebTSan();
}
