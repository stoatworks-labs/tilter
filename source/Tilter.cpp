#include "Tilter.h"

//The SDK's umbrella FFGLSDK.h pulls in every other scoped binding but leaves
//this one out (SDK b1afaf9), so it has to be reached for by hand.
#include <ffglex/FFGLScopedFBOBinding.h>

#include "Diag.h"
#include "Shaders.h"

#include <algorithm>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace tilter;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Tilter >,                // Create method
	"TL01",                                 // Plugin unique ID of maximum length 4.
	"Tilter",                               // Plugin name
	2,                                      // API major version number
	1,                                      // API minor version number
	0,                                      // Plugin major version number
	1,                                      // Plugin minor version number
	FF_EFFECT,                              // Plugin type
	"A tilt-shift lens with a real aperture behind it - the trick that makes a city look like a model railway.\n\nThe plugin decides how far each pixel is from focus; the blur decides what that looks like. Those two are completely independent, which is why there are four focus geometries and two blur models rather than eight effects.\n\nThat number is signed: 0 at the plane of focus, 1 at full blur, and the sign says which side you fell on. It is not decoration - chromatic aberration reverses across focus on a real lens, and the near and far sides do not blur at the same rate.\n\nStart from a Preset, at the bottom.",// Plugin description
	"Tilter FFGL effect"                    // About
);

namespace
{
/// glGetString returns nullptr when there is no current context, and feeding
/// that to std::string is undefined behaviour. A logging call must never be the
/// thing that brings the host down.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}
} // namespace

