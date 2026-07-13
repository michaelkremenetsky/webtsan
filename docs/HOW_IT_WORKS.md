# How WebTSan works

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

## The detector

The core detector is a new Blink module,
`third_party/blink/renderer/core/tsan/`:

- Each Worker (plus the main thread) gets a logical thread ID with a
  **vector clock**.
- Each SharedArrayBuffer byte gets **shadow cells** (2 per byte) recording
  recent accesses, allocated in the host's address space — not inside the SAB.
- Synchronization points (Atomics ops, `postMessage`, worker start/join)
  propagate vector clocks between threads.
- Two conflicting accesses with no happens-before ordering → data race report.

## V8 integration

On the V8 side, a new `src/runtime/runtime-tsan.{cc,h}` exposes a callback
table that Blink registers at startup (so hooks work across the component-build
dylib boundary), and hooks are threaded through the futex emulation, the
SharedArrayBuffer builtins, and the Liftoff Wasm compiler.
