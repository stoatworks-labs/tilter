# tilter

A tilt-shift lens — a shallow, tiltable plane of focus with a real aperture
behind it — as an FFGL effect for Resolume Arena/Avenue, plus an OpenFX build
for Resolve/Nuke/Natron/Vegas. C++/GLSL, CMake MODULE → universal `.bundle`
(macOS) + Windows `.dll`. Public MIT repo.

Read `AGENTS.md` before changing the focus field, the sampling, or the units.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build`
- Render a frame offline: `./build/tiltest --out /tmp/frame.png`
- Contact sheet of every mode: `./build/tiltest --sheet /tmp/sheet.png`
- The test scene on its own: `./build/tiltest --scene /tmp/scene.png`
- List parameters, with types and defaults: `./build/tiltest --list`
- Set anything by its host-facing name:
  `--set "Focus Shape=2" --set "Tilt=0.7" --set "Blur Amount=0.6"`
- Render size: `--size 1920x1080`

## OpenFX build
- `source/ofx/TilterOFX.cpp` → `build/Tilter.ofx.bundle` (target `TilterOFX`,
  `-DBUILD_OFX=OFF` to skip) for Resolve/Vegas/Nuke/Natron.
- `Focus.cpp` and `Controls.cpp` link straight from source — one home for the
  field and for every parameter curve, so a preset cannot mean different things
  in Resolume and Resolve. Only the per-pixel GPU work is mirrored there.
- Smoke test:
  `../resolume-ofx-bridge/build/ofxprobe --dir build --render com.stoatworks.tilter --size 640x360 --out /tmp/t.bmp`
- OFX SDK subset (BSD-3) vendored under `external/openfx`.
- Install for Resolve: copy the bundle into `/Library/OFX/Plugins`.

## Verify
- Everything: `tools/verify.sh`
- The CoC field against `Focus.cpp`, all four shapes: `./build/tiltest --focus`
- The blur blurs, and only where it should: `./build/tiltest --blur`
- The aperture's shape and size: `./build/tiltest --aperture`
- Presets distinct and non-degenerate: `./build/tiltest --presets`
- No dead controls: `python3 tools/sweep.py`

## Notes
- **The field and the blur are independent.** A geometry writes one signed
  number per pixel; a blur reads it. Four shapes × two blurs is six pieces of
  code, not eight.
- **Positive defocus is the NEAR side.** Not arbitrary — it is scene inverse
  depth minus lens inverse depth, and inverse depth grows as things get closer.
- **No clock.** The output is a pure function of the input frame and the
  parameters; animation comes from the host keyframing controls.
- **`CoC.cpp` mirrors `Focus.cpp`** and is the only mirrored file. Blocks are
  marked `//= mirrored` in both. Change one → run `--focus`.
- **Distances are in frame-height units**, not normalised u/v, so a band keeps
  its thickness across aspect ratios. Invisible on a square render — which is
  why `--focus` uses 320×180.
- **`Focus Y` = 0 is the top.** GL's v runs the other way; the flip happens once,
  on the first line of `CoC.cpp`'s `main()`.
- **The blur runs on a box-downsampled copy.** Not just for speed: a sparse
  gather aliases its source, and it gets worse as the radius grows. See
  `Downsample.cpp`.
- **Option parameters hold the element value**, not 0..1, and are read through
  `controls::option()`. Standard parameters are all 0..1 and converted in
  `Controls.cpp` — `SetParamInfo` clamps a ranged default.
- **`SetTextParameter` must be overridden** or no real host can instantiate the
  plugin at all. See `AGENTS.md`.
- macOS build must be universal (arm64 + x86_64). Verify with `lipo`, never the
  build log.
- `flat`, `active`, `filter`, `input`, `output`, `sample`, `common` are GLSL
  reserved words. Shader errors surface only at runtime, in the diagnostics log.
- Public repo. "Commit" = commit **and** push.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume), no bundle command. It covers the one failure that actually happens: a
shader that will not compile, which otherwise looks like "the effect does
nothing" with no message anywhere. Seven stages, so the log names which one, with
the GL vendor and version beside it.

    ~/Library/Logs/tilter/tilter.YYYY-MM-DD.log