Tilter::Tilter()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//---------------------------------------------------------------------
	// Defaults. SetParamInfof reads each one back out of GetFloatParameter,
	// so these assignments are what the host is told the defaults are.
	//
	// They are set to a lens that is visibly doing something the moment it is
	// dropped on a layer: a band across the lower third with a real aperture
	// behind it. An effect that does nothing until six sliders are moved is an
	// effect nobody finds out is any good.
	//---------------------------------------------------------------------
	const controls::HostValues defaults;

	params[ PT_GEOMETRY ]       = defaults.geometry;
	params[ PT_FOCUS_X ]        = defaults.focusX;
	params[ PT_FOCUS_Y ]        = defaults.focusY;
	params[ PT_ANGLE ]          = defaults.angle;
	params[ PT_WIDTH ]          = defaults.width;
	params[ PT_FEATHER ]        = defaults.feather;
	params[ PT_ELLIPSE_ASPECT ] = defaults.ellipseAspect;
	params[ PT_HORIZON ]        = defaults.horizon;
	params[ PT_TILT ]           = defaults.tilt;
	params[ PT_RATE ]           = defaults.rate;
	params[ PT_DEPTH_BIAS ]     = defaults.depthBias;
	params[ PT_DEPTH_CONTRAST ] = defaults.depthContrast;
	params[ PT_INVERT ]         = defaults.invert;

	params[ PT_BLUR_MODEL ]     = defaults.blurModel;
	params[ PT_BLUR ]           = defaults.blur;
	params[ PT_QUALITY ]        = defaults.quality;
	params[ PT_BLADES ]         = defaults.blades;
	params[ PT_BLADE_ROTATION ] = defaults.bladeRotation;
	params[ PT_HIGHLIGHT ]      = defaults.highlight;

	params[ PT_SATURATION ]     = defaults.saturation;
	params[ PT_CONTRAST ]       = defaults.contrast;
	params[ PT_VIGNETTE ]       = defaults.vignette;
	params[ PT_ABERRATION ]     = defaults.aberration;

	params[ PT_SHOW_FOCUS ]     = defaults.showFocus;
	params[ PT_MIX ]            = defaults.mix;

	params[ PT_PRESET ]         = 0.0f;//Custom: the sliders are the truth

	//---------------------------------------------------------------------
	// Declaration. The groups matter: this is twenty-five parameters, and an
	// ungrouped list of twenty-five in somebody else's inspector is unusable.
	//---------------------------------------------------------------------
	SetOptionParamInfo( PT_GEOMETRY, "Focus Shape", controls::geometryCount(), params[ PT_GEOMETRY ] );
	for( int i = 0; i < controls::geometryCount(); ++i )
		SetParamElementInfo( PT_GEOMETRY, i, controls::geometryLabel( i ), static_cast< float >( i ) );

	SetParamInfof( PT_FOCUS_X, "Focus X", FF_TYPE_STANDARD );
	SetParamInfof( PT_FOCUS_Y, "Focus Y", FF_TYPE_STANDARD );
	SetParamInfof( PT_ANGLE, "Angle", FF_TYPE_STANDARD );
	SetParamInfof( PT_WIDTH, "Focus Width", FF_TYPE_STANDARD );
	SetParamInfof( PT_FEATHER, "Feather", FF_TYPE_STANDARD );
	SetParamInfof( PT_ELLIPSE_ASPECT, "Iris Aspect", FF_TYPE_STANDARD );
	SetParamInfof( PT_HORIZON, "Horizon", FF_TYPE_STANDARD );
	SetParamInfof( PT_TILT, "Tilt", FF_TYPE_STANDARD );
	SetParamInfof( PT_RATE, "Falloff Rate", FF_TYPE_STANDARD );
	SetParamInfof( PT_DEPTH_BIAS, "Depth Focus", FF_TYPE_STANDARD );
	SetParamInfof( PT_DEPTH_CONTRAST, "Depth Contrast", FF_TYPE_STANDARD );
	SetParamInfof( PT_INVERT, "Invert Focus", FF_TYPE_BOOLEAN );

	SetOptionParamInfo( PT_BLUR_MODEL, "Blur", controls::blurModelCount(), params[ PT_BLUR_MODEL ] );
	for( int i = 0; i < controls::blurModelCount(); ++i )
		SetParamElementInfo( PT_BLUR_MODEL, i, controls::blurModelLabel( i ), static_cast< float >( i ) );

	SetParamInfof( PT_BLUR, "Blur Amount", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_QUALITY, "Quality", controls::qualityCount(), params[ PT_QUALITY ] );
	for( int i = 0; i < controls::qualityCount(); ++i )
		SetParamElementInfo( PT_QUALITY, i, controls::qualityLabel( i ), static_cast< float >( i ) );

	SetOptionParamInfo( PT_BLADES, "Aperture", controls::bladesCount(), params[ PT_BLADES ] );
	for( int i = 0; i < controls::bladesCount(); ++i )
		SetParamElementInfo( PT_BLADES, i, controls::bladesLabel( i ), static_cast< float >( i ) );

	SetParamInfof( PT_BLADE_ROTATION, "Aperture Angle", FF_TYPE_STANDARD );
	SetParamInfof( PT_HIGHLIGHT, "Highlights", FF_TYPE_STANDARD );

	SetParamInfof( PT_SATURATION, "Saturation", FF_TYPE_STANDARD );
	SetParamInfof( PT_CONTRAST, "Contrast", FF_TYPE_STANDARD );
	SetParamInfof( PT_VIGNETTE, "Vignette", FF_TYPE_STANDARD );
	SetParamInfof( PT_ABERRATION, "Aberration", FF_TYPE_STANDARD );

	SetParamInfof( PT_SHOW_FOCUS, "Show Focus", FF_TYPE_BOOLEAN );
	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	// Factory presets. Element 0 is Custom; picking anything else copies that
	// preset's values into the covered parameters and raises value events so
	// the host re-reads the sliders. Editing a covered slider flips back to
	// Custom.
	SetOptionParamInfo( PT_PRESET, "Preset", 1 + presets::kCount, params[ PT_PRESET ] );
	SetParamElementInfo( PT_PRESET, 0, "Custom", 0.0f );
	for( int i = 0; i < presets::kCount; ++i )
		SetParamElementInfo( PT_PRESET, 1 + i, presets::kPresets[ i ].name, static_cast< float >( 1 + i ) );

	// The About block. Inline rather than through a helper: SetParamInfo is
	// protected on CFFGLPlugin, so nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}

	for( FFUInt32 i = PT_GEOMETRY; i <= PT_INVERT; ++i )
		SetParamGroup( i, "Focus" );
	for( FFUInt32 i = PT_BLUR_MODEL; i <= PT_HIGHLIGHT; ++i )
		SetParamGroup( i, "Lens" );
	for( FFUInt32 i = PT_SATURATION; i <= PT_ABERRATION; ++i )
		SetParamGroup( i, "Photograph" );
	for( FFUInt32 i = PT_SHOW_FOCUS; i <= PT_MIX; ++i )
		SetParamGroup( i, "Output" );

	SetParamGroup( PT_PRESET, "Preset" );

	FFGLLog::LogToHost( "Created Tilter effect" );

	diag::init();
}

