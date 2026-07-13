# WebTSan — ThreadSanitizer for the Web

A patch set for Chromium and V8 that adds **data-race detection for
SharedArrayBuffer-based web concurrency** — a simplified ThreadSanitizer that
lives in the renderer process and understands the web's threading primitives.

## How it works

Native TSan can't see web-level races: to the OS, a `SharedArrayBuffer` is just
memory, and `postMessage`/`Atomics.wait` are opaque. WebTSan wires
happens-before edges through the primitives web apps actually synchronize with:

- **JS Atomics / SAB** — `Atomics.wait`/`notify` (futex emulation),
  `load`/`store`/`exchange` and other RMW ops, and typed-array access to
  SharedArrayBuffers.
- **WebAssembly shared memory** — Liftoff-compiled Wasm loads/stores are
  instrumented (`--wasm_tsan` disables tier-up so all Wasm stays in Liftoff).
- **Blink** — `postMessage` send/receive happens-before edges, and Worker
  lifecycle (logical thread ID assignment/release).

The core detector is a new Blink module,
`third_party/blink/renderer/core/tsan/`:

- Each Worker (plus the main thread) gets a logical thread ID with a
  **vector clock**.
- Each SharedArrayBuffer byte gets **shadow cells** (2 per byte) recording
  recent accesses, allocated in the host's address space — not inside the SAB.
- Synchronization points (Atomics ops, `postMessage`, worker start/join)
  propagate vector clocks between threads.
- Two conflicting accesses with no happens-before ordering → data race report.

On the V8 side, a new `src/runtime/runtime-tsan.{cc,h}` exposes a callback
table that Blink registers at startup (so hooks work across the component-build
dylib boundary), and hooks are threaded through the futex emulation, the
SharedArrayBuffer builtins, and the Liftoff Wasm compiler.

`docs/PATCHES.md` is the original design doc walking through every upstream
edit with before/after diffs. It was written against an earlier iteration, so
the final code in `patches/` diverges in places, but the architecture it
describes still holds.

## Status

Research prototype. It compiles and runs against the pinned revisions below,
but it is not production-quality: max 64 logical threads, 2 shadow cells per
byte (vs. TSan's 4), and only the instrumented execution paths are covered
(e.g. TurboFan-compiled Wasm is excluded by design via tier-up suppression).

## Applying the patches and building

See [docs/BUILDING.md](docs/BUILDING.md) for the pinned Chromium/V8 base
revisions, how to apply the patches to a fresh checkout, and build/run flags.

## License

BSD 3-Clause — see [LICENSE](LICENSE). The patches and the copied files under
`modified-files/` contain code from the Chromium and V8 projects, copyright
The Chromium Authors and the V8 project authors, under their BSD-style
licenses.
