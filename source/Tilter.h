#pragma once

#include <FFGLSDK.h>

#include <string>

#include "Controls.h"
#include "PassBuffer.h"
#include "Presets.h"
#include "StoatworksAboutParams.h"

/**
    Tilter -- a tilt-shift lens for Resolume.

    A tilt-shift lens is one whose focal plane is not parallel to the sensor.
    Photographers bought them to keep buildings straight; everybody else
    discovered that pointing one at a city from a long way up makes it look like
    a model railway, because a plane of focus that shallow is something the eye
    only ever sees on things a few inches across.

    ------------------------------------------------------------- the one idea

    **The plugin decides how far each pixel is from focus. The blur decides what
    that looks like.** Those are completely independent. Four focus geometries
    and two blur models are six pieces of code, not eight -- a geometry writes
    one signed number per pixel into a buffer and a blur reads it, and neither
    knows what the other is.

    Everything else follows. A new geometry is one branch in one shader. A new
    blur is one pass that reads the same buffer. The focus overlay is a view of
    that buffer, which is why it can be trusted to show what the blur is
    actually doing rather than a second opinion about it.

    ------------------------------------------------------------------ no clock

    Nothing here animates, so unlike most of the fleet there is no time model at
    all: the output is a pure function of the input frame and the parameters.
    Two consequences worth knowing. A re-render is bit-identical, and any
    animation comes from the host keyframing the controls, which is where it
    belongs.

    See Shaders.h for the stages and AGENTS.md for the traps.
*/
class Tilter : public CFFGLPlugin
{
public:
	Tilter();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Display-only text still needs this.
	///
	/// The SDK's `instantiateGL` sets EVERY parameter's default on a fresh
	/// instance and **deletes the instance if any set returns FF_FAIL** -- and
	/// the base class's SetTextParameter is a stub that returns exactly that.
	/// So declaring the About block without overriding this means no real host
	/// can instantiate the plugin at all, while every harness that drives the
	/// plugin class directly passes, because they bypass plugMain.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	/// Everything the operator can reach, in the order Resolume shows it: what
	/// is in focus, what the lens does to everything else, what the photograph
	/// looks like afterwards, and how much of it to keep.
	///
	/// Public because the harness drives the plugin by parameter id and needs
	/// PT_COUNT to enumerate them.
	enum ParamID : FFUInt32
	{
		//Focus
		PT_GEOMETRY,
		PT_FOCUS_X,
		PT_FOCUS_Y,
		PT_ANGLE,
		PT_WIDTH,
		PT_FEATHER,
		PT_ELLIPSE_ASPECT,
		PT_HORIZON,
		PT_TILT,
		PT_RATE,
		PT_DEPTH_BIAS,
		PT_DEPTH_CONTRAST,
		PT_INVERT,

		//Lens
		PT_BLUR_MODEL,
		PT_BLUR,
		PT_QUALITY,
		PT_BLADES,
		PT_BLADE_ROTATION,
		PT_HIGHLIGHT,

		//Photograph
		PT_SATURATION,
		PT_CONTRAST,
		PT_VIGNETTE,
		PT_ABERRATION,

		//Output
		PT_SHOW_FOCUS,
		PT_MIX,

		//Preset. Declared after the real controls so their IDs -- which a saved
		//composition refers to -- do not shift under existing users.
		PT_PRESET,

		//About. FFGL has no window, so the name, the version and the links are
		//parameters the host draws. See StoatworksAboutParams.h.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

	/// params[] as the shared control struct, so FFGL and OFX ask the same
	/// question of the same code. Public for the harness.
	tilter::controls::HostValues hostValues() const;

	/// Test seams: the intermediate buffers, by GL texture handle.
	///
	/// `tiltest --focus` is the only thing standing between Focus.cpp and its
	/// GLSL mirror in CoC.cpp, and it can only do that job by reading what the
	/// GPU actually wrote. Recovering the field from the finished picture was
	/// the alternative and it is a worse test: the Show Focus overlay's tint
	/// arithmetic would sit between the thing under test and the measurement,
	/// so a wrong field and a wrong tint would be indistinguishable.
	///
	/// Valid only after a successful ProcessOpenGL, and only until the next
	/// one. Zero before that.
	GLuint CoCTextureForTest() const
	{
		return cocBuffer.GetTextureInfo().Handle;
	}

	/// The smoothed depth cues, for the same reason. The pre-pass that fills
	/// this is GPU-only and has no C++ mirror, so --focus feeds the values it
	/// reads here BACK into focus::defocus() -- which keeps the comparison
	/// about the geometry arithmetic, which is the part that is mirrored.
	GLuint DepthTextureForTest() const
	{
		return depthBuffer[ 0 ].GetTextureInfo().Handle;
	}

private:
	/// The ParamID each presets::Param drives, in presets::Param order. The
	/// preset table stays host-agnostic; this is the FFGL binding of it.
	static constexpr unsigned int kPresetParamIDs[ tilter::presets::kParamCount ] = {
		PT_GEOMETRY, PT_WIDTH, PT_FEATHER, PT_ELLIPSE_ASPECT, PT_HORIZON, PT_TILT,
		PT_RATE, PT_DEPTH_BIAS, PT_DEPTH_CONTRAST, PT_INVERT, PT_BLUR_MODEL, PT_BLUR,
		PT_BLADES, PT_BLADE_ROTATION, PT_HIGHLIGHT, PT_SATURATION, PT_CONTRAST,
		PT_VIGNETTE, PT_ABERRATION
	};

	/// Copy a factory preset's values into params[] and raise value events so
	/// the host re-reads the sliders. `presetIndex` is 1-based; 0 is Custom.
	void applyPreset( int presetIndex );

	bool compileShaders();
	void releaseBuffers();

	ffglex::FFGLShader downsampleShader;
	ffglex::FFGLShader depthShader;
	ffglex::FFGLShader smoothShader;
	ffglex::FFGLShader cocShader;
	ffglex::FFGLShader gaussianShader;
	ffglex::FFGLShader bokehShader;
	ffglex::FFGLShader compositeShader;
	ffglex::FFGLScreenQuad quad;

	tilter::PassBuffer depthBuffer[ 2 ];///< Quarter res. Image Depth mode only.
	tilter::PassBuffer cocBuffer;       ///< The signed defocus field. Full resolution.

	/// The box-downsampled picture the blur actually works on, and the blur's
	/// own ping-pong. All three at composition size / lens.blurScale -- see
	/// Downsample.cpp for why the blur does not run at full resolution.
	tilter::PassBuffer sourceBuffer;
	tilter::PassBuffer blurBuffer[ 2 ];///< Gaussian needs both; Bokeh uses one.

	float params[ PT_COUNT ];

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
