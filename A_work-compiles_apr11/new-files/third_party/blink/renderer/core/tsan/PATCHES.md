# WebTSan: Required Patches to Chromium/V8

This documents the exact code changes needed in existing Chromium/V8 files
to wire up the WebTSan hooks. Each section shows the file, the location,
and the before/after diff.

---

## 1. V8: Futex Emulation (Atomics.wait / Atomics.notify)

### File: `v8/src/execution/futex-emulation.cc`

#### 1a. Atomics.wait — acquire on completion

After WaitSync returns (the thread was woken), insert an acquire:

```cpp
// EXISTING (line ~388-440 in WaitSync):
template <typename T>
Tagged<Object> FutexEmulation::WaitSync(Isolate* isolate,
                                        Handle<JSArrayBuffer> array_buffer,
                                        size_t addr, T value, bool use_timeout,
                                        int64_t rel_timeout_ns,
                                        CallType call_type) {
  // ... existing implementation ...
  // AFTER the do-while loop and before the final callback:

+ // >>> WebTSan: Atomics.wait completed = acquire semantics
+ #if defined(ENABLE_WEB_TSAN)
+   {
+     void* wait_loc = FutexWaitList::ToWaitLocation(*array_buffer, addr);
+     // TODO: map isolate -> tsan tid. For now use a hash of the isolate ptr.
+     uint32_t tsan_tid = WebTSanGetTid(isolate);
+     blink::tsan::OnAtomicsWaitComplete(tsan_tid, wait_loc);
+   }
+ #endif

  isolate->RunAtomicsWaitCallback(callback_result, array_buffer, addr, value,
                                  rel_timeout_ms, nullptr);
  // ...
}
```

#### 1b. Atomics.notify — release before waking

```cpp
// EXISTING (line ~681):
int FutexEmulation::Wake(Tagged<JSArrayBuffer> array_buffer, size_t addr,
                         uint32_t num_waiters_to_wake) {
  void* wait_location = FutexWaitList::ToWaitLocation(array_buffer, addr);

+ // >>> WebTSan: Atomics.notify = release semantics
+ #if defined(ENABLE_WEB_TSAN)
+   uint32_t tsan_tid = WebTSanGetCurrentTid();
+   blink::tsan::OnAtomicsNotify(tsan_tid, wait_location);
+ #endif

  return Wake(wait_location, num_waiters_to_wake);
}
```

---

## 2. V8: SharedArrayBuffer Builtins (Atomics.load/store/exchange/etc)

### File: `v8/src/builtins/builtins-sharedarraybuffer.cc`

#### 2a. Atomics.store — release

```cpp
// In the BUILTIN(AtomicsStore) or the generated TF builtin for Atomics.store,
// after computing the target address and before/after the actual store:

+ #if defined(ENABLE_WEB_TSAN)
+   void* location = static_cast<uint8_t*>(array_buffer->backing_store()) +
+                     byte_offset;
+   blink::tsan::OnAtomicsStore(WebTSanGetCurrentTid(), location);
+ #endif
```

#### 2b. Atomics.load — acquire

```cpp
+ #if defined(ENABLE_WEB_TSAN)
+   void* location = static_cast<uint8_t*>(array_buffer->backing_store()) +
+                     byte_offset;
+   blink::tsan::OnAtomicsLoad(WebTSanGetCurrentTid(), location);
+ #endif
```

#### 2c. Atomics.compareExchange / exchange / add / sub / and / or / xor — RMW

```cpp
+ #if defined(ENABLE_WEB_TSAN)
+   void* location = static_cast<uint8_t*>(array_buffer->backing_store()) +
+                     byte_offset;
+   blink::tsan::OnAtomicsRMW(WebTSanGetCurrentTid(), location);
+ #endif
```

---

## 3. V8: Interpreter — Typed Array access on SABs (--jitless mode)

### File: `v8/src/ic/accessor-assembler.cc`

This is the slow path for typed array access. In --jitless mode all typed array
access goes through here.

#### 3a. In the `if_typed_array` / `if_rab_gsab_typed_array` code paths

After computing the element address and before/after the actual Load/Store:

```cpp
// Pseudocode — this is actually CodeStubAssembler (CSA) code, not normal C++.
// The actual hook needs to be a runtime call inserted into the CSA graph.

// For LOADS:
//   Before the load instruction, emit:
//   CallRuntime(Runtime::kWebTSanRecordRead, context, backing_store_ptr,
//               byte_offset, element_size);

// For STORES:
//   Before the store instruction, emit:
//   CallRuntime(Runtime::kWebTSanRecordWrite, context, backing_store_ptr,
//               byte_offset, element_size);
```

#### 3b. Add runtime functions

### File: `v8/src/runtime/runtime.h`

```cpp
// Add to the RUNTIME_FUNCTION_LIST:
+ F(WebTSanRecordRead, 3, 1)   // backing_store, offset, size
+ F(WebTSanRecordWrite, 3, 1)  // backing_store, offset, size
```

### File: `v8/src/runtime/runtime-tsan.cc` (NEW FILE)

```cpp
#include "src/runtime/runtime-utils.h"
#include "third_party/blink/renderer/core/tsan/web_tsan_hooks.h"

namespace v8 {
namespace internal {

RUNTIME_FUNCTION(Runtime_WebTSanRecordRead) {
  // Extract backing_store pointer, offset, size from args
  // Call blink::tsan::OnSABAccess(..., /*is_write=*/false)
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_WebTSanRecordWrite) {
  // Extract backing_store pointer, offset, size from args
  // Call blink::tsan::OnSABAccess(..., /*is_write=*/true)
  return ReadOnlyRoots(isolate).undefined_value();
}

}  // namespace internal
}  // namespace v8
```

