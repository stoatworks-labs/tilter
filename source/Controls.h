#pragma once

#include "Focus.h"

namespace tilter
{
/**
    The one place a slider position becomes a physical quantity.

    -------------------------------------------------------------------- why

    Two reasons, and the second is the one that bites.

    **A ranged FF_TYPE_STANDARD parameter cannot have a ranged default.** The
    SDK's `SetParamInfo` clamps a default into 0..1 *before* returning, and
    `SetParamRange` can only be called afterwards because it finds the parameter
    by id. There is no `SetParamDefault`. So a control declared in degrees
    cannot declare a default in degrees -- 90 silently becomes 1. Every standard
    parameter here therefore lives in 0..1 and is converted on the way through,
    which is this file.

    **FFGL and OFX must agree.** They expose the same 0..1 controls and the same
    factory presets, so if the conversion lived in each host's glue there would
    be two copies of every curve and a preset would mean something slightly
    different in Resolume and in Resolve. Both builds fill a `HostValues` and
    ask here.

    ------------------------------------------------------------- the curves

    Anything that is a ratio -- the ellipse's aspect, the blur growth rate --
    converts exponentially, so that half a slider is unity and equal distances
    either side are reciprocal factors. Anything that is a position or an amount
    converts linearly. Anything centred on "no change" puts that at 0.5.
*/
namespace controls
{
/// The controls exactly as the host holds them: every one 0..1, option
/// parameters holding their element index. Both builds fill this.
struct HostValues
{
	float geometry = 0.0f;
	float focusX = 0.5f;
	float focusY = 0.55f;
	float angle = 0.5f;
	float width = 0.25f;
	float feather = 0.4f;
	float ellipseAspect = 0.5f;
	float horizon = 0.35f;
	float tilt = 0.5f;
	float rate = 0.5f;
	float depthBias = 0.7f;
	float depthContrast = 0.45f;
	float invert = 0.0f;

	float blurModel = 0.0f;
	float blur = 0.4f;
	float quality = 1.0f;
	float blades = 0.0f;
	float bladeRotation = 0.0f;
	float highlight = 0.35f;

	float saturation = 0.5f;
	float contrast = 0.5f;
	float vignette = 0.2f;
	float aberration = 0.15f;

	float showFocus = 0.0f;
	float mix = 1.0f;
};

/// Which blur is running. The order is the dropdown's, so append only.
enum BlurModel
{
	kGaussian = 0,
	kBokeh,
	kBlurModelCount
};

/// Everything the render needs that is not the focus field.
struct Lens
{
	int model = kGaussian;

	/// Maximum blur radius **in pixels**, already scaled from the frame height
	/// so that the same slider gives the same-looking picture at 720p and 4K.
	float maxRadius = 0.0f;

	int gaussianTaps = 8;///< Per side, per pass.
	int bokehSamples = 48;

	int blades = 0;///< 0 is a circular aperture; otherwise 5..9.
	float bladeRotation = 0.0f;
	float highlight = 0.0f;

	float saturation = 1.0f;
	float contrast = 1.0f;
	float vignette = 0.0f;
	float aberration = 0.0f;

	bool showFocus = false;
	float mix = 1.0f;
};

/// Element labels for the host's dropdowns.
int geometryCount();
const char* geometryLabel( int index );

int blurModelCount();
const char* blurModelLabel( int index );

int qualityCount();
const char* qualityLabel( int index );

int bladesCount();
const char* bladesLabel( int index );
/// Blade count for a Blades dropdown index. 0 means a circular aperture.
int bladesValue( int index );

/// Read an option parameter. Option parameters hold the element value the
/// operator chose -- 0, 1, 2 -- not a 0..1 fraction, so they are rounded and
/// clamped rather than scaled. A stale composition naming an element that no
/// longer exists is why it clamps.
int option( float value, int elementCount );

/// The focus field these controls describe.
focus::Field field( const HostValues& host, float aspectRatio );

/// The lens these controls describe. `frameHeight` is in pixels.
Lens lens( const HostValues& host, float frameHeight );

} // namespace controls
} // namespace tilter
