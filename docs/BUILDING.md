# Applying the patches and building

Only the patches live in this repo — the upstream Chromium/V8 source is
fetched separately via `depot_tools` at the recorded base commits.

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

## Building and running

```sh
gn gen out/WebTSan --args='enable_web_tsan=true'
autoninja -C out/WebTSan chrome
out/WebTSan/Chromium.app/Contents/MacOS/Chromium \
  --enable-web-tsan \
  --js-flags=--wasm_tsan   # optionally, for Wasm shared-memory coverage
```