---

## 4. Blink: postMessage (happens-before edge)

### File: `third_party/blink/renderer/core/messaging/message_port.cc`

#### 4a. Sender side — in postMessage(), embed edge_id in the message

```cpp
void MessagePort::postMessage(ScriptState* script_state,
                              const ScriptValue& message,
                              const PostMessageOptions* options,
                              ExceptionState& exception_state) {
  // ... existing code ...
  msg.sender_agent_cluster_id = GetExecutionContext()->GetAgentClusterID();
  msg.locked_to_sender_agent_cluster = msg.message->IsLockedToAgentCluster();

+ // >>> WebTSan: record happens-before edge on postMessage send
+ #if defined(ENABLE_WEB_TSAN)
+   msg.tsan_edge_id = blink::tsan::OnPostMessageSend(
+       GetCurrentWorkerTSanTid());
+ #endif

  // ... rest of existing code ...
```

#### 4b. Receiver side — in DispatchMessageEvent(), consume edge_id

```cpp
void MessagePort::DispatchMessageEvent(BlinkTransferableMessage message) {
  // ... existing code ...

+ // >>> WebTSan: happens-after on message receive
+ #if defined(ENABLE_WEB_TSAN)
+   blink::tsan::OnPostMessageReceive(
+       GetCurrentWorkerTSanTid(), message.tsan_edge_id);
+ #endif

  Event* evt = CreateMessageEvent(message);
  // ... rest ...
```

#### 4c. Add tsan_edge_id to BlinkTransferableMessage

### File: `third_party/blink/renderer/core/messaging/blink_transferable_message.h`

```cpp
struct BlinkTransferableMessage : TransferableMessage {
  // ... existing fields ...
+ #if defined(ENABLE_WEB_TSAN)
+   uint64_t tsan_edge_id = 0;
+ #endif
};
```

---

## 5. Blink: Worker lifecycle

### File: `third_party/blink/renderer/core/workers/dedicated_worker.cc`

#### 5a. Worker creation — assign tid

```cpp
DedicatedWorker* DedicatedWorker::Create(...) {
  // ... existing code ...
  DedicatedWorker* worker = MakeGarbageCollected<DedicatedWorker>(
      context, script_request_url, options);
  worker->UpdateStateIfNeeded();
  worker->Start();

+ #if defined(ENABLE_WEB_TSAN)
+   worker->tsan_tid_ = blink::tsan::OnWorkerCreate();
+ #endif

  return worker;
}
```

#### 5b. Worker destruction — release tid

```cpp
void DedicatedWorker::Dispose() {
  DCHECK(!GetExecutionContext() || GetExecutionContext()->IsContextThread());

+ #if defined(ENABLE_WEB_TSAN)
+   if (tsan_tid_ != UINT32_MAX) {
+     blink::tsan::OnWorkerDestroy(tsan_tid_);
+     tsan_tid_ = UINT32_MAX;
+   }
+ #endif

  context_proxy_->ParentObjectDestroyed();
  factory_client_.reset();
}
```

#### 5c. Add tsan_tid_ field to DedicatedWorker

### File: `third_party/blink/renderer/core/workers/dedicated_worker.h`

```cpp
class DedicatedWorker : ... {
  // ... existing members ...
+ #if defined(ENABLE_WEB_TSAN)
+   uint32_t tsan_tid_ = UINT32_MAX;
+ #endif
};
```

---

## 6. Thread ID mapping: Isolate -> TSan tid

The V8 side needs to map an Isolate* (one per worker) to its WebTSan tid.
Two approaches:

### Option A: Store tid on the Isolate (requires V8 patch)

```cpp
// v8/src/execution/isolate.h
class Isolate {
  // ... existing ...
+ #if defined(ENABLE_WEB_TSAN)
+   uint32_t web_tsan_tid_ = UINT32_MAX;
+ #endif
};
```

### Option B: Thread-local (no V8 patch needed)

```cpp
// In web_tsan_hooks.h:
inline thread_local uint32_t g_current_tsan_tid = UINT32_MAX;

inline uint32_t WebTSanGetCurrentTid() { return g_current_tsan_tid; }
inline void WebTSanSetCurrentTid(uint32_t tid) { g_current_tsan_tid = tid; }
```

Option B is simpler and works because each Worker runs on its own OS thread.
Set the TLS in the worker thread startup path.

---

## 7. Build system

### File: `third_party/blink/renderer/core/BUILD.gn`

```gn
# Add to the blink_core_sources list:
if (enable_web_tsan) {
  sources += [
    "tsan/web_tsan.cc",
    "tsan/web_tsan.h",
    "tsan/web_tsan_hooks.h",
  ]
  defines += [ "ENABLE_WEB_TSAN" ]
}
```

### File: `build/config/features.gni`

```gn
declare_args() {
  enable_web_tsan = false
}
```

Build with: `gn gen out/Debug --args='enable_web_tsan=true'`

---

## 8. Enabling at runtime

Add a command-line flag:

### File: `chrome/browser/about_flags.cc` (or simpler: a runtime flag)

```
--enable-web-tsan    Enables WebTSan race detection for SharedArrayBuffer
```

When present, call `blink::tsan::WebTSan::Get().Enable()` during renderer init.
