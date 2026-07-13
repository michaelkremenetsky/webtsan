# WebTSan — ThreadSanitizer for the Web

A patch set for Chromium and V8 that adds **data-race detection for
SharedArrayBuffer-based web concurrency** — a simplified ThreadSanitizer that
lives in the renderer process and understands the web's threading primitives.

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

## How it works

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

## Repository layout

```
patches/
  chromium/            Patches to chromium/src
    METADATA.txt         Base commit + remote the patch applies to
    tracked-changes.patch  git diff of all modified tracked files
    modified-files/      Verbatim full copies of each modified file
    new-files/           Verbatim copies of all added files (the tsan/ module)
  v8/                  Same structure, for the nested v8/ checkout
  gclient.config       The .gclient config used for the checkout
docs/PATCHES.md        Design doc: every upstream edit, with rationale
tests/web_tsan_test.cc Standalone test of the vector-clock/shadow-memory core
```

Only the patches live here — the upstream Chromium/V8 source is fetched
separately via `depot_tools` at the recorded base commits.

## Applying the patches

Base revisions (see `METADATA.txt` in each patch dir):

| Repo | Commit |
|---|---|
| chromium/src | `4250fd77673f56c0dacc7c167117b51512385865` |
| v8/v8 | `16b79075b64e96b7e803a336507d52d49f8607f2` (14.9.68) |

```sh
# 1. Fetch Chromium at the recorded base commit (needs depot_tools)
fetch --nohooks chromium && cd src
git checkout 4250fd77673f56c0dacc7c167117b51512385865
gclient sync -D --with_branch_heads

# 2. Pin V8 to the recorded base, apply the V8 patch, copy new files
cd v8
git checkout 16b79075b64e96b7e803a336507d52d49f8607f2
git apply /path/to/patches/v8/tracked-changes.patch
cp -R /path/to/patches/v8/new-files/* .
cd ..

# 3. Apply the Chromium/Blink patch and copy new files
git apply /path/to/patches/chromium/tracked-changes.patch
cp -R /path/to/patches/chromium/new-files/* .
```

If `git apply` rejects a hunk due to upstream drift, the verbatim copies under
`modified-files/` are the exact final versions to merge by hand.

### Building and running

```sh
gn gen out/WebTSan --args='enable_web_tsan=true'
autoninja -C out/WebTSan chrome
out/WebTSan/Chromium.app/Contents/MacOS/Chromium \
  --enable-web-tsan \
  --js-flags=--wasm_tsan   # optionally, for Wasm shared-memory coverage
```

## Standalone core test

The detector's vector-clock/shadow-memory logic can be tested without a
Chromium checkout:

```sh
c++ -std=c++17 -DSTANDALONE_TEST tests/web_tsan_test.cc -o web_tsan_test
./web_tsan_test
```

## License

BSD 3-Clause — see [LICENSE](LICENSE). The patches and the copied files under
`modified-files/` contain code from the Chromium and V8 projects, copyright
The Chromium Authors and the V8 project authors, under their BSD-style
licenses.
