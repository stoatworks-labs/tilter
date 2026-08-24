# Notes

Working notes for this repo: status, decisions, and the traps that have actually bitten.
Migrated out of Claude Code's memory on 2026-08-24, so they are written in the first
person and dated by when each thing was learned — that date is usually the useful part.

Cross-cutting notes that are not specific to this repo live in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).

## tilter

*tilter — tilt-shift lens FFGL+OFX plugin for Resolume/Resolve; PUBLIC MIT v0.1.0 RELEASED 2026-08-17, all seven homes live; the CoC field and the blur are independent*

**tilter** (built and released 2026-08-17) — a tilt-shift lens as an FFGL 2.1
effect for Resolume (`Tilter`, ID **`TL01`**) plus an OpenFX build for
Resolve/Nuke/Natron/Vegas. C++17 + GLSL 4.1, CMake, `~/Projects/resolume/tilter`,
**PUBLIC MIT** at `stoatworks-labs/tilter`, **v0.1.0 released**.

**The one idea:** the plugin decides how far each pixel is from focus and writes
one *signed* number; a blur consumes it. The halves never touch, so four focus
shapes × two blurs is six pieces of code, not eight. **Positive is the NEAR
side** — it is scene inverse depth minus lens inverse depth, and inverse depth
grows as things get closer. Show Focus is a view of the very buffer the blur
reads.

**What Tilted Plane actually buys, because the obvious expectation is wrong:**
for a *planar* subject perspective makes inverse depth affine in image position,
so a real tilted lens's sharp region is **exactly a straight band** — it is not a
differently-shaped Linear Band. What it buys is the falloff: inverse depth has a
floor at infinity, so the far side ramps and then **stops** at the horizon while
the near side grows without limit. A plain band ramps symmetrically forever, and
that asymmetry is the tell in fake tilt-shift.

**Three real defects, all found by measurement, all fixed:**

- **The blur ALIASED at large radii, and it got worse as the radius grew.** A
  sparse tap sum folds the source's high harmonics down into visible stipple.
  Whole-frame detail at uniform full defocus went 3.24, 6.46, 4.26, 11.32 as Blur
  rose — increasing and not even monotonic. No affordable tap count fixes it at
  4K radii (the radius reaches 8% of frame height = 173px), so the blur now runs
  on a **box-downsampled copy** at a scale chosen from the radius
  (`Downsample.cpp`). Now 8.97, 2.29, 1.45.
- Blur fetches **mirror rather than clamp** at the frame edge. Kept on merit —
  it did NOT fix the aliasing, and the comment says so with the numbers.
- **The focus overlay had near and far inverted.** Every automated check passed;
  only the contact sheet caught it.

**A gather cannot render a true point highlight and no setting will make it.** An
isolated 3px highlight covers <1% of a 19px-radius disc, so most output pixels
miss it. Clean discs around true points need a **scatter** pass — different
architecture, not present. ~10px highlights and up bokeh correctly. `--aperture`
sizes its probe accordingly and says so.

**Two measurement traps worth remembering** (both looked exactly like code bugs):
detail in a *fixed strip* is not monotonic in blur radius, because a wider blur
drags structure INTO the strip from outside — defocus the whole frame and measure
all of it. And the Image Depth mirror check disagreed by 0.4 purely because the
shader reads its cues through a **GL_LINEAR** sampler on a quarter-res buffer
while the harness did a nearest lookup; matching the filter *and the half-texel
offset* took it to 0.00254.

**Verified 19/19 by `tools/verify.sh`** from a clean universal build. The
`--focus` **tolerance is derived, not chosen**: RGBA16F's quantum is 2⁻¹¹ =
0.000489 and the three analytic geometries land on exactly that; Image Depth gets
`1 + 1.5/feather` quanta because it reads a second fp16 buffer the ramp then
amplifies. The suite carries a **control case** that must disagree. All 26
controls swept live.

