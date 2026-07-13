# WebTSan — ThreadSanitizer for the Web

A patch set for Chromium and V8 that adds **data-race detection for
SharedArrayBuffer-based web concurrency** — a simplified ThreadSanitizer that
lives in the renderer process and understands the web's threading primitives.

## Status

Research prototype. It compiles and runs on real apps, but it is not production-quality: likely has false postives, max 64 logical threads, 2 shadow cells per
byte (vs. TSan's 4), and some JIT tiers like turbofan are disabled for now.

Main issue with practical use is real-world WASM libcs like emscripten and wasi-libc need to be patched as they currently cause a lot of false postives because they are not written with being run through a thread santizer in mind. This is where most of the work is left, and it's the hardest 70%.

## How it works

Native TSan can't see web-level races: to the OS, a `SharedArrayBuffer` is just
memory, and `postMessage`/`Atomics.wait` are opaque. WebTSan wires
happens-before edges through the primitives web apps actually synchronize with
(JS Atomics, Wasm shared memory, `postMessage`, Worker lifecycle), and detects
races with vector clocks and shadow memory.

See [docs/HOW_IT_WORKS.md](docs/HOW_IT_WORKS.md) for the full architecture.

## Applying the patches and building

See [docs/BUILDING.md](docs/BUILDING.md) for the pinned Chromium/V8 base
revisions, how to apply the patches to a fresh checkout, and build/run flags.

## License

BSD 3-Clause — see [LICENSE](LICENSE). The patches and the copied files under
`modified-files/` contain code from the Chromium and V8 projects, copyright
The Chromium Authors and the V8 project authors, under their BSD-style
licenses.
