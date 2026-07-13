# WebTSan — ThreadSanitizer for the Web

A patch set for Chromium and V8 that adds **data-race detection for
SharedArrayBuffer-based web concurrency** — a simplified ThreadSanitizer that
lives in the renderer process and understands the web's threading primitives.

Approaches to getting the LLVM ThreadSanitizer running in the browser failed because:
* **It runs out of memory** - ThreadSanitizer typically increases an application's memory usage by about 5x to 10x. This would make most real-world applications blow past the 4GB wasm32 memory limit, limiting their usefulness.
* **Half the race is outside Wasm** - Most WASM applications have FFI calls to JS code, which a pure WASM approach wouldn't be able to instrument, so it misses races and reports false ones.
* **There is nothing to hook** - the primitives that actually create happens-before edges (Atomics.wait/notify, postMessage, Worker lifecycle) execute in the engine and browser, below the module's view.


WebTSan fixes these issues by going a layer deeper and implementing the ThreadSantizer in the browser itself bypassing these limitations.

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