**Loaded into Resolume Arena on macOS and CONFIRMED WORKING by Allan
(2026-08-17, shortly after release)** — so it registers, instantiates and renders
in a real host, which the offline harness cannot establish. README, Status and the
website statusNote were updated: a disclaimer is a factual claim and goes stale
like any other. **The OpenFX build has never been opened in Resolve** (probe
only), Windows is CI-only and never run, and nothing has been used on a live show.

**The YouTube description still carries the pre-confirmation wording** ("never
been loaded into Resolume or Resolve"). There is no write API for video metadata
— it needs YouTube Studio via Claude-in-Chrome, and watch out for the unanswered
Made-for-Kids radio silently disabling Save. See
[youtube studio edits](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_youtube_studio_edits.md).

All seven homes live 2026-08-17: repo, release (6 assets, 3/3 macOS kinds
notarised), website `/software/tilter/`, demo `tilter-demo.stoatworks-labs.com`,
**YouTube `_CCMEQP3S94`** (48.8s, rendered not filmed), **Instagram Reel
`DcIUQFTnHR8`**, both embeds, download block.

Traps are in the repo's `AGENTS.md`. See [ffgl sdk bugs](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_ffgl_sdk_bugs.md),
[plugin factory presets](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_plugin_factory_presets.md), [resolume demo kit](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_resolume_demo_kit.md),
**release workflow** (working-practice note, kept in Claude memory), **tilter video pipeline traps** (below).

## tilter video pipeline traps

*video/lib/assemble.py treats the LAST beat as an end marker and truncates the footage there — its caption never shows; plus clip choice by brightness*

Two things about `stoatworks-backend/video` that each cost a full re-render on
tilter, and neither announces itself.

**The last entry in `BEATS` is an END MARKER, not a caption.**
`assemble.body_end()` is `min(footage_duration, max(beat times))` — deliberately,
so a recorder asked for slack does not stretch the final caption over ten seconds
of a still app. The consequence is that **whatever caption sits on the final beat
is never drawn**, and every second of footage after it is discarded.

On tilter that silently removed the closing beat *and* seven seconds of footage.
The only visible sign was a **41.8s cut where 48.8s was expected** — no error, no
warning, and the cut looks fine unless you know what should have been at the end.
Add an explicit sentinel entry at the intended end:

    {"t": 42.0, "caption": "", "focus": WHOLE},   # sentinel, never drawn

Worth checking older projects against this: **regauss's last beat is at 35.0 with
41s of footage**, so its final caption ("Interference Only, to stack on your own
CRT look") is very likely never shown either. Not verified — check before
"fixing" it, it may be deliberate slack.

**Choose clips by measured brightness and by what the effect needs, not by name.**
Resolume's `Shop74` library is mostly isolated objects on white or black, with
almost no depth. For tilter:

- `Enter5_12` is the **only** clip with a vanishing point in it — and it is nearly
  black, hairlines on a dark field. Opening on it gave eight seconds of black
  rectangle. It now carries exactly the one beat where **Show Focus is on** and
  the overlay supplies the colour the clip lacks.
- `NoHopeJustFear_44` (dense, brightest, most saturated) opens and closes.
- `IntoTheGlow_21` is the only clip with clipped highlights bright enough for a
  bokeh disc to form around — an aperture polygon is invisible without one.
- `Metalive 01` for the colour push, `FogAndDust_3` for aberration on a dark field.

Sample frames into a montage and **look** before writing the cue sheet; the same
mistake is recorded in regauss's render.py, which is where the warning came from.

**An FFGL plugin's video is rendered, not filmed** — no window, control surface is
Resolume's inspector, nothing addressable in the accessibility tree. The harness
grows a `--pipe` mode (raw RGBA in on stdin, out on stdout) plus `--script`, a
`frame Parameter Name value` cue sheet interpolated linearly. **Option and boolean
parameters must STEP** (two keys one frame apart) or a ramp passes through every
setting in between.

See **tilter** (below), **release workflow** (working-practice note, kept in Claude memory),
**instagram publish scope** (working-practice note, kept in Claude memory).
