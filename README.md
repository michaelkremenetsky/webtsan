# WebTSan — Patched Chromium (ThreadSanitizer for the Web)

Private backup of my ThreadSanitizer-for-Chromium patch set, captured so the
full ~46 GB of Chromium source/build checkouts can be deleted and the work
fully reconstructed later. **Only the patches are stored here** — the upstream
Chromium/V8 source is re-fetched via `depot_tools`/`gclient` at the recorded
base commits.

## What WebTSan does
Wires TSan happens-before edges through the web concurrency primitives:
- **V8 atomics / SAB**: `Atomics.wait/notify`, `load/store/exchange/RMW`,
  futex emulation, typed-array access on SharedArrayBuffers (jitless path).
- **Blink**: `postMessage` happens-before edges, worker lifecycle (tid
  assign/release), worker thread plumbing.
- New `third_party/blink/renderer/core/tsan/` module + V8 `runtime-tsan.*`.

See `A_work-compiles_apr11/new-files/.../tsan/PATCHES.md` for the full design
doc describing every upstream edit.

## Two snapshots (both preserved — they diverge)

### B_chromium-src_apr17  ← MOST COMPLETE / latest
- Source: `~/chromium/src` (the gclient checkout; `.gclient` saved as `gclient.config.txt`)
- Chromium base: `4250fd77673f56c0dacc7c167117b51512385865` ("Roll ios_internal ...")
- **V8 base: `16b79075b64e96b7e803a336507d52d49f8607f2` (Version 14.9.68)**
- Outer repo: 12 modified files + new `tsan/` dir
- **V8 sub-repo: 10 modified files + new `runtime-tsan.cc/.h`** (in `v8-subrepo/`)
- Dated Apr 17 — the full implementation (V8 + Blink + content/).

### A_work-compiles_apr11  ← earlier, Blink-only
- Source: `~/Work/compiles/chromium`
- Chromium base: `9398c4a1fa432316ff8386b7eeccf6578951ae3d`
- 8 modified files + new `tsan/` dir. **Has `PATCHES.md` design doc and
  `web_tsan_test.cc`**, which are NOT in snapshot B — that's why it's kept.
- No V8 changes (Blink-side only).

## Layout (per snapshot)
- `METADATA.txt`        — source path, base commit, remote
- `tracked-changes.patch` — `git diff` of all modified tracked files (reapplyable)
- `tracked-changes.stat.txt`
- `modified-files-list.txt` / `modified-files/` — verbatim full copies of each modified file
- `new-files-list.txt` / `new-files/` — verbatim copies of all new files
- `v8-subrepo/` (snapshot B only) — same structure for the nested V8 repo

## How to restore (snapshot B, the complete one)
```sh
# 1. Re-fetch Chromium at the recorded base commit (needs depot_tools)
fetch --nohooks chromium && cd src
git checkout 4250fd77673f56c0dacc7c167117b51512385865
gclient sync -D --with_branch_heads
# 2. Pin V8 to the recorded base, then apply V8 patch
cd v8 && git checkout 16b79075b64e96b7e803a336507d52d49f8607f2
git apply /path/to/B_chromium-src_apr17/v8-subrepo/tracked-changes.patch
# copy new V8 files
cp -R /path/to/B_chromium-src_apr17/v8-subrepo/new-files/* .
cd ..
# 3. Apply outer (Chromium/Blink) patch + new files
git apply /path/to/B_chromium-src_apr17/tracked-changes.patch
cp -R /path/to/B_chromium-src_apr17/new-files/* .
```
If `git apply` rejects a hunk due to upstream drift, the verbatim copies under
`modified-files/` are the exact final versions to merge by hand.