controls::HostValues Tilter::hostValues() const
{
	controls::HostValues out;

	out.geometry      = params[ PT_GEOMETRY ];
	out.focusX        = params[ PT_FOCUS_X ];
	out.focusY        = params[ PT_FOCUS_Y ];
	out.angle         = params[ PT_ANGLE ];
	out.width         = params[ PT_WIDTH ];
	out.feather       = params[ PT_FEATHER ];
	out.ellipseAspect = params[ PT_ELLIPSE_ASPECT ];
	out.horizon       = params[ PT_HORIZON ];
	out.tilt          = params[ PT_TILT ];
	out.rate          = params[ PT_RATE ];
	out.depthBias     = params[ PT_DEPTH_BIAS ];
	out.depthContrast = params[ PT_DEPTH_CONTRAST ];
	out.invert        = params[ PT_INVERT ];

	out.blurModel     = params[ PT_BLUR_MODEL ];
	out.blur          = params[ PT_BLUR ];
	out.quality       = params[ PT_QUALITY ];
	out.blades        = params[ PT_BLADES ];
	out.bladeRotation = params[ PT_BLADE_ROTATION ];
	out.highlight     = params[ PT_HIGHLIGHT ];

	out.saturation    = params[ PT_SATURATION ];
	out.contrast      = params[ PT_CONTRAST ];
	out.vignette      = params[ PT_VIGNETTE ];
	out.aberration    = params[ PT_ABERRATION ];

	out.showFocus     = params[ PT_SHOW_FOCUS ];
	out.mix           = params[ PT_MIX ];

	return out;
}

bool Tilter::compileShaders()
{
	struct Stage
	{
		FFGLShader* shader;
		const char* fragment;
		const char* name;
	};

	const Stage stages[] = {
		{ &downsampleShader, shaders::kDownsampleFragment, "downsample" },
		{ &depthShader, shaders::kDepthFragment, "depth" },
		{ &smoothShader, shaders::kSmoothFragment, "smooth" },
		{ &cocShader, shaders::kCoCFragment, "coc" },
		{ &gaussianShader, shaders::kGaussianFragment, "gaussian" },
		{ &bokehShader, shaders::kBokehFragment, "bokeh" },
		{ &compositeShader, shaders::kCompositeFragment, "composite" },
	};

	for( const Stage& stage : stages )
	{
		if( !stage.shader->Compile( shaders::kVertex, stage.fragment ) )
		{
			//Returning FF_FAIL from InitGL is invisible to the operator: the
			//effect simply does nothing in Resolume, with no message anywhere.
			//This line is the only record of which stage it was.
			diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the effect will do nothing" );
			FFGLLog::LogToHost( "Tilter: shader failed to compile" );
			return false;
		}
	}

	return true;
}

