// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/runtime/runtime-tsan.h"

#include <sstream>

#include "src/base/vector.h"
#include "src/execution/arguments-inl.h"
#include "src/execution/execution.h"
#include "src/execution/isolate-inl.h"
#include "src/execution/messages.h"
#include "src/flags/flags.h"
#include "src/heap/factory.h"
#include "src/objects/js-array-buffer-inl.h"
#include "src/runtime/runtime-utils.h"

namespace v8 {
namespace internal {
namespace web_tsan {

WebTSanCallbacks g_callbacks;

void RegisterCallbacks(const WebTSanCallbacks& cb) { g_callbacks = cb; }

}  // namespace web_tsan

namespace {
uint32_t GetTid() {
  auto fn = web_tsan::g_callbacks.get_current_tid;
  return fn ? fn() : UINT32_MAX;
}

// Consume any pending race report from the current thread's TLS and log
// it to the browser console (console.error) with a JS stack trace.
// Always returns undefined — races are non-fatal so the app keeps running
// and the user sees the full stream of races rather than just the first.
Tagged<Object> MaybeThrowRace(Isolate* isolate) {
  if (!web_tsan::g_callbacks.consume_race)
    return ReadOnlyRoots(isolate).undefined_value();
  char msg[512];
  if (!web_tsan::g_callbacks.consume_race(msg, sizeof(msg)))
    return ReadOnlyRoots(isolate).undefined_value();

  HandleScope handle_scope(isolate);

  // Format msg + current stack into one string so console.error renders
  // the wasm frames in devtools (passing a JS Error object often loses
  // the stack because its `.stack` is lazy / context-dependent).
  std::ostringstream combined;
  combined << msg;
  isolate->PrintCurrentStackTrace(combined);
  std::string combined_str = combined.str();
  Handle<String> log_str =
      isolate->factory()
          ->NewStringFromUtf8(base::CStrVector(combined_str.c_str()))
          .ToHandleChecked();
  DirectHandle<JSGlobalObject> global(isolate->global_object());
  Handle<Object> console_val;
  Handle<String> console_key =
      isolate->factory()->NewStringFromAsciiChecked("console");
  if (Object::GetProperty(isolate, global, console_key).ToHandle(&console_val)
      && IsJSReceiver(*console_val)) {
    Handle<JSReceiver> console = Cast<JSReceiver>(console_val);
    Handle<String> error_key =
        isolate->factory()->NewStringFromAsciiChecked("error");
    Handle<Object> error_fn;
    if (Object::GetProperty(isolate, console, error_key).ToHandle(&error_fn)
        && IsCallable(*error_fn)) {
      DirectHandle<Object> argv[] = {Cast<Object>(log_str)};
      MaybeHandle<Object> result = Execution::Call(
          isolate, error_fn, console, base::VectorOf(argv));
      if (result.is_null() && isolate->has_exception()) {
        isolate->clear_exception();
      }
    }
  }
  return ReadOnlyRoots(isolate).undefined_value();
}
}  // namespace

RUNTIME_FUNCTION(Runtime_WebTSanRecordRead) {
  if (!v8_flags.wasm_tsan || !web_tsan::g_callbacks.on_sab_access)
    return ReadOnlyRoots(isolate).undefined_value();
  HandleScope scope(isolate);
  DCHECK_EQ(2, args.length());

  DirectHandle<JSTypedArray> typed_array = args.at<JSTypedArray>(0);
  int index = args.smi_value_at(1);

  DirectHandle<JSArrayBuffer> buffer = typed_array->GetBuffer(isolate);
  if (!buffer->is_shared()) return ReadOnlyRoots(isolate).undefined_value();

  uint32_t tid = GetTid();
  if (tid == UINT32_MAX) return ReadOnlyRoots(isolate).undefined_value();

  web_tsan::g_callbacks.on_sab_access(
      buffer->backing_store(), buffer->byte_length(),
      typed_array->byte_offset() + index * typed_array->element_size(),
      typed_array->element_size(), tid, 0);
  return MaybeThrowRace(isolate);
}

RUNTIME_FUNCTION(Runtime_WebTSanRecordWrite) {
  if (!v8_flags.wasm_tsan || !web_tsan::g_callbacks.on_sab_access)
    return ReadOnlyRoots(isolate).undefined_value();
  HandleScope scope(isolate);
  DCHECK_EQ(2, args.length());

  DirectHandle<JSTypedArray> typed_array = args.at<JSTypedArray>(0);
  int index = args.smi_value_at(1);

  DirectHandle<JSArrayBuffer> buffer = typed_array->GetBuffer(isolate);
  if (!buffer->is_shared()) return ReadOnlyRoots(isolate).undefined_value();

  uint32_t tid = GetTid();
  if (tid == UINT32_MAX) return ReadOnlyRoots(isolate).undefined_value();

  web_tsan::g_callbacks.on_sab_access(
      buffer->backing_store(), buffer->byte_length(),
      typed_array->byte_offset() + index * typed_array->element_size(),
      typed_array->element_size(), tid, 1);
  return MaybeThrowRace(isolate);
}

RUNTIME_FUNCTION(Runtime_WebTSanAtomicsLoad) {
  if (!v8_flags.wasm_tsan || !web_tsan::g_callbacks.on_atomics_load)
    return ReadOnlyRoots(isolate).undefined_value();
  HandleScope scope(isolate);
  DCHECK_EQ(2, args.length());

  DirectHandle<JSTypedArray> typed_array = args.at<JSTypedArray>(0);
  int index = args.smi_value_at(1);

  DirectHandle<JSArrayBuffer> buffer = typed_array->GetBuffer(isolate);
  if (!buffer->is_shared()) return ReadOnlyRoots(isolate).undefined_value();

  uint32_t tid = GetTid();
  if (tid == UINT32_MAX) return ReadOnlyRoots(isolate).undefined_value();

  web_tsan::g_callbacks.on_atomics_load(
      tid, static_cast<uint8_t*>(buffer->backing_store()) +
               typed_array->byte_offset() +
               index * typed_array->element_size());
  // Forward any pending race from a prior non-atomic WASM access (which
  // couldn't throw under SealHandleScope) as a JS error now.
  return MaybeThrowRace(isolate);
}

RUNTIME_FUNCTION(Runtime_WebTSanAtomicsStore) {
  if (!v8_flags.wasm_tsan || !web_tsan::g_callbacks.on_atomics_store)
    return ReadOnlyRoots(isolate).undefined_value();
  HandleScope scope(isolate);
  DCHECK_EQ(2, args.length());

  DirectHandle<JSTypedArray> typed_array = args.at<JSTypedArray>(0);
  int index = args.smi_value_at(1);

  DirectHandle<JSArrayBuffer> buffer = typed_array->GetBuffer(isolate);
  if (!buffer->is_shared()) return ReadOnlyRoots(isolate).undefined_value();

  uint32_t tid = GetTid();
  if (tid == UINT32_MAX) return ReadOnlyRoots(isolate).undefined_value();

  web_tsan::g_callbacks.on_atomics_store(
      tid, static_cast<uint8_t*>(buffer->backing_store()) +
               typed_array->byte_offset() +
               index * typed_array->element_size());
  return MaybeThrowRace(isolate);
}

RUNTIME_FUNCTION(Runtime_WebTSanAtomicsRMW) {
  if (!v8_flags.wasm_tsan || !web_tsan::g_callbacks.on_atomics_rmw)
    return ReadOnlyRoots(isolate).undefined_value();
  HandleScope scope(isolate);
  DCHECK_EQ(2, args.length());

  DirectHandle<JSTypedArray> typed_array = args.at<JSTypedArray>(0);
  int index = args.smi_value_at(1);

  DirectHandle<JSArrayBuffer> buffer = typed_array->GetBuffer(isolate);
  if (!buffer->is_shared()) return ReadOnlyRoots(isolate).undefined_value();

  uint32_t tid = GetTid();
  if (tid == UINT32_MAX) return ReadOnlyRoots(isolate).undefined_value();

  web_tsan::g_callbacks.on_atomics_rmw(
      tid, static_cast<uint8_t*>(buffer->backing_store()) +
               typed_array->byte_offset() +
               index * typed_array->element_size());
  return MaybeThrowRace(isolate);
}

}  // namespace internal
}  // namespace v8
