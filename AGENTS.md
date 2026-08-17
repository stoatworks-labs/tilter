# tilter — orientation for another LLM (or a newcomer)

**What it is:** a tilt-shift lens — the one whose focal plane is not parallel to
the sensor — as an FFGL 2.1 effect for Resolume Arena/Avenue, plus an OpenFX
build for Resolve/Nuke/Natron/Vegas. C++17 + GLSL 4.1, CMake, universal macOS
`.bundle` and a Windows `.dll`. Public, MIT, `github.com/stoatworks-labs/tilter`.

`CLAUDE.md` is the command reference — build, install, verify. This file is the
*why*: read it before touching the focus field, the sampling, or the units.

---

## The one idea

**The plugin decides how far each pixel is from focus. The blur decides what
that looks like.**

Those two are completely independent. A geometry writes one signed number per
pixel into a buffer; a blur reads that buffer. Neither knows anything about the
other, which is why four focus geometries and two blur models are six pieces of
code rather than eight effects.

The number is the *signed normalised defocus*: 0 at the plane of focus, ±1 at
full blur, and the sign says which side. **Positive is the near side.** That is
not a convention picked at random — in Tilted Plane the raw quantity is scene
inverse depth minus lens inverse depth, and inverse depth grows as things get
closer, so positive genuinely means "nearer than the focal plane". Linear Band
agrees by construction: positive is below the band, which is the near end of a
frame shot looking down.

### What falls out of it

A new geometry is one branch in one shader. A new blur is one pass reading the
same buffer. The Show Focus overlay is a *view of that buffer*, which is why it
can be trusted to show what the blur is really doing rather than offering a
second opinion about it.

**There is no clock.** Unlike most of the fleet, nothing here animates: the
output is a pure function of the input frame and the parameters. A re-render is
bit-identical, and any movement comes from the host keyframing the controls,
which is where it belongs.

---

## What Tilted Plane actually buys

Worth reading before reaching for it, because the obvious expectation is wrong.

For a **planar** subject — a street, a table top, anything flat — perspective
maps inverse depth to an affine function of image position. That is a theorem,
not an approximation. So the in-focus region of a real tilted lens on a flat
scene is **exactly a straight band**, which Linear Band already draws. Tilted
Plane is not a differently-shaped band.

What it buys is the **falloff**, which is the half people get wrong when they
fake this:

- A real lens blurs by an amount proportional to the difference in *inverse*
  depth, and inverse depth has a floor — nothing is further away than infinity.
  So on the far side the blur ramps up and then **stops** at the horizon, and
  everything past it (sky, distant hills, the backs of buildings) shares one
  blur. On the near side there is no such limit and it grows without bound.
- A linear band ramps symmetrically and forever in both directions. That is the
  tell, and it is why fake tilt-shift so often has a sky that is blurred by a
  visibly different amount from the distant buildings under it.

`Tilt` then swings the focal plane about the viewing axis, so the focused depth
varies *along* the band as well as across it and the sharp zone converges the
way it does in a real tilt-shift frame. At `Tilt` = 0 the mode is a band with a
plateau, and nothing more.

**What none of this can do is depth.** There is no depth channel, so a building
standing up off the ground plane is treated as though it lay flat on it. That is
the honest limit of every one of these geometries.

---

## The traps

Ordered by how much time they will cost you.

**A sparse tap sum aliases, and it gets WORSE as the radius grows.** This is the
one that will waste a day. A gather blur is a discrete estimate of an integral;
anything in the source finer than the tap spacing folds down into coarse
structure that was never in the picture. Because the taps spread out as the
radius grows, the artefact *strengthens* exactly where the picture is supposed
to be getting smoother. Measured with the whole frame at uniform full defocus,
whole-frame detail went **3.24, 6.46, 4.26, 11.32** as Blur went 0.35, 0.50,
0.70, 1.00 — rising, and not even monotonically, because it is a beat between
the tap spacing and the scene's own pitch. On screen it is a fine stipple lying
over a picture that should be perfectly smooth.

Raising the tap count does **not** fix it. The radius reaches eight per cent of
the frame height, which is 173 pixels on a 4K composition, and no affordable
budget keeps sub-pixel spacing over that. The fix is to remove the frequencies
before sampling: `Downsample.cpp` box-filters the picture down to a scale where
the radius is about a dozen pixels, and the blur runs there. `tiltest --blur` is
the test, and its monotonicity check is the thing that catches a regression.