FFResult Tilter::InitGL( const FFGLViewportStruct* vp )
{
	//The GL strings first, and unconditionally. When a shader will not compile
	//it is almost always the driver or the GL version, and knowing which machine
	//reported what is the whole diagnosis.
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	if( !compileShaders() )
	{
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		FFGLLog::LogToHost( "Tilter: quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	diag::info( "initialised" );

	//Use the base class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

FFResult Tilter::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& input = *pGL->inputTextures[ 0 ];

	//The host's viewport, not the one InitGL was handed: Resolume changes
	//composition resolution without reinitialising the plugin.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );
	const int frameW = std::max( 1, hostViewport[ 2 ] );
	const int frameH = std::max( 1, hostViewport[ 3 ] );

	const float frameWf     = static_cast< float >( frameW );
	const float frameHf     = static_cast< float >( frameH );
	const float aspectRatio = frameWf / frameHf;

	const controls::HostValues host = hostValues();
	const focus::Field field        = controls::field( host, aspectRatio );
	const controls::Lens lens       = controls::lens( host, frameHf );

	const bool needsDepth = field.geometry == focus::kImageDepth;

	//Quarter resolution for the depth cues. See Depth.cpp for why that is not
	//only about being cheap.
	const int depthW = std::max( 1, frameW / 4 );
	const int depthH = std::max( 1, frameH / 4 );

	//------------------------------------------------------------------
	// Every allocation FIRST, before anything is bound.
	//
	// FFGLFBO::Initialise sizes its colour texture inside a scoped texture
	// binding, and those CLEAR to 0 on scope exit rather than restoring -- so
	// an Ensure() called after a texture was bound silently unbinds it, and the
	// frame that allocated renders black. PassBuffer::Ensure saves and restores
	// around it as well, but the ordering here is the real defence: do not move
	// these below the passes.
	//------------------------------------------------------------------
	//The blur works on a box-downsampled copy. See Downsample.cpp: it is what
	//stops a sparse tap sum aliasing the picture's own fine detail, and it is
	//also what makes a very large radius affordable.
	const int blurW = std::max( 1, frameW / lens.blurScale );
	const int blurH = std::max( 1, frameH / lens.blurScale );

	if( !cocBuffer.Ensure( frameW, frameH, GL_RGBA16F )
	    || !sourceBuffer.Ensure( blurW, blurH, GL_RGBA16F )
	    || !blurBuffer[ 0 ].Ensure( blurW, blurH, GL_RGBA16F )
	    || !blurBuffer[ 1 ].Ensure( blurW, blurH, GL_RGBA16F ) )
	{
		diag::error( "could not allocate the pass buffers" );
		return FF_FAIL;
	}

	if( needsDepth )
	{
		if( !depthBuffer[ 0 ].Ensure( depthW, depthH, GL_RGBA16F )
		    || !depthBuffer[ 1 ].Ensure( depthW, depthH, GL_RGBA16F ) )
		{
			diag::error( "could not allocate the depth buffers" );
			return FF_FAIL;
		}
	}

	const FFGLTexCoords maxCoords = GetMaxGLTexCoords( input );
	const float inputHalfTexelU   = 0.5f / std::max( 1.0f, static_cast< float >( input.Width ) );
	const float inputHalfTexelV   = 0.5f / std::max( 1.0f, static_cast< float >( input.Height ) );

	//Our own buffers have no padding, so their MaxUV is 1 and their half texel
	//is off their own size -- which for the blur chain is the DOWNSAMPLED size,
	//not the composition's.
	const float blurWf          = static_cast< float >( blurW );
	const float blurHf          = static_cast< float >( blurH );
	const float blurHalfTexelU  = 0.5f / blurWf;
	const float blurHalfTexelV  = 0.5f / blurHf;

	//The radius the blur passes work in. They run on the downsampled copy, so
	//everything they measure is in its pixels.
	const float blurRadius = lens.maxRadius / static_cast< float >( lens.blurScale );

	//Every pass does its geometry in picture space and applies MaxUV at the
	//fetch, so the vertex shader's scaling is always off.
	const float kNoScale = 1.0f;

	//------------------------------------------------------------------
	// 1. Depth cues, then smooth them. Image Depth geometry only.
	//------------------------------------------------------------------
	if( needsDepth )
	{
		{
			ScopedFBOBinding fbo( depthBuffer[ 0 ].GetGLID(), ScopedFBOBinding::RB_REVERT );
			depthBuffer[ 0 ].ResizeViewPort();
			ScopedShaderBinding shader( depthShader.GetGLID() );
			ScopedSamplerActivation sampler( 0 );
			Scoped2DTextureBinding texture( input.Handle );

			depthShader.Set( "MaxUV", kNoScale, kNoScale );
			depthShader.Set( "InputTexture", 0 );
			depthShader.Set( "SourceMaxUV", maxCoords.s, maxCoords.t );
			depthShader.Set( "SourceHalfTexel", inputHalfTexelU, inputHalfTexelV );
			//The ring sits a texel and a half out at the reduced resolution,
			//expressed in picture space because that is what fetch() takes.
			depthShader.Set( "RingStep", 1.5f / static_cast< float >( depthW ),
			                 1.5f / static_cast< float >( depthH ) );
			quad.Draw();
		}

		const struct
		{
			int from;
			int to;
			float dx;
			float dy;
		} smooths[] = {
			{ 0, 1, 1.0f / static_cast< float >( depthW ), 0.0f },
			{ 1, 0, 0.0f, 1.0f / static_cast< float >( depthH ) },
		};

		for( const auto& pass : smooths )
		{
			ScopedFBOBinding fbo( depthBuffer[ pass.to ].GetGLID(), ScopedFBOBinding::RB_REVERT );
			depthBuffer[ pass.to ].ResizeViewPort();
			ScopedShaderBinding shader( smoothShader.GetGLID() );
			ScopedSamplerActivation sampler( 0 );
			Scoped2DTextureBinding texture( depthBuffer[ pass.from ].GetTextureInfo().Handle );

			smoothShader.Set( "MaxUV", kNoScale, kNoScale );
			smoothShader.Set( "SourceTexture", 0 );
			smoothShader.Set( "Direction", pass.dx, pass.dy );
			quad.Draw();
		}
	}

	//------------------------------------------------------------------
	// 2. The picture the blur will work on, box filtered down.
	//------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( sourceBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		sourceBuffer.ResizeViewPort();
		ScopedShaderBinding shader( downsampleShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( input.Handle );

		downsampleShader.Set( "MaxUV", kNoScale, kNoScale );
		downsampleShader.Set( "InputTexture", 0 );
		downsampleShader.Set( "SourceMaxUV", maxCoords.s, maxCoords.t );
		downsampleShader.Set( "SourceHalfTexel", inputHalfTexelU, inputHalfTexelV );
		//One COMPOSITION texel in picture space -- the block is measured in the
		//full-resolution grid, not in the downsampled one.
		downsampleShader.Set( "SourceTexel", 1.0f / frameWf, 1.0f / frameHf );
		downsampleShader.Set( "Scale", lens.blurScale );
		quad.Draw();
	}

	//------------------------------------------------------------------
	// 3. The circle of confusion field.
	//------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( cocBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		cocBuffer.ResizeViewPort();
		ScopedShaderBinding shader( cocShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		//Bound even when the geometry does not read it: a sampler left pointing
		//at a deleted texture is undefined behaviour, not a harmless no-op.
		Scoped2DTextureBinding texture( needsDepth ? depthBuffer[ 0 ].GetTextureInfo().Handle
		                                           : blurBuffer[ 0 ].GetTextureInfo().Handle );

		cocShader.Set( "MaxUV", kNoScale, kNoScale );
		cocShader.Set( "DepthTexture", 0 );
		cocShader.Set( "Geometry", field.geometry );
		cocShader.Set( "Centre", field.centreU, field.centreV );
		cocShader.Set( "Angle", field.angle );
		cocShader.Set( "Width", field.width );
		cocShader.Set( "Feather", field.feather );
		cocShader.Set( "EllipseAspect", field.aspect );
		cocShader.Set( "Horizon", field.horizon );
		cocShader.Set( "Tilt", field.tilt );
		cocShader.Set( "Rate", field.rate );
		cocShader.Set( "DepthBias", field.depthBias );
		cocShader.Set( "DepthContrast", field.depthContrast );
		cocShader.Set( "Invert", field.invert ? 1.0f : 0.0f );
		cocShader.Set( "FrameAspect", field.aspectRatio );
		quad.Draw();
	}

	//------------------------------------------------------------------
	// 4. The blur. One model or the other, never both. Both run entirely in
	//    downsampled space, reading the box-filtered copy.
	//------------------------------------------------------------------
	GLuint blurredTexture = 0;

	if( lens.model == controls::kBokeh )
	{
		ScopedFBOBinding fbo( blurBuffer[ 0 ].GetGLID(), ScopedFBOBinding::RB_REVERT );
		blurBuffer[ 0 ].ResizeViewPort();
		ScopedShaderBinding shader( bokehShader.GetGLID() );

		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, sourceBuffer.GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, cocBuffer.GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE0 );

		bokehShader.Set( "MaxUV", kNoScale, kNoScale );
		bokehShader.Set( "SourceTexture", 0 );
		bokehShader.Set( "CoCTexture", 1 );
		bokehShader.Set( "SourceMaxUV", kNoScale, kNoScale );
		bokehShader.Set( "SourceHalfTexel", blurHalfTexelU, blurHalfTexelV );
		bokehShader.Set( "FrameSize", blurWf, blurHf );
		bokehShader.Set( "MaxRadius", blurRadius );
		bokehShader.Set( "Samples", lens.bokehSamples );
		bokehShader.Set( "Blades", lens.blades );
		bokehShader.Set( "BladeRotation", lens.bladeRotation );
		bokehShader.Set( "Highlight", lens.highlight );
		quad.Draw();

		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, 0 );

		blurredTexture = blurBuffer[ 0 ].GetTextureInfo().Handle;
	}
	else
	{
		//Both passes read one of our own downsampled buffers, so unlike the
		//pre-downsample version they share a MaxUV and a half texel.
		const struct
		{
			int to;
			float dx;
			float dy;
			bool fromSource;
		} passes[] = {
			{ 0, 1.0f, 0.0f, true },
			{ 1, 0.0f, 1.0f, false },
		};

		for( const auto& pass : passes )
		{
			ScopedFBOBinding fbo( blurBuffer[ pass.to ].GetGLID(), ScopedFBOBinding::RB_REVERT );
			blurBuffer[ pass.to ].ResizeViewPort();
			ScopedShaderBinding shader( gaussianShader.GetGLID() );

			glActiveTexture( GL_TEXTURE0 );
			glBindTexture( GL_TEXTURE_2D, pass.fromSource ? sourceBuffer.GetTextureInfo().Handle
			                                              : blurBuffer[ 0 ].GetTextureInfo().Handle );
			glActiveTexture( GL_TEXTURE1 );
			glBindTexture( GL_TEXTURE_2D, cocBuffer.GetTextureInfo().Handle );
			glActiveTexture( GL_TEXTURE0 );

			gaussianShader.Set( "MaxUV", kNoScale, kNoScale );
			gaussianShader.Set( "SourceTexture", 0 );
			gaussianShader.Set( "CoCTexture", 1 );
			gaussianShader.Set( "SourceMaxUV", kNoScale, kNoScale );
			gaussianShader.Set( "SourceHalfTexel", blurHalfTexelU, blurHalfTexelV );
			gaussianShader.Set( "Direction", pass.dx, pass.dy );
			gaussianShader.Set( "FrameSize", blurWf, blurHf );
			gaussianShader.Set( "MaxRadius", blurRadius );
			gaussianShader.Set( "Taps", lens.gaussianTaps );
			quad.Draw();

			glActiveTexture( GL_TEXTURE1 );
			glBindTexture( GL_TEXTURE_2D, 0 );
			glActiveTexture( GL_TEXTURE0 );
			glBindTexture( GL_TEXTURE_2D, 0 );
		}

		blurredTexture = blurBuffer[ 1 ].GetTextureInfo().Handle;
	}

	//------------------------------------------------------------------
	// 5. The photograph, straight into whatever the host handed us.
	//------------------------------------------------------------------
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( compositeShader.GetGLID() );

		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, blurredTexture );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, input.Handle );
		glActiveTexture( GL_TEXTURE2 );
		glBindTexture( GL_TEXTURE_2D, cocBuffer.GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE0 );

		compositeShader.Set( "MaxUV", kNoScale, kNoScale );
		compositeShader.Set( "BlurredTexture", 0 );
		compositeShader.Set( "InputTexture", 1 );
		compositeShader.Set( "CoCTexture", 2 );
		compositeShader.Set( "SourceMaxUV", maxCoords.s, maxCoords.t );
		compositeShader.Set( "SourceHalfTexel", inputHalfTexelU, inputHalfTexelV );
		compositeShader.Set( "Saturation", lens.saturation );
		compositeShader.Set( "Contrast", lens.contrast );
		compositeShader.Set( "Vignette", lens.vignette );
		compositeShader.Set( "Aberration", lens.aberration );
		compositeShader.Set( "Mix", lens.mix );
		compositeShader.Set( "ShowFocus", lens.showFocus ? 1.0f : 0.0f );
		compositeShader.Set( "FrameAspect", aspectRatio );
		//Composition pixels, not blur-space: the composite decides where the
		//blur is too small to be worth taking, and that judgement is about the
		//picture the operator sees.
		compositeShader.Set( "MaxRadius", lens.maxRadius );
		quad.Draw();

		glActiveTexture( GL_TEXTURE2 );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	return FF_SUCCESS;
}

