// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// WebTSan initialization.
// Called during renderer process startup to enable TSan based on a
// command-line flag.
//
// Usage: chrome --enable-web-tsan --jitless <url>
//
// --jitless is required because the JIT emits raw load/store instructions
// that bypass our Runtime hooks. With --jitless, all code goes through the
// interpreter where we can intercept every SAB access.

#include "third_party/blink/renderer/core/tsan/web_tsan.h"

#if defined(ENABLE_WEB_TSAN)

#include "base/command_line.h"
#include "base/logging.h"

namespace blink {
namespace tsan {

// Command-line switch name.
const char kEnableWebTSan[] = "enable-web-tsan";

void InitializeWebTSan() {
  if (!base::CommandLine::ForCurrentProcess()->HasSwitch(kEnableWebTSan)) {
    return;
  }

  WebTSan::Get().Enable();

  // Assign tid 0 to the main thread.
  uint32_t main_tid = WebTSan::Get().ThreadCreate();
  WebTSanSetCurrentTid(main_tid);

  LOG(WARNING) << "WebTSan: ENABLED. Main thread tid=" << main_tid
               << ". Race reports will appear in LOG(ERROR). "
               << "Use --jitless for full coverage of non-atomic SAB access.";
}

// Called from RenderThreadImpl::Init() or similar renderer startup path.
// Patch location:
//   content/renderer/render_thread_impl.cc, in RenderThreadImpl::Init(),
//   after the V8 platform is initialized but before any JS runs.
//
// Insert:
//   #if defined(ENABLE_WEB_TSAN)
//   blink::tsan::InitializeWebTSan();
//   #endif

}  // namespace tsan
}  // namespace blink

#endif  // defined(ENABLE_WEB_TSAN)