**A gather cannot render a true point highlight, and no setting will make it.**
An isolated 3-pixel highlight inside a 19-pixel-radius disc covers under one per
cent of that disc's area, so most output pixels miss it entirely and the few
that hit it become single bright specks. The disc never forms because the
estimator's variance is larger than the thing being estimated. This is a
property of gather-based defocus, not a defect in this one — clean discs around
true points need a **scatter** pass that draws a sprite per bright pixel, which
is a different architecture and is not here. Highlights of about ten pixels and
up bokeh correctly, which is most real footage. `tiltest --aperture` sizes its
probe accordingly, and says so in a comment, rather than quietly measuring
something easier.

**Distances are in units of FRAME HEIGHT, not in normalised u or v.** A width of
0.25 is a quarter of the frame's height whatever the aspect ratio is, so a band
keeps its thickness when the composition goes 16:9 to 4:3 and a circle stays
circular. The aspect correction is applied to the u axis only, in `bandCoords`.
Getting this wrong is **invisible on a square test render** and wrong on every
real output — which is why `tiltest --focus` renders 320×180 rather than
something square.

**Image Depth measures a different axis, in different units.** `Focus Width` and
`Feather` are frame-height units in the first three geometries and units of
*guessed depth* (0..1) in the fourth. There is no way around that — the axis
being measured is not a distance in the picture — and it is why that mode's
defaults are set separately.

**GL's v runs up; the focus field's runs down.** `Focus Y` = 0 is the top of the
frame, because that is how an operator reads one. The flip happens once, on the
first line of `CoC.cpp`'s `main()`, and nowhere else. Getting it wrong is
completely invisible on a centred symmetrical band, which is exactly why the
mirror test deliberately places the focus off centre and off axis.

**`CoC.cpp` is a mirror of `Focus.cpp`, and it is the only mirrored file.** The
field is a function of every pixel so the GPU must evaluate it, while the OFX
build and the harness need the C++ one. Every mirrored block is marked
`//= mirrored` in both files. `tiltest --focus` is the only thing standing
between the two copies and a silent drift — **if you change one, run it.**

**Every `ffglex::Scoped*` binding CLEARS to 0 on scope exit — it does not
restore.** `FFGLFBO::Initialise` sizes its colour texture under one of those, so
*allocating a buffer silently unbinds your input texture*. The symptom is the
dangerous part: correct on every frame except the one that allocates. This
plugin is safe by construction — `ProcessOpenGL` calls every `Ensure()` before
it binds anything — and `PassBuffer::Ensure` also saves and restores
`GL_TEXTURE_BINDING_2D`, so it survives somebody moving one line.

**`ScopedFBOBinding` restores the framebuffer and NOT the viewport.** The host
viewport is captured with `glGetIntegerv` at the top of `ProcessOpenGL` and put
back by hand before the final pass. Without that the composite inherits whatever
size the last off-screen pass left, and the effect renders into a corner of the
frame — which in any viewer that shows transparency as white reads as the effect
having blown out to solid white rather than as a viewport bug.

**A TEXT parameter without a `SetTextParameter` override makes
`FF_INSTANTIATE_GL` fail for the whole plugin.** The SDK's `instantiateGL` sets
every parameter's default on a fresh instance and **deletes the instance if any
set returns FF_FAIL**; the base class's `SetTextParameter` is a stub returning
exactly that. So the About block would mean no real host could instantiate this
at all — while every harness that drives the plugin class directly passes,
because they bypass `plugMain`. `Tilter::SetTextParameter` exists solely for
this.

**Option parameters do NOT hold 0..1.** They hold the element value the operator
chose — 0, 1, 2 — so they are read through `controls::option()`, which rounds and
clamps. A stale composition naming an element that no longer exists is why it
clamps.

**A ranged `FF_TYPE_STANDARD` parameter cannot have a ranged default.**
`SetParamInfo` clamps a default into 0..1 *before* returning and there is no
`SetParamDefault`, so every standard control here lives in 0..1 and is converted
in `Controls.cpp`. That file is the single home for those curves, and it is
shared with the OFX build so a preset cannot mean different things in Resolume
and Resolve.

**`Angle` runs ±90°, not ±180°.** A band is its own mirror image at 180°, so a
full turn would make every angle appear twice — and would make a slider sweep
from one end to the other render the identical picture at both ends, which
reports a working control as dead.

**`flat`, `active`, `filter`, `input`, `output`, `sample` and `common` are GLSL
reserved words**, and a shader that will not compile surfaces only at runtime as
"the effect does nothing". That is what `Diag` is for: it names the stage.

---

## Checking your work

`tools/verify.sh` runs the lot. The ones that matter check different things:

- **`--focus`** compares the CoC field the GPU actually wrote against
  `Focus.cpp`, per geometry, at a few thousand points. Two things make it worth
  trusting. Its **tolerance is derived, not chosen** — everything round-trips
  through RGBA16F, whose mantissa gives a quantum of 2⁻¹¹ = 0.000489, and the
  three analytic geometries come out at exactly that, which is the evidence they
  agree to the last bit the storage can hold. Image Depth is allowed
  `1 + 1.5/feather` quanta because it reads a second fp16 buffer whose error the
  focus ramp then amplifies. And it carries a **control case**: the same
  comparison against a deliberately wrong geometry, which must fail. Rows of
  agreement are exactly when to ask whether a test can fail at all.
- **`--blur`** measures local detail inside the sharp band and far outside it,
  then walks the Blur control demanding detail fall monotonically. It catches a
  blur that does nothing, one that blurs everything, a focus field wired in
  backwards, and the aliasing above.
- **`--aperture`** is the only thing that checks the *shape*. A blade count that
  never reached the shader shows up here and nowhere else.
- **`--presets`** catches a preset that renders flat, or that is identical to
  another because its table row never reached the parameters.
- **`sweep.py`** is the only thing that catches a dead control.
- **`--sheet`** asserts nothing and is the most valuable tool in the repo.

### Measure in a place where the quantity means something

Two of this session's dead ends were measurement bugs that looked exactly like
code bugs, and both are worth knowing before writing another check here.

**Detail in a fixed strip is not monotonic in the blur radius**, because a wider
blur drags structure *into* the strip from outside it. The test scene has
highlights on a 64-pixel lattice; at a 10px radius the row above the strip stays
out of it and at 20px it spreads in. Measured that way the numbers read as a
blur that gives up past halfway, and measured forty rows lower they read the
opposite. The fix is to defocus the **whole frame** and measure **all** of it,
so structure can move around inside the measured region without entering or
leaving it.

**Compare like with like across a resolution change.** The Image Depth mirror
check disagreed by 0.4 — two hundred times the tolerance — purely because the
shader reads its cues through a `GL_LINEAR` sampler on a quarter-resolution
buffer while the harness was doing a nearest lookup. Matching the filter (and
the half-texel offset, since `texture()` puts texel centres at `(i+0.5)/size`)
took it to 0.00254.

---

## Things deliberately not done

- **No scatter pass.** See the traps: it is the only way to bokeh a true point
  highlight, and it is a different architecture — a draw call per bright pixel,
  with sorting and blending — for a case that ten-pixel highlights already
  cover.
- **No real depth input.** Resolume has no depth channel to give us. Image Depth
  guesses from aerial perspective and is labelled a guess *in the host's own
  dropdown*, because an operator picking it should know that before they wonder
  why a bright foreground went soft.
- **No near/far layer separation.** A single-layer gather with a per-tap reach
  test handles both directions well enough: a blurred foreground has a large CoC
  so it spreads over a sharp background, and a sharp background has a small one
  so it does not bleed into the blur beside it. Splitting into layers would buy
  correct occlusion behind a blurred foreground edge, for a lot of machinery.
- **No anamorphic squeeze.** `Iris Aspect` makes an oval iris, which is the part
  of that look anyone actually wants. A real squeeze belongs in the composition's
  transform, not in a lens effect.
- **No chromatic aberration on the sharp band.** It scales with defocus because
  that is what the real thing does; a global CA control would be a different
  effect wearing this one's clothes.

Related: [old-cathode](https://github.com/stoatworks-labs/old-cathode) and
[flipbook](https://github.com/stoatworks-labs/flipbook) (the CMake, harness,
Diag and preset patterns came from there), downpour, orrery, porthole.

## Factory presets

`source/Presets.h` is one table of named lenses in the host-facing 0..1 space,
and it drives **both** builds, so a preset cannot drift between Resolume and
Resolve. Element 0 of the dropdown is always **Custom**, which is not in the
table: it means "the sliders are the truth".

Picking a preset copies the row into the real parameters — the FFGL side raises
`FF_EVENT_FLAG_VALUE` per changed parameter so the host re-reads its sliders,
the OFX side setValues inside one edit block so undo takes the whole preset back
at once. A host that ignores the events still renders correctly and merely shows
stale knobs. Editing any covered parameter flips back to Custom, judged **by
comparing values, never by the change reason**, so a host echoing our own writes
cannot un-set the preset.

**What a preset must not touch:** Focus X, Focus Y and Angle, because those are
where the operator's subject happens to be in their own footage and a preset
reaching into them is not a look, it is breakage. Nor Quality, which is a choice
about the machine the show is running on. Nor Mix, nor Show Focus.