void Tilter::releaseBuffers()
{
	for( auto& buffer : depthBuffer )
		buffer.Destroy();
	cocBuffer.Destroy();
	sourceBuffer.Destroy();
	for( auto& buffer : blurBuffer )
		buffer.Destroy();
}

FFResult Tilter::DeInitGL()
{
	downsampleShader.FreeGLResources();
	depthShader.FreeGLResources();
	smoothShader.FreeGLResources();
	cocShader.FreeGLResources();
	gaussianShader.FreeGLResources();
	bokehShader.FreeGLResources();
	compositeShader.FreeGLResources();
	quad.Release();
	releaseBuffers();

	return FF_SUCCESS;
}

void Tilter::seedHostSaid()
{
	// Seeded on first parameter traffic rather than in the constructor, so the
	// whole mechanism stays in one place. It has to happen BEFORE applyPreset
	// can run: seeding afterwards would record the preset's own values as the
	// host's opening position, and the host's very next restatement would then
	// look like an edit -- which is the bug this exists to fix, reintroduced.
	if( hostSaidSeeded )
		return;

	for( unsigned int i = 0; i < PT_COUNT; ++i )
		hostSaid[ i ] = params[ i ];

	hostSaidSeeded = true;
}

float Tilter::presetValue( int presetIndex, unsigned int id ) const
{
	if( presetIndex < 1 || presetIndex > presets::kCount )
		return -1.0f;

	const presets::Preset& preset = presets::kPresets[ presetIndex - 1 ];

	for( int i = 0; i < presets::kParamCount; ++i )
		if( kPresetParamIDs[ i ] == id )
			return preset.v[ i ];

	return -1.0f;
}

