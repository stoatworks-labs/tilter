#!/usr/bin/env bash
#
# Everything that can be checked without a human, in one command.
#
#     tools/verify.sh
#
# ---------------------------------------------------------------- the point
#
# Half of this file checks things the RELEASE job checks. That is deliberate,
# and it is the lesson of the last plugin: a check that only ever runs in CI,
# after a tag, is a check that will catch you after the tag. The bundle layout,
# the plist, the architectures and the signature can all be verified here in a
# second, and the alternative is a failed release and a force-moved tag.
#
# The two that have actually bitten this fleet:
#
#   * `CFBundleExecutable` carrying the PREVIOUS plugin's name, because
#     cmake/InfoOFX.plist.in was copied from another repo. Nothing fails: the
#     bundle assembles, the binary is universal, `nm` finds the entry point and
#     a probe renders a correct frame. Then codesign says "code object is not
#     signed at all" and mentions nothing about a plist.
#
#   * A macOS build that is quietly arm64-only, because CMAKE_OSX_ARCHITECTURES
#     was set after the first target existed. The build log calls that a
#     success. Only `lipo` knows.
#
set -uo pipefail

cd "$(dirname "$0")/.."

PASS=0
FAIL=0

ok()   { printf '  \033[32mok\033[0m    %s\n' "$1"; PASS=$((PASS+1)); }
bad()  { printf '  \033[31mFAIL\033[0m  %s\n' "$1"; FAIL=$((FAIL+1)); }
head_() { printf '\n\033[1m%s\033[0m\n' "$1"; }

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler, before a host has to find out.
#
# A shader that will not compile presents to an operator as "the effect does
# nothing", with the real message buried in the diagnostics log -- so without
# this it is caught at run time, in a host, or not at all.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails.
#---------------------------------------------------------------------------
shaders_compile() {
    local dir bad=0 n=0 shader

    if ! command -v glslc >/dev/null 2>&1; then
        printf '   skipped: glslc not installed (brew install shaderc)\n'
        return 0
    fi

    dir="$( mktemp -d )"

    python3 - "$dir" <<'SHADERS_PY'
import re, sys, pathlib
out = pathlib.Path( sys.argv[ 1 ] )

# Where this repo keeps its GLSL.
FILES = [
	"source/shaders/Bokeh.cpp",
	"source/shaders/CoC.cpp",
	"source/shaders/Composite.cpp",
	"source/shaders/Depth.cpp",
	"source/shaders/Downsample.cpp",
	"source/shaders/Gaussian.cpp",
	"source/shaders/Smooth.cpp",
	"source/shaders/Vertex.cpp",
]

named, unnamed = {}, []
for f in FILES:
	text = pathlib.Path( f ).read_text()
	for m in re.finditer( r'(?:(\w+)\s*(?:\[\s*\])?\s*=\s*)?R"\((.*?)\)"', text, re.S ):
		if m.group( 1 ): named[ m.group( 1 ) ] = m.group( 2 )
		else:            unnamed.append( m.group( 2 ) )
	for m in re.finditer( r'(\w+)\s*=\s*((?:"(?:[^"\\\n]|\\.)*"\s*)+);', text ):
		named.setdefault( m.group( 1 ), "".join(
			s.encode().decode( "unicode_escape" )
			for s in re.findall( r'"((?:[^"\\\n]|\\.)*)"', m.group( 2 ) ) ) )

def emit( name, body ):
	# The vertex shader is the one that writes gl_Position; everything else is a
	# fragment shader. glslc takes the stage from the extension.
	ext = ".vert" if re.search( r"\bgl_Position\s*=", body ) else ".frag"
	( out / ( name + ext ) ).write_text( body )

for name, body in named.items():
	if body.lstrip().startswith( "#version" ) and "void main" in body:
		emit( name, body )
SHADERS_PY

    for shader in "$dir"/*.vert "$dir"/*.frag; do
        [ -e "$shader" ] || continue
        n=$(( n + 1 ))
        if ! glslc --target-env=opengl4.5 -fauto-map-locations \
               "$shader" -o /dev/null 2>"$dir/err"; then
            printf '   %s does not compile\n' "$( basename "$shader" )"
            sed "s|$dir/||; s|^|      |" "$dir/err"
            bad=$(( bad + 1 ))
        fi
    done

    if [ "$n" -eq 0 ]; then
        # No shaders at all is a FAILURE, not a pass. It means the extraction
        # above has lost track of where this repo keeps its GLSL, and a check
        # that silently looks at nothing is worse than no check.
        printf '   no shaders were extracted -- the extraction has gone stale\n'
        rm -rf "$dir"
        return 1
    fi

    if [ "$bad" -eq 0 ]; then
        printf '   %d shaders, all compile\n' "$n"
    fi
    rm -rf "$dir"
    return "$bad"
}

#---------------------------------------------------------------------------
head_ "Shaders"
#---------------------------------------------------------------------------
if shaders_compile; then
    ok "every shader compiles"
else
    bad "a shader does not compile"
fi

# ---------------------------------------------------------------------------
head_ "Build (universal, both plugins)"
# ---------------------------------------------------------------------------
# A fresh configure, because the thing most likely to be stale is the cache
# that decides the architectures.
if cmake -B build -DCMAKE_BUILD_TYPE=Release >/tmp/tilter-configure.log 2>&1 \
   && cmake --build build >/tmp/tilter-build.log 2>&1; then
    ok "configured and built"
else
    bad "build failed -- see /tmp/tilter-build.log"
    tail -20 /tmp/tilter-build.log
    exit 1
fi

FFGL_BIN="build/Tilter.bundle/Contents/MacOS/Tilter"
OFX_BUNDLE="build/Tilter.ofx.bundle"
OFX_BIN="$OFX_BUNDLE/Contents/MacOS/Tilter.ofx"

# ---------------------------------------------------------------------------
head_ "Architectures"
# ---------------------------------------------------------------------------
for binary in "$FFGL_BIN" "$OFX_BIN"; do
    if [ ! -f "$binary" ]; then
        bad "missing: $binary"
        continue
    fi
    archs=$(lipo -archs "$binary" 2>/dev/null)
    # Both, or Resolume's Intel build and half the installed base cannot load it.
    if [[ "$archs" == *arm64* && "$archs" == *x86_64* ]]; then
        ok "$(basename "$binary") is universal ($archs)"
    else
        bad "$(basename "$binary") is not universal: got '$archs'"
    fi
done

# ---------------------------------------------------------------------------
head_ "Entry points"
# ---------------------------------------------------------------------------
# Plain grep, never `grep -q`: under `set -o pipefail` a quiet grep exits as
# soon as it matches, nm gets SIGPIPE, and the pipeline reports failure for a
# check that actually PASSED.
if nm -gU "$FFGL_BIN" 2>/dev/null | grep '_plugMain' > /dev/null; then
    ok "FFGL exports plugMain"
else
    bad "FFGL does NOT export plugMain -- the host will load nothing"
fi

if nm -gU "$OFX_BIN" 2>/dev/null | grep '_OfxGetPlugin' > /dev/null; then
    ok "OFX exports OfxGetPlugin"
else
    bad "OFX does NOT export OfxGetPlugin"
fi

# The plugin registers itself from a file-scope constructor that nothing
# references by name. In a STATIC archive the linker may drop the whole
# translation unit, giving a bundle that loads, exports plugMain, and reports
# that it contains no plugins. tilter_core is an OBJECT library for this reason.
if strings "$FFGL_BIN" 2>/dev/null | grep -F 'TL01' > /dev/null; then
    ok "FFGL bundle carries its plugin ID (registration survived linking)"
else
    bad "plugin ID TL01 is not in the binary -- the registration TU was dropped"
fi

if strings "$FFGL_BIN" 2>/dev/null | grep -F 'tilter 0' > /dev/null; then
    ok "build stamp present"
else
    bad "build stamp missing from the FFGL binary"
fi

# ---------------------------------------------------------------------------
head_ "Bundle metadata (the release-time traps)"
# ---------------------------------------------------------------------------
declared=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" \
           "$OFX_BUNDLE/Contents/Info.plist" 2>/dev/null)
onDisk=$(basename "$OFX_BIN")
if [ "$declared" = "$onDisk" ]; then
    ok "OFX CFBundleExecutable '$declared' matches the binary on disk"
else
    bad "OFX CFBundleExecutable is '$declared' but the binary is '$onDisk'"
    echo "        this passes every build and test, and fails codesign at release"
fi

identifier=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" \
             "$OFX_BUNDLE/Contents/Info.plist" 2>/dev/null)
case "$identifier" in
    *tilter*) ok "OFX bundle identifier is ours ($identifier)" ;;
    *)        bad "OFX bundle identifier looks carried over: '$identifier'" ;;
esac

# ---------------------------------------------------------------------------
head_ "Signing"
# ---------------------------------------------------------------------------
# Ad-hoc, on a COPY. This is the exact command the release job runs, and it is
# the one that reads the plist above and refuses.
SIGDIR=$(mktemp -d)
cp -R "$OFX_BUNDLE" "$SIGDIR/" 2>/dev/null
if codesign --force --sign - "$SIGDIR/$(basename "$OFX_BUNDLE")" >/dev/null 2>&1 \
   && codesign --verify "$SIGDIR/$(basename "$OFX_BUNDLE")" >/dev/null 2>&1; then
    ok "OFX bundle ad-hoc signs and verifies"
else
    bad "OFX bundle will not sign -- check CFBundleExecutable above"
fi
rm -rf "$SIGDIR"

SIGDIR=$(mktemp -d)
cp -R "build/Tilter.bundle" "$SIGDIR/" 2>/dev/null
if codesign --force --sign - "$SIGDIR/Tilter.bundle" >/dev/null 2>&1 \
   && codesign --verify "$SIGDIR/Tilter.bundle" >/dev/null 2>&1; then
    ok "FFGL bundle ad-hoc signs and verifies"
else
    bad "FFGL bundle will not sign"
fi
rm -rf "$SIGDIR"

# ---------------------------------------------------------------------------
head_ "The lens"
# ---------------------------------------------------------------------------
run_check() {
    local label="$1"; shift
    if "$@" > /tmp/tilter-check.log 2>&1; then
        ok "$label"
    else
        bad "$label"
        sed 's/^/        /' /tmp/tilter-check.log | tail -12
    fi
}

run_check "focus field matches its GLSL mirror, all four shapes" ./build/tiltest --focus
run_check "blur blurs where it should and only there"            ./build/tiltest --blur
run_check "aperture shape and size reach the picture"            ./build/tiltest --aperture
run_check "every factory preset is distinct and non-degenerate"  ./build/tiltest --presets
run_check "presets survive every host behaviour"                 ./build/tiltest --hosts

# ---------------------------------------------------------------------------
head_ "Controls"
# ---------------------------------------------------------------------------
# The only thing that catches a GLSL uniform name that does not match the C++:
# glGetUniformLocation returns -1, glUniform(-1) is a documented no-op, and
# nothing else anywhere says a word.
run_check "no dead controls" python3 tools/sweep.py

# ---------------------------------------------------------------------------
head_ "Browser demo"
# ---------------------------------------------------------------------------
# The demo page holds a second copy of all eight shaders, and two copies drift.
# Nothing else can catch it: a demo rendering a *plausible* picture looks
# exactly like one rendering the right picture, and the whole claim of that page
# is that it runs the plugin's own shader rather than a lookalike.
if [ -f demo/tools/check_shaders.py ]; then
    run_check "demo shaders identical to the plugin's" python3 demo/tools/check_shaders.py
else
    printf '  \033[33mskip\033[0m  no demo/tools/check_shaders.py\n'
fi

# ---------------------------------------------------------------------------
head_ "OpenFX render"
# ---------------------------------------------------------------------------
PROBE="../resolume-ofx-bridge/build/ofxprobe"
if [ -x "$PROBE" ]; then
    if "$PROBE" --dir build --render com.stoatworks.tilter --size 320x180 \
                --out /tmp/tilter-ofx.bmp > /tmp/tilter-probe.log 2>&1; then
        # A render that "succeeds" having changed nothing is not evidence that
        # the plugin ran, only that the host called it.
        if grep -E '[0-9]+ of [0-9]+ bytes differ' /tmp/tilter-probe.log > /dev/null; then
            ok "OFX bundle renders and changes the picture"
        else
            bad "OFX rendered but the output matches the input"
        fi
    else
        bad "ofxprobe failed -- see /tmp/tilter-probe.log"
    fi
else
    printf '  \033[33mskip\033[0m  ofxprobe not built (%s)\n' "$PROBE"
fi

# ---------------------------------------------------------------------------
head_ "Contact sheet"
# ---------------------------------------------------------------------------
# Asserts nothing, and is the most valuable thing here. Every real bug in this
# fleet's plugins was found by looking at one of these rather than by a number
# coming out wrong -- including this plugin's inverted focus overlay, which
# passed every check above.
if ./build/tiltest --sheet docs/contact-sheet.png > /dev/null 2>&1; then
    ok "wrote docs/contact-sheet.png -- LOOK AT IT"
else
    bad "could not write the contact sheet"
fi

# ---------------------------------------------------------------------------
printf '\n'
if [ "$FAIL" -eq 0 ]; then
    printf '\033[32m%d checks passed\033[0m\n' "$PASS"
    exit 0
fi
printf '\033[31m%d passed, %d FAILED\033[0m\n' "$PASS" "$FAIL"
exit 1
