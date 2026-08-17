"""Every parameter must actually change the picture.

A GLSL uniform name that does not match the C++ is silently ignored:
glGetUniformLocation returns -1, glUniform on -1 is a documented no-op, and
nothing in the build says a word. A control can therefore be completely dead
while everything compiles, links, loads and renders. Nothing else in this repo
catches that.

So: render each parameter at both ends of its range against a baseline where
every stage is switched on, and report any that made no difference.

    python3 tools/sweep.py

Run it after adding a parameter, renaming a uniform, or moving anything between
the C++ and the GLSL. Exit code 1 means something is dead.

---------------------------------------------------------------- the two traps

Most of this plugin's controls are SUPPOSED to do nothing in the default
configuration, and a sweep that ignores that reports half of them dead and is
right to.

  * **Most controls belong to one mode.** Iris Aspect only exists in Radial.
    Horizon, Tilt and Falloff Rate only exist in Tilted Plane. Depth Focus and
    Depth Contrast only exist in Image Depth. Aperture, Aperture Angle and
    Highlights only exist in the Bokeh path. That is what CONTEXT is for: each
    of those is swept in a baseline where its mode is selected.

  * **Two settings can be the same picture.** Angle runs -90 to +90 degrees and
    a band is its own mirror image at 180, so sweeping the slider end to end
    renders the identical frame twice and reports a working Angle as dead. ENDS
    holds the endpoints for anything like that.

And one that is a property of the geometry rather than of the test: **Focus X
does nothing to a horizontal band**, because the band is infinite along its own
axis. The baseline therefore sets Angle off-axis and Tilt away from neutral, so
that both focus coordinates genuinely reach the picture. If a control ever reads
dead, work out what is masking it before assuming the test is wrong.
"""
import os
import struct
import subprocess
import sys
import tempfile
import zlib

TILTEST = "./build/tiltest"
SIZE = "640x360"
SCRATCH = tempfile.mkdtemp(prefix="tiltersweep")

# A baseline with every stage active, so nothing reads dead merely because the
# thing it modifies is switched off.
#
# Tilted Plane with a real Tilt and an off-axis Angle is deliberate: it is the
# one configuration in which BOTH focus coordinates, the horizon and the falloff
# all reach the picture at once.
BASE = {
    "Focus Shape": 2,
    "Focus X": 0.45,
    "Focus Y": 0.50,
    "Angle": 0.58,
    "Focus Width": 0.20,
    "Feather": 0.35,
    "Horizon": 0.30,
    "Tilt": 0.65,
    "Falloff Rate": 0.50,
    "Blur": 1,
    "Blur Amount": 0.50,
    "Quality": 2,
    "Aperture": 2,
    "Aperture Angle": 0.20,
    "Highlights": 0.40,
    "Saturation": 0.50,
    "Contrast": 0.50,
    "Vignette": 0.30,
    "Aberration": 0.30,
    "Mix": 1.0,
}

# Parameters that only exist in one mode, and the baseline change that switches
# that mode on.
CONTEXT = {
    "Iris Aspect": {"Focus Shape": 1},
    "Depth Focus": {"Focus Shape": 3},
    "Depth Contrast": {"Focus Shape": 3},
}

# Endpoints to sweep between, where 0 and 1 are the wrong pair.
ENDS = {
    # Discrete option parameters: sweep the real element range.
    "Focus Shape": (0, 3),
    "Blur": (0, 1),
    "Quality": (0, 2),
    "Aperture": (0, 5),
    # -90 and +90 degrees are the same band.
    "Angle": (0.50, 0.75),
    # At the extremes the focus region leaves the frame entirely and both ends
    # render one uniform blur.
    "Focus X": (0.30, 0.70),
    "Focus Y": (0.30, 0.70),
    "Horizon": (0.20, 0.80),
    "Falloff Rate": (0.20, 0.80),
    "Depth Focus": (0.25, 0.75),
    "Depth Contrast": (0.10, 0.90),
}

# Not controls: the About block is a text line and four buttons that open a
# browser.
SKIP_TYPES = {"text", "event"}


def render(path, overrides):
    args = [TILTEST, "--out", path, "--size", SIZE]
    merged = dict(BASE)
    merged.update(overrides)
    for key, value in merged.items():
        args += ["--set", f"{key}={value}"]
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        print("render failed:", result.stdout, result.stderr)
        sys.exit(1)
    with open(path, "rb") as handle:
        return handle.read()


def pixels(png):
    i = 8
    idat = b""
    width = height = 0
    while i < len(png):
        length = struct.unpack(">I", png[i:i + 4])[0]
        kind = png[i + 4:i + 8]
        data = png[i + 8:i + 8 + length]
        if kind == b"IHDR":
            width, height = struct.unpack(">II", data[:8])
        if kind == b"IDAT":
            idat += data
        i += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    return b"".join(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)] for y in range(height))


def difference(a, b):
    pa, pb = pixels(a), pixels(b)
    changed = 0
    total = 0
    count = len(pa) // 4
    for i in range(0, len(pa), 4):
        d = max(abs(pa[i] - pb[i]), abs(pa[i + 1] - pb[i + 1]), abs(pa[i + 2] - pb[i + 2]))
        if d > 2:
            changed += 1
        total += d
    return changed / count * 100.0, total / count


def parameters():
    listing = subprocess.run([TILTEST, "--list"], capture_output=True, text=True)
    if listing.returncode != 0:
        print("could not list parameters:", listing.stderr)
        sys.exit(1)

    out = []
    for line in listing.stdout.strip().splitlines()[1:]:
        fields = line.split()
        if len(fields) < 4:
            continue
        kind = fields[-2]
        name = " ".join(fields[1:-2])
        if kind in SKIP_TYPES:
            continue
        out.append(name)
    return out


def main():
    if not os.path.exists(TILTEST):
        print(f"{TILTEST} not found -- build first")
        return 1

    names = parameters()
    print(f"{'parameter':<18} {'pixels changed':>15} {'mean delta':>11}   verdict")

    dead = []
    for name in names:
        low, high = ENDS.get(name, (0.0, 1.0))
        context = CONTEXT.get(name, {})

        a = render(os.path.join(SCRATCH, "a.png"), {**context, name: low})
        b = render(os.path.join(SCRATCH, "b.png"), {**context, name: high})

        percent, mean = difference(a, b)
        # A tenth of a per cent of the frame is a real change; anything below is
        # dithering and rounding between two renders of the same picture.
        alive = percent > 0.1
        if not alive:
            dead.append(name)

        note = "" if not context else "  (" + ", ".join(f"{k}={v}" for k, v in context.items()) + ")"
        print(f"{name:<18} {percent:>14.2f}% {mean:>11.3f}   {'ok' if alive else 'DEAD'}{note}")

    print()
    if dead:
        print("DEAD CONTROLS:", ", ".join(dead))
        print("Check the uniform name matches the GLSL, and that nothing in BASE masks it.")
        return 1

    print(f"all {len(names)} controls reach the picture")
    return 0


if __name__ == "__main__":
    sys.exit(main())