bool Tilter::hostIsRestatingItself( unsigned int index, float value )
{
	const float lastFromHost = hostSaid[ index ];
	hostSaid[ index ]        = value;

	const float fromPreset =
		presetValue( static_cast< int >( std::lround( params[ PT_PRESET ] ) ), index );
	if( fromPreset < 0.0f )
		return false;

	// A quantisation allowance rather than a float epsilon. A host that keeps
	// its parameters shorter than a float -- or round-trips them through a UI,
	// a MIDI value or a saved composition -- hands back a number NEAR ours
	// rather than ours, and 1e-4 read that as an edit.
	constexpr float kSame = 1e-3f;

	if( std::fabs( value - fromPreset ) <= kSame )
	{
		// The host agreeing with the preset. Nothing to write -- and writing it
		// would actively hurt: a host that quantises hands back a ROUNDED copy
		// of our own value, params[] would take the rounding, and the "did a
		// covered parameter move?" test below works to a tighter tolerance than
		// this one and would read that rounding as an edit.
		return true;
	}

	if( std::fabs( value - lastFromHost ) > kSame )
		return false;//neither: the operator has taken over

	// Deliberately not logged. A host that pushes its parameters every frame
	// would put a line here every frame, and a log that scrolls is a log nobody
	// reads. The event worth recording is the fallback to Custom, which
	// happens once.
	return true;
}

