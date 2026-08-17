#pragma once

/**
    Factory presets: named lenses an operator can reach in one gesture.

    Each entry is *a lens on a subject* -- a long lens wide open on a face, a
    view camera swung down over a street, a cheap plastic one that was never
    corrected for anything -- rather than a random set of slider positions. The
    controls here are the parts of an optical system, so a coherent look is a
    coherent story about which lens this is.

    The values live in the same 0..1 host-facing space both builds expose, so
    ONE table drives the FFGL and the OFX plugin and a preset cannot look
    different in Resolume and Resolve. Plain data only; the machinery that
    applies it lives with each host's glue.

    Element 0 of the dropdown is "Custom" and is not in this table: it means
    "the sliders are the truth".

    -------------------------------------------------- what a preset must not set

    **Not the framing.** Focus X, Focus Y and Angle are where the operator's
    subject happens to be in their own footage. A preset that reached into those
    would take a correctly placed focal plane and move it off the subject, which
    is not a look, it is breakage.

    **Not Quality.** That is a choice about the machine the show is running on,
    not about the picture -- the same reasoning that keeps downpour's Font out
    of its presets.

    **Not Mix, and not Show Focus.** One is the wet/dry every effect has and the
    other is a diagnostic.
*/

namespace tilter
{
namespace presets
{
/// The parameters a preset sets, in one fixed order. The FFGL build binds this
/// order to its ParamIDs and the OFX build to its param handles; both
/// static_assert against kParamCount so the three lists cannot drift silently.
enum Param
{
	kGeometry,
	kWidth,
	kFeather,
	kEllipseAspect,
	kHorizon,
	kTilt,
	kRate,
	kDepthBias,
	kDepthContrast,
	kInvert,
	kBlurModel,
	kBlur,
	kBlades,
	kBladeRotation,
	kHighlight,
	kSaturation,
	kContrast,
	kVignette,
	kAberration,
	kParamCount
};

struct Preset
{
	const char* name;
	float v[ kParamCount ];
};

// Option values are element indices: Geometry 0 Linear Band / 1 Radial /
// 2 Tilted Plane / 3 Image Depth; Blur Model 0 Gaussian / 1 Bokeh Disc;
// Blades 0 Circular / 1 five / 2 six / 3 seven / 4 eight / 5 nine.
// Saturation and Contrast sit at unity on 0.5. Invert is a boolean and must be
// exactly 0 or 1 -- a fractional value there means the FFGL build gets 0.35 and
// the OFX build gets false, and the two stop agreeing.
inline constexpr Preset kPresets[] = {
	// The postcard: a narrow band across a wide scene, everything else gone,
	// colour pushed the way a photograph of a painted model looks.
	{ "Model Village",
	  { /*Geom*/ 0, /*Width*/ 0.18f, /*Feather*/ 0.35f, /*Ellipse*/ 0.5f,
	    /*Horizon*/ 0.35f, /*Tilt*/ 0.5f, /*Rate*/ 0.5f, /*DBias*/ 0.7f, /*DContrast*/ 0.45f,
	    /*Invert*/ 0.0f, /*Model*/ 1, /*Blur*/ 0.55f, /*Blades*/ 2, /*BladeRot*/ 0.0f,
	    /*Highlight*/ 0.4f, /*Sat*/ 0.66f, /*Contrast*/ 0.60f, /*Vig*/ 0.3f, /*Aberr*/ 0.15f } },

	// Shot from a bridge onto moving traffic: a very thin plane of focus and
	// enough blur that nothing outside it is readable as a real vehicle.
	{ "Toy Traffic",
	  { /*Geom*/ 0, /*Width*/ 0.10f, /*Feather*/ 0.22f, /*Ellipse*/ 0.5f,
	    /*Horizon*/ 0.35f, /*Tilt*/ 0.5f, /*Rate*/ 0.5f, /*DBias*/ 0.7f, /*DContrast*/ 0.45f,
	    /*Invert*/ 0.0f, /*Model*/ 1, /*Blur*/ 0.75f, /*Blades*/ 1, /*BladeRot*/ 0.2f,
	    /*Highlight*/ 0.6f, /*Sat*/ 0.72f, /*Contrast*/ 0.62f, /*Vig*/ 0.35f, /*Aberr*/ 0.2f } },

	// A view camera looking down at a street: the focal plane laid along the
	// ground so the whole surface stays sharp and only what stands up off it
	// goes soft. Tilt past halfway is what does that.
	{ "Street Level",
	  { /*Geom*/ 2, /*Width*/ 0.12f, /*Feather*/ 0.30f, /*Ellipse*/ 0.5f,
	    /*Horizon*/ 0.30f, /*Tilt*/ 0.68f, /*Rate*/ 0.55f, /*DBias*/ 0.7f, /*DContrast*/ 0.45f,
	    /*Invert*/ 0.0f, /*Model*/ 1, /*Blur*/ 0.5f, /*Blades*/ 2, /*BladeRot*/ 0.0f,
	    /*Highlight*/ 0.35f, /*Sat*/ 0.60f, /*Contrast*/ 0.56f, /*Vig*/ 0.25f, /*Aberr*/ 0.12f } },

	// The same camera used properly rather than for the trick: a shallow plane
	// that stays parallel to the subject, no colour push, no vignette to speak
	// of. What a tilt-shift lens is actually sold for.
	{ "Architect's Model",
	  { /*Geom*/ 2, /*Width*/ 0.22f, /*Feather*/ 0.40f, /*Ellipse*/ 0.5f,
	    /*Horizon*/ 0.28f, /*Tilt*/ 0.5f, /*Rate*/ 0.42f, /*DBias*/ 0.7f, /*DContrast*/ 0.45f,
	    /*Invert*/ 0.0f, /*Model*/ 1, /*Blur*/ 0.35f, /*Blades*/ 4, /*BladeRot*/ 0.0f,
	    /*Highlight*/ 0.2f, /*Sat*/ 0.5f, /*Contrast*/ 0.5f, /*Vig*/ 0.1f, /*Aberr*/ 0.05f } },

	// A fast prime wide open on a subject in the middle of frame. Round
	// aperture, because a lens wide open has no polygon to show, and the
	// highlight weighting turned up because that is the entire look.
	{ "Portrait Iris",
	  { /*Geom*/ 1, /*Width*/ 0.20f, /*Feather*/ 0.32f, /*Ellipse*/ 0.55f,
	    /*Horizon*/ 0.35f, /*Tilt*/ 0.5f, /*Rate*/ 0.5f, /*DBias*/ 0.7f, /*DContrast*/ 0.45f,
	    /*Invert*/ 0.0f, /*Model*/ 1, /*Blur*/ 0.6f, /*Blades*/ 0, /*BladeRot*/ 0.0f,
	    /*Highlight*/ 0.7f, /*Sat*/ 0.54f, /*Contrast*/ 0.52f, /*Vig*/ 0.3f, /*Aberr*/ 0.1f } },

	// An oval iris and a lot of lateral colour: the scope-lens look, without
	// pretending to be a real anamorphic squeeze.
	{ "Anamorphic Dream",
	  { /*Geom*/ 1, /*Width*/ 0.16f, /*Feather*/ 0.34f, /*Ellipse*/ 0.78f,
	    /*Horizon*/ 0.35f, /*Tilt*/ 0.5f, /*Rate*/ 0.5f, /*DBias*/ 0.7f, /*DContrast*/ 0.45f,
	    /*Invert*/ 0.0f, /*Model*/ 1, /*Blur*/ 0.62f, /*Blades*/ 0, /*BladeRot*/ 0.0f,
	    /*Highlight*/ 0.8f, /*Sat*/ 0.58f, /*Contrast*/ 0.5f, /*Vig*/ 0.45f, /*Aberr*/ 0.5f } },

	// Something moulded rather than ground: soft everywhere off centre,
	// coloured fringes it was never corrected for, and a heavy falloff.
	// Gaussian on purpose -- a lens this bad has no clean aperture to show.
	{ "Cheap Lens",
	  { /*Geom*/ 1, /*Width*/ 0.14f, /*Feather*/ 0.55f, /*Ellipse*/ 0.5f,
	    /*Horizon*/ 0.35f, /*Tilt*/ 0.5f, /*Rate*/ 0.5f, /*DBias*/ 0.7f, /*DContrast*/ 0.45f,
	    /*Invert*/ 0.0f, /*Model*/ 0, /*Blur*/ 0.45f, /*Blades*/ 0, /*BladeRot*/ 0.0f,
	    /*Highlight*/ 0.1f, /*Sat*/ 0.42f, /*Contrast*/ 0.44f, /*Vig*/ 0.6f, /*Aberr*/ 0.7f } },

	// The guessed-depth mode doing what it is actually good at: haze on a wide
	// landscape, where "smooth and bright" really does mean "a long way off".
	{ "Landscape Haze",
	  { /*Geom*/ 3, /*Width*/ 0.12f, /*Feather*/ 0.45f, /*Ellipse*/ 0.5f,
	    /*Horizon*/ 0.35f, /*Tilt*/ 0.5f, /*Rate*/ 0.5f, /*DBias*/ 0.30f, /*DContrast*/ 0.6f,
	    /*Invert*/ 0.0f, /*Model*/ 0, /*Blur*/ 0.4f, /*Blades*/ 0, /*BladeRot*/ 0.0f,
	    /*Highlight*/ 0.25f, /*Sat*/ 0.56f, /*Contrast*/ 0.54f, /*Vig*/ 0.2f, /*Aberr*/ 0.08f } },
};

inline constexpr int kCount = int( sizeof( kPresets ) / sizeof( kPresets[ 0 ] ) );

} // namespace presets
} // namespace tilter