const unsigned int* Tilter::PresetParamIDsForTest( int& count )
{
	count = presets::kParamCount;
	return kPresetParamIDs;
}

FFResult Tilter::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// An About button is a press, not a value to keep: it opens a browser and
	// nothing about the effect changes. Handled before any of the bookkeeping
	// below, because pressing one is not the operator editing a control.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	seedHostSaid();

	if( index == PT_PRESET )
	{
		const int chosen = static_cast< int >( std::lround( value ) );
		if( chosen != static_cast< int >( std::lround( params[ PT_PRESET ] ) ) )
			applyPreset( chosen );
		return FF_SUCCESS;
	}

	// The host may be restating a value it still believes in rather than the
	// operator moving anything. Letting that through would overwrite the
	// preset's value in params[] AND read as an edit, dropping the dropdown
	// straight back to Custom -- which is what made presets look like they
	// could not be selected at all.
	if( hostIsRestatingItself( index, value ) )
		return FF_SUCCESS;

	// A slider moved while a preset is active means the operator has taken
	// over: the dropdown falls back to Custom. The tolerance here is
	// deliberately tighter than the quantisation allowance above: a restatement
	// never reaches this point, so anything that does is a real move.
	const float previous = params[ index ];
	params[ index ]      = value;

	const int active = static_cast< int >( std::lround( params[ PT_PRESET ] ) );
	if( active > 0 && std::fabs( value - previous ) > 1e-4f )
	{
		for( unsigned int id : kPresetParamIDs )
		{
			if( id == index )
			{
				params[ PT_PRESET ] = 0.0f;
				RaiseParamEvent( PT_PRESET, FF_EVENT_FLAG_VALUE );
				break;
			}
		}
	}

	return FF_SUCCESS;
}

void Tilter::applyPreset( int presetIndex )
{
	params[ PT_PRESET ] = static_cast< float >( presetIndex );

	if( presetIndex <= 0 || presetIndex > presets::kCount )
		return;//Custom: the sliders keep whatever they said

	const presets::Preset& preset = presets::kPresets[ presetIndex - 1 ];
	for( int j = 0; j < presets::kParamCount; ++j )
	{
		const unsigned int id = kPresetParamIDs[ j ];
		if( std::fabs( params[ id ] - preset.v[ j ] ) <= 1e-6f )
			continue;

		// The copy is what changes the picture; the event only tells the host
		// to re-read the slider. A host that ignores it renders the preset
		// correctly and merely shows stale knobs.
		//
		// ☠️ `hostSaid[ id ]` is deliberately NOT written here. It records what
		// the HOST last said, and the host has not said anything yet -- it
		// still believes the values from before the preset was chosen.
		// Recording the preset's own values as the host's opening position
		// makes the host's very next restatement of what it believes look like
		// an operator edit, and the dropdown snaps straight back to Custom.
		// `tiltest --hosts` fails in the "ignores" column without this.
		params[ id ] = preset.v[ j ];
		RaiseParamEvent( id, FF_EVENT_FLAG_VALUE );
	}
}

float Tilter::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

char* Tilter::GetTextParameter( unsigned int index )
{
	// The host is handed a bare pointer, so the string is kept as a member
	// rather than built on the stack here.
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Tilter::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class returns FF_FAIL, and instantiateGL
	// deletes the whole instance when setting any default fails. The About text
	// is display-only, so there is genuinely nothing to store -- but it has to
	// say so successfully.
	(void)value;

	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, value );
}
