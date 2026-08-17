/// The OpenFX build of Tilter, for DaVinci Resolve, Nuke, Natron, Vegas and
/// other OFX hosts.
///
/// Same lens as the FFGL build. The focus field lives once, in Focus.cpp, and
/// every parameter curve lives once, in Controls.cpp — this file LINKS both
/// rather than copying them, which is what stops a preset meaning one thing in
/// Resolume and another in Resolve.
///
/// What IS mirrored here is the per-pixel machinery the GPU did per fragment:
/// the box downsample, the two blur gathers, and the composite. When editing
/// Downsample.cpp, Gaussian.cpp, Bokeh.cpp or Composite.cpp, edit this too.
///
/// ------------------------------------------------------------ the structure
///
/// The GPU renders this as a chain of full-frame passes and so does this file,
/// because the alternative — computing each output pixel from scratch — would
/// re-derive the blur of its neighbours for every one of them. `setup()` builds
/// the downsampled picture, its circle-of-confusion field and the blurred
/// result once; `multiThreadProcessImages` then does the per-pixel composite,
/// which is the part worth threading.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsProcessing.h"

#include "../Controls.h"
#include "../Focus.h"
#include "../Presets.h"

namespace
{
constexpr const char* kPluginIdentifier = "com.stoatworks.tilter";
constexpr const char* kPluginName       = "Tilter";
constexpr const char* kPluginGrouping   = "Stoatworks";
constexpr const char* kPluginDescription =
	"Tilt-shift lens with a real aperture.\n\n"
	"Puts a shallow plane of focus across the picture and a real aperture "
	"behind it. Four ways to choose what is sharp — a linear band, an ellipse, "
	"a tilted plane with a horizon, or a guess at depth from the picture "
	"itself — and two blurs: a bokeh gather with 5 to 9 aperture blades that "
	"makes highlights bloom into aperture shapes, or a cheaper Gaussian.\n\n"
	"https://stoatworks-labs.com";

constexpr const char* kParamPreset        = "preset";
constexpr const char* kParamGeometry      = "focusShape";
constexpr const char* kParamFocusX        = "focusX";
constexpr const char* kParamFocusY        = "focusY";
constexpr const char* kParamAngle         = "angle";
constexpr const char* kParamWidth         = "focusWidth";
constexpr const char* kParamFeather       = "feather";
constexpr const char* kParamEllipseAspect = "irisAspect";
constexpr const char* kParamHorizon       = "horizon";
constexpr const char* kParamTilt          = "tilt";
constexpr const char* kParamRate          = "falloffRate";
constexpr const char* kParamDepthBias     = "depthFocus";
constexpr const char* kParamDepthContrast = "depthContrast";
constexpr const char* kParamInvert        = "invertFocus";
constexpr const char* kParamBlurModel     = "blur";
constexpr const char* kParamBlurAmount    = "blurAmount";
constexpr const char* kParamQuality       = "quality";
constexpr const char* kParamBlades        = "aperture";
constexpr const char* kParamBladeRotation = "apertureAngle";
constexpr const char* kParamHighlight     = "highlights";
constexpr const char* kParamSaturation    = "saturation";
constexpr const char* kParamContrast      = "contrast";
constexpr const char* kParamVignette      = "vignette";
constexpr const char* kParamAberration    = "aberration";
constexpr const char* kParamShowFocus     = "showFocus";
constexpr const char* kParamMix           = "mix";

/// The preset table is host-agnostic; this is the OFX binding of it, in
/// presets::Param order. Same job as the FFGL build's kPresetParamIDs.
const char* const kPresetParamNames[ tilter::presets::kParamCount ] = {
	kParamGeometry, kParamWidth, kParamFeather, kParamEllipseAspect, kParamHorizon,
	kParamTilt, kParamRate, kParamDepthBias, kParamDepthContrast, kParamInvert,
	kParamBlurModel, kParamBlurAmount, kParamBlades, kParamBladeRotation,
	kParamHighlight, kParamSaturation, kParamContrast, kParamVignette, kParamAberration
};

struct Rgba
{
	float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
};

float clamp01f( float x )
{
	return x < 0.0f ? 0.0f : ( x > 1.0f ? 1.0f : x );
}

/// GLSL smoothstep.
float smoothstepf( float edge0, float edge1, float x )
{
	const float t = clamp01f( ( x - edge0 ) / ( edge1 - edge0 ) );
	return t * t * ( 3.0f - 2.0f * t );
}

/// GLSL mod(): x - y*floor(x/y), correct for negatives, which std::fmod is not.
float glslMod( float x, float y )
{
	return x - y * std::floor( x / y );
}

//= mirrored: the mirror1()/mirrorUV() in Gaussian.cpp and Bokeh.cpp
float mirrorCoord( float x )
{
	const float m = glslMod( std::fabs( x ), 2.0f );
	return m > 1.0f ? 2.0f - m : m;
}

float lumaOf( float r, float g, float b )
{
	return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

/// Everything one render needs, in the physical units Focus.h and Controls.h
/// work in. Filled from the same conversion functions the FFGL build uses.
struct Settings
{
	tilter::focus::Field field;
	tilter::controls::Lens lens;
	float aspectRatio = 16.0f / 9.0f;
};

/**
    A small floating-point image, premultiplied RGBA.

    The chain needs three of these — the downsampled picture, its blurred
    result, and a scratch buffer for the Gaussian's second pass — and they are
    all at blur resolution, which is why holding them is affordable.
*/
struct Plane
{
	int width  = 0;
	int height = 0;
	std::vector< Rgba > pixels;

	void allocate( int w, int h )
	{
		width  = std::max( 1, w );
		height = std::max( 1, h );
		pixels.assign( static_cast< size_t >( width ) * height, Rgba() );
	}

	Rgba& at( int x, int y )
	{
		return pixels[ static_cast< size_t >( std::clamp( y, 0, height - 1 ) ) * width
		               + std::clamp( x, 0, width - 1 ) ];
	}

	const Rgba& at( int x, int y ) const
	{
		return pixels[ static_cast< size_t >( std::clamp( y, 0, height - 1 ) ) * width
		               + std::clamp( x, 0, width - 1 ) ];
	}

	/// Bilinear fetch in picture space, mirrored at the edges -- the same rule
	/// the blur shaders use, for the same reason (see Gaussian.cpp).
	Rgba sampleMirrored( float u, float v ) const
	{
		u = mirrorCoord( u );
		v = mirrorCoord( v );

		const float fx = u * static_cast< float >( width ) - 0.5f;
		const float fy = v * static_cast< float >( height ) - 0.5f;
		const int x0   = static_cast< int >( std::floor( fx ) );
		const int y0   = static_cast< int >( std::floor( fy ) );
		const float tx = fx - static_cast< float >( x0 );
		const float ty = fy - static_cast< float >( y0 );

		const Rgba& a = at( x0, y0 );
		const Rgba& b = at( x0 + 1, y0 );
		const Rgba& c = at( x0, y0 + 1 );
		const Rgba& d = at( x0 + 1, y0 + 1 );

		Rgba out;
		out.r = ( a.r * ( 1 - tx ) + b.r * tx ) * ( 1 - ty ) + ( c.r * ( 1 - tx ) + d.r * tx ) * ty;
		out.g = ( a.g * ( 1 - tx ) + b.g * tx ) * ( 1 - ty ) + ( c.g * ( 1 - tx ) + d.g * tx ) * ty;
		out.b = ( a.b * ( 1 - tx ) + b.b * tx ) * ( 1 - ty ) + ( c.b * ( 1 - tx ) + d.b * tx ) * ty;
		out.a = ( a.a * ( 1 - tx ) + b.a * tx ) * ( 1 - ty ) + ( c.a * ( 1 - tx ) + d.a * tx ) * ty;
		return out;
	}
};

class TilterProcessorBase : public OFX::ImageProcessor
{
public:
	explicit TilterProcessorBase( OFX::ImageEffect& effect ) :
		OFX::ImageProcessor( effect )
	{
	}

protected:
	OFX::Image* srcImg = nullptr;
	Settings settings;
	bool premultiplied = false;
	int srcW           = 0;
	int srcH           = 0;

	/// Blur-resolution planes. See the note at the top of the file.
	Plane blurred;
	std::vector< float > blurCoC;///< signed defocus at blur resolution

	/// Blur-space radius, in blur-resolution pixels.
	float blurRadius = 0.0f;
};

template< class PIX, int nComponents, int maxValue >
class TilterProcessor : public TilterProcessorBase
{
public:
	explicit TilterProcessor( OFX::ImageEffect& effect ) :
		TilterProcessorBase( effect )
	{
	}

	void setup( OFX::Image* src, const Settings& s, bool premultipliedValue )
	{
		srcImg        = src;
		settings      = s;
		premultiplied = premultipliedValue;

		const OfxRectI bounds = src->getBounds();
		srcW                  = bounds.x2 - bounds.x1;
		srcH                  = bounds.y2 - bounds.y1;

		const int scale = std::max( 1, settings.lens.blurScale );
		const int bw    = std::max( 1, srcW / scale );
		const int bh    = std::max( 1, srcH / scale );

		blurRadius = settings.lens.maxRadius / static_cast< float >( scale );

		//--------------------------------------------------------------
		// 1. The depth cues, for the Image Depth geometry only.
		//--------------------------------------------------------------
		const bool needsDepth = settings.field.geometry == tilter::focus::kImageDepth;
		if( needsDepth )
			buildDepth();

		//--------------------------------------------------------------
		// 2. Box downsample. Mirrors Downsample.cpp -- a real average of
		//    every texel of the block, which is what stops the gather that
		//    follows aliasing its own source.
		//--------------------------------------------------------------
		Plane source;
		source.allocate( bw, bh );
		for( int y = 0; y < bh; ++y )
		{
			for( int x = 0; x < bw; ++x )
			{
				double r = 0, g = 0, b = 0, a = 0;
				int taken = 0;
				for( int j = 0; j < scale; ++j )
				{
					for( int i = 0; i < scale; ++i )
					{
						const Rgba p = readSource( x * scale + i, y * scale + j );
						r += p.r;
						g += p.g;
						b += p.b;
						a += p.a;
						++taken;
					}
				}
				Rgba& out = source.at( x, y );
				out.r     = static_cast< float >( r / taken );
				out.g     = static_cast< float >( g / taken );
				out.b     = static_cast< float >( b / taken );
				out.a     = static_cast< float >( a / taken );
			}
		}

		//--------------------------------------------------------------
		// 3. The circle of confusion at blur resolution, for the taps.
		//    The centre pixel's own defocus is computed exactly, at full
		//    resolution, in the composite below.
		//--------------------------------------------------------------
		blurCoC.assign( static_cast< size_t >( bw ) * bh, 0.0f );
		for( int y = 0; y < bh; ++y )
		{
			for( int x = 0; x < bw; ++x )
			{
				const float u     = ( static_cast< float >( x ) + 0.5f ) / static_cast< float >( bw );
				const float vDown = 1.0f - ( static_cast< float >( y ) + 0.5f ) / static_cast< float >( bh );
				float luma = 0.0f, detail = 0.0f;
				if( needsDepth )
					sampleDepth( u, 1.0f - vDown, luma, detail );
				blurCoC[ static_cast< size_t >( y ) * bw + x ] =
					tilter::focus::defocus( settings.field, u, vDown, luma, detail );
			}
		}

		//--------------------------------------------------------------
		// 4. The blur itself.
		//--------------------------------------------------------------
		if( settings.lens.model == tilter::controls::kBokeh )
			gatherBokeh( source, bw, bh );
		else
			gatherGaussian( source, bw, bh );
	}

	void multiThreadProcessImages( OfxRectI window ) override
	{
		const OfxRectI dstBounds = _dstImg->getBounds();
		const int dstW           = dstBounds.x2 - dstBounds.x1;
		const int dstH           = dstBounds.y2 - dstBounds.y1;

		const tilter::controls::Lens& lens = settings.lens;
		const bool needsDepth = settings.field.geometry == tilter::focus::kImageDepth;

		for( int y = window.y1; y < window.y2; ++y )
		{
			if( _effect.abort() )
				break;

			PIX* dstPix = static_cast< PIX* >( _dstImg->getPixelAddress( window.x1, y ) );

			for( int x = window.x1; x < window.x2; ++x, dstPix += nComponents )
			{
				const float u = ( static_cast< float >( x - dstBounds.x1 ) + 0.5f ) / static_cast< float >( dstW );
				const float vUp = ( static_cast< float >( y - dstBounds.y1 ) + 0.5f ) / static_cast< float >( dstH );
				//Focus.cpp works with v running DOWN; OFX images, like GL, run up.
				const float vDown = 1.0f - vUp;

				float luma = 0.0f, detail = 0.0f;
				if( needsDepth )
					sampleDepth( u, vUp, luma, detail );

				const float signedDefocus =
					tilter::focus::defocus( settings.field, u, vDown, luma, detail );
				const float defocus = std::fabs( signedDefocus );

				//= mirrored: Composite.cpp
				const float fromCentreX = ( u - 0.5f ) * settings.aspectRatio;
				const float fromCentreY = vUp - 0.5f;
				const float radial = std::sqrt( fromCentreX * fromCentreX + fromCentreY * fromCentreY );

				Rgba colour;
				if( lens.aberration > 0.0001f && defocus > 0.0001f )
				{
					const float dirX = radial > 0.0001f ? fromCentreX / radial : 0.0f;
					const float dirY = radial > 0.0001f ? fromCentreY / radial : 0.0f;
					const float shift = lens.aberration * signedDefocus * 0.01f;

					const Rgba mid  = blurred.sampleMirrored( u, vUp );
					const Rgba high = blurred.sampleMirrored( u + dirX * shift, vUp + dirY * shift );
					const Rgba low  = blurred.sampleMirrored( u - dirX * shift, vUp - dirY * shift );

					colour.r = high.r;
					colour.g = mid.g;
					colour.b = low.b;
					colour.a = mid.a;
				}
				else
				{
					colour = blurred.sampleMirrored( u, vUp );
				}

				//The blurred plane is lower resolution, so the sharp picture
				//has to come back where the lens is sharp. Same threshold as
				//the shader: under about a pixel of defocus there is nothing a
				//blur could do.
				const Rgba sharp = readSource( x - dstBounds.x1, y - dstBounds.y1 );
				const float radiusPixels = defocus * lens.maxRadius;
				const float blurWeight   = smoothstepf( 0.5f, 2.0f, radiusPixels );
				colour.r = sharp.r + ( colour.r - sharp.r ) * blurWeight;
				colour.g = sharp.g + ( colour.g - sharp.g ) * blurWeight;
				colour.b = sharp.b + ( colour.b - sharp.b ) * blurWeight;
				colour.a = sharp.a + ( colour.a - sharp.a ) * blurWeight;

				//Grade in straight alpha: contrast on a premultiplied pixel
				//pushes colour and coverage together and hardens every soft
				//edge.
				const float alpha = colour.a;
				float sr = alpha > 0.001f ? colour.r / alpha : colour.r;
				float sg = alpha > 0.001f ? colour.g / alpha : colour.g;
				float sb = alpha > 0.001f ? colour.b / alpha : colour.b;

				const float grey = lumaOf( sr, sg, sb );
				sr = grey + ( sr - grey ) * lens.saturation;
				sg = grey + ( sg - grey ) * lens.saturation;
				sb = grey + ( sb - grey ) * lens.saturation;

				sr = ( sr - 0.5f ) * lens.contrast + 0.5f;
				sg = ( sg - 0.5f ) * lens.contrast + 0.5f;
				sb = ( sb - 0.5f ) * lens.contrast + 0.5f;

				if( lens.vignette > 0.0001f )
				{
					const float v = smoothstepf( 0.7f, 0.25f, radial );
					const float k = 1.0f + ( v - 1.0f ) * lens.vignette;
					sr *= k;
					sg *= k;
					sb *= k;
				}

				sr = std::max( sr, 0.0f );
				sg = std::max( sg, 0.0f );
				sb = std::max( sb, 0.0f );

				if( lens.showFocus )
				{
					//Positive is the near side -- see Focus.h. Warm for near,
					//cold for far.
					const float tintR = signedDefocus > 0.0f ? 1.0f : 0.15f;
					const float tintG = signedDefocus > 0.0f ? 0.35f : 0.5f;
					const float tintB = signedDefocus > 0.0f ? 0.15f : 1.0f;

					const float g2 = lumaOf( sr, sg, sb );
					const float mr = g2 + ( tintR - g2 ) * 0.6f;
					const float mg = g2 + ( tintG - g2 ) * 0.6f;
					const float mb = g2 + ( tintB - g2 ) * 0.6f;

					sr = sr + ( mr - sr ) * defocus;
					sg = sg + ( mg - sg ) * defocus;
					sb = sb + ( mb - sb ) * defocus;
				}

				float outR = sr * alpha;
				float outG = sg * alpha;
				float outB = sb * alpha;
				float outA = alpha;

				//Wet/dry against the untouched input, last.
				outR = sharp.r + ( outR - sharp.r ) * lens.mix;
				outG = sharp.g + ( outG - sharp.g ) * lens.mix;
				outB = sharp.b + ( outB - sharp.b ) * lens.mix;
				outA = sharp.a + ( outA - sharp.a ) * lens.mix;

				if( !premultiplied && nComponents == 4 && outA > 0.0f )
				{
					outR /= outA;
					outG /= outA;
					outB /= outA;
				}

				dstPix[ 0 ] = quantise( outR );
				dstPix[ 1 ] = quantise( outG );
				dstPix[ 2 ] = quantise( outB );
				if( nComponents == 4 )
					dstPix[ 3 ] = quantise( outA );
			}
		}
	}

private:
	//----------------------------------------------------------------------
	// The depth cues. Mirrors Depth.cpp and Smooth.cpp.
	//----------------------------------------------------------------------
	int depthW = 0;
	int depthH = 0;
	std::vector< float > depthLuma;
	std::vector< float > depthDetail;

	void buildDepth()
	{
		depthW = std::max( 1, srcW / 4 );
		depthH = std::max( 1, srcH / 4 );
		depthLuma.assign( static_cast< size_t >( depthW ) * depthH, 0.0f );
		depthDetail.assign( static_cast< size_t >( depthW ) * depthH, 0.0f );

		const float ringX = 1.5f / static_cast< float >( depthW );
		const float ringY = 1.5f / static_cast< float >( depthH );

		for( int y = 0; y < depthH; ++y )
		{
			for( int x = 0; x < depthW; ++x )
			{
				const float u = ( static_cast< float >( x ) + 0.5f ) / static_cast< float >( depthW );
				const float v = ( static_cast< float >( y ) + 0.5f ) / static_cast< float >( depthH );

				const float centre = lumaAtPicture( u, v );
				float lo = centre;
				float hi = centre;

				for( int i = 0; i < 8; ++i )
				{
					const float a = static_cast< float >( i ) * 0.7853981634f;
					const float l = lumaAtPicture( u + std::cos( a ) * ringX, v + std::sin( a ) * ringY );
					lo = std::min( lo, l );
					hi = std::max( hi, l );
				}

				depthLuma[ static_cast< size_t >( y ) * depthW + x ]   = centre;
				depthDetail[ static_cast< size_t >( y ) * depthW + x ] = clamp01f( ( hi - lo ) * 4.0f );
			}
		}

		//Separable nine-tap smoothing, both channels, both axes. Same weights
		//as Smooth.cpp: a depth field carrying per-pixel detail makes a blur
		//that shimmers.
		static const float kWeights[ 5 ] = { 0.2270270270f, 0.1945945946f, 0.1216216216f,
		                                     0.0540540541f, 0.0162162162f };
		smoothPlane( depthLuma, kWeights );
		smoothPlane( depthDetail, kWeights );
	}

	void smoothPlane( std::vector< float >& plane, const float ( &weights )[ 5 ] )
	{
		std::vector< float > scratch( plane.size(), 0.0f );

		auto get = [ & ]( const std::vector< float >& p, int x, int y ) {
			return p[ static_cast< size_t >( std::clamp( y, 0, depthH - 1 ) ) * depthW
			          + std::clamp( x, 0, depthW - 1 ) ];
		};

		for( int y = 0; y < depthH; ++y )
			for( int x = 0; x < depthW; ++x )
			{
				float sum = get( plane, x, y ) * weights[ 0 ];
				for( int i = 1; i < 5; ++i )
					sum += ( get( plane, x - i, y ) + get( plane, x + i, y ) ) * weights[ i ];
				scratch[ static_cast< size_t >( y ) * depthW + x ] = sum;
			}

		for( int y = 0; y < depthH; ++y )
			for( int x = 0; x < depthW; ++x )
			{
				float sum = get( scratch, x, y ) * weights[ 0 ];
				for( int i = 1; i < 5; ++i )
					sum += ( get( scratch, x, y - i ) + get( scratch, x, y + i ) ) * weights[ i ];
				plane[ static_cast< size_t >( y ) * depthW + x ] = sum;
			}
	}

	/// Bilinear, to match the GL_LINEAR sampler the shader reads this through.
	/// A nearest lookup here is not a small error: it put a 0.4 disagreement
	/// into the FFGL build's mirror test, two hundred times its tolerance.
	void sampleDepth( float u, float vUp, float& luma, float& detail ) const
	{
		if( depthW == 0 )
		{
			luma = detail = 0.0f;
			return;
		}

		const float fx = clamp01f( u ) * static_cast< float >( depthW ) - 0.5f;
		const float fy = clamp01f( vUp ) * static_cast< float >( depthH ) - 0.5f;
		const int x0   = static_cast< int >( std::floor( fx ) );
		const int y0   = static_cast< int >( std::floor( fy ) );
		const float tx = fx - static_cast< float >( x0 );
		const float ty = fy - static_cast< float >( y0 );

		auto tap = [ & ]( const std::vector< float >& p, int x, int y ) {
			return p[ static_cast< size_t >( std::clamp( y, 0, depthH - 1 ) ) * depthW
			          + std::clamp( x, 0, depthW - 1 ) ];
		};
		auto bilinear = [ & ]( const std::vector< float >& p ) {
			const float top = tap( p, x0, y0 ) * ( 1 - tx ) + tap( p, x0 + 1, y0 ) * tx;
			const float bot = tap( p, x0, y0 + 1 ) * ( 1 - tx ) + tap( p, x0 + 1, y0 + 1 ) * tx;
			return top * ( 1 - ty ) + bot * ty;
		};

		luma   = bilinear( depthLuma );
		detail = bilinear( depthDetail );
	}

	float lumaAtPicture( float u, float v ) const
	{
		const int x = std::clamp( static_cast< int >( u * static_cast< float >( srcW ) ), 0, srcW - 1 );
		const int y = std::clamp( static_cast< int >( v * static_cast< float >( srcH ) ), 0, srcH - 1 );
		const Rgba p = readSource( x, y );
		//Straight alpha before measuring brightness, or every semi-transparent
		//region reads as dark and therefore as near.
		if( p.a > 0.001f )
			return lumaOf( p.r / p.a, p.g / p.a, p.b / p.a );
		return lumaOf( p.r, p.g, p.b );
	}

	//----------------------------------------------------------------------
	// The blurs. Mirrors Gaussian.cpp and Bokeh.cpp.
	//----------------------------------------------------------------------
	float radiusAt( int bw, int bh, float u, float v ) const
	{
		const float mu = mirrorCoord( u );
		const float mv = mirrorCoord( v );
		const int x    = std::clamp( static_cast< int >( mu * static_cast< float >( bw ) ), 0, bw - 1 );
		const int y    = std::clamp( static_cast< int >( mv * static_cast< float >( bh ) ), 0, bh - 1 );
		return std::fabs( blurCoC[ static_cast< size_t >( y ) * bw + x ] ) * blurRadius;
	}

	void gatherGaussian( const Plane& source, int bw, int bh )
	{
		const int taps = std::max( 1, settings.lens.gaussianTaps );
		Plane intermediate = source;
		blurred            = source;

		//Two passes, horizontal then vertical, exactly as the shader.
		for( int pass = 0; pass < 2; ++pass )
		{
			const Plane& from = pass == 0 ? source : intermediate;
			Plane& to         = pass == 0 ? intermediate : blurred;
			const float dx    = pass == 0 ? 1.0f : 0.0f;
			const float dy    = pass == 0 ? 0.0f : 1.0f;

			for( int y = 0; y < bh; ++y )
			{
				for( int x = 0; x < bw; ++x )
				{
					const float u = ( static_cast< float >( x ) + 0.5f ) / static_cast< float >( bw );
					const float v = ( static_cast< float >( y ) + 0.5f ) / static_cast< float >( bh );

					float radius = radiusAt( bw, bh, u, v );
					for( int i = 1; i <= 2; ++i )
					{
						const float px = dx * ( blurRadius * static_cast< float >( i ) * 0.5f ) / static_cast< float >( bw );
						const float py = dy * ( blurRadius * static_cast< float >( i ) * 0.5f ) / static_cast< float >( bh );
						radius = std::max( radius, radiusAt( bw, bh, u + px, v + py ) );
						radius = std::max( radius, radiusAt( bw, bh, u - px, v - py ) );
					}

					if( radius < 0.5f )
					{
						to.at( x, y ) = from.at( x, y );
						continue;
					}

					Rgba sum        = from.at( x, y );
					float weightSum = 1.0f;
					const float step = radius / static_cast< float >( taps );

					for( int i = 1; i <= taps; ++i )
					{
						const float distance = step * static_cast< float >( i );
						const float t        = static_cast< float >( i ) / static_cast< float >( taps );
						const float gaussian = std::exp( -0.5f * ( t * 3.0f ) * ( t * 3.0f ) );

						for( int side = 0; side < 2; ++side )
						{
							const float sign = side == 0 ? 1.0f : -1.0f;
							const float au = u + sign * dx * distance / static_cast< float >( bw );
							const float av = v + sign * dy * distance / static_cast< float >( bh );

							const float reach =
								clamp01f( ( radiusAt( bw, bh, au, av ) - distance ) * 0.5f + 0.5f );
							const float w = gaussian * reach;

							const Rgba c = from.sampleMirrored( au, av );
							sum.r += c.r * w;
							sum.g += c.g * w;
							sum.b += c.b * w;
							sum.a += c.a * w;
							weightSum += w;
						}
					}

					Rgba& out = to.at( x, y );
					out.r     = sum.r / weightSum;
					out.g     = sum.g / weightSum;
					out.b     = sum.b / weightSum;
					out.a     = sum.a / weightSum;
				}
			}
		}
	}

	/// How far the aperture opening extends at this angle. 1 for circular.
	//= mirrored: Bokeh.cpp apertureEdge()
	float apertureEdge( float theta ) const
	{
		const int blades = settings.lens.blades;
		if( blades < 3 )
			return 1.0f;

		const float segment = 6.2831853072f / static_cast< float >( blades );
		const float half    = segment * 0.5f;
		const float a       = glslMod( theta + settings.lens.bladeRotation, segment ) - half;
		return std::cos( half ) / std::cos( a );
	}

	void gatherBokeh( const Plane& source, int bw, int bh )
	{
		blurred            = source;
		const int samples  = std::max( 1, settings.lens.bokehSamples );
		const float highlight = settings.lens.highlight;

		for( int y = 0; y < bh; ++y )
		{
			for( int x = 0; x < bw; ++x )
			{
				const float u = ( static_cast< float >( x ) + 0.5f ) / static_cast< float >( bw );
				const float v = ( static_cast< float >( y ) + 0.5f ) / static_cast< float >( bh );

				float radius = radiusAt( bw, bh, u, v );
				for( int i = 0; i < 4; ++i )
				{
					const float a = static_cast< float >( i ) * 1.5707963268f;
					radius = std::max( radius, radiusAt( bw, bh,
					                                     u + std::cos( a ) * blurRadius * 0.6f / static_cast< float >( bw ),
					                                     v + std::sin( a ) * blurRadius * 0.6f / static_cast< float >( bh ) ) );
				}

				if( radius < 0.5f )
				{
					blurred.at( x, y ) = source.at( x, y );
					continue;
				}

				Rgba sum;
				float weightSum = 0.0f;

				for( int i = 0; i < samples; ++i )
				{
					const float fi    = ( static_cast< float >( i ) + 0.5f ) / static_cast< float >( samples );
					const float theta = static_cast< float >( i ) * 2.3999632297f;
					const float rho   = std::sqrt( fi ) * apertureEdge( theta );

					const float distance = rho * radius;
					const float au = u + std::cos( theta ) * distance / static_cast< float >( bw );
					const float av = v + std::sin( theta ) * distance / static_cast< float >( bh );

					const float reach = clamp01f( ( radiusAt( bw, bh, au, av ) - distance ) * 0.5f + 0.5f );
					if( reach <= 0.0f )
						continue;

					const Rgba c = source.sampleMirrored( au, av );
					const float sr = c.a > 0.001f ? c.r / c.a : c.r;
					const float sg = c.a > 0.001f ? c.g / c.a : c.g;
					const float sb = c.a > 0.001f ? c.b / c.a : c.b;
					const float l  = lumaOf( sr, sg, sb );

					const float w = reach * ( 1.0f + highlight * l * l * l * l );

					sum.r += c.r * w;
					sum.g += c.g * w;
					sum.b += c.b * w;
					sum.a += c.a * w;
					weightSum += w;
				}

				Rgba& out = blurred.at( x, y );
				if( weightSum > 0.0f )
				{
					out.r = sum.r / weightSum;
					out.g = sum.g / weightSum;
					out.b = sum.b / weightSum;
					out.a = sum.a / weightSum;
				}
				else
				{
					out = source.at( x, y );
				}
			}
		}
	}

	//----------------------------------------------------------------------
	// Source access, always premultiplied RGBA floats.
	//----------------------------------------------------------------------
	Rgba readSource( int x, int y ) const
	{
		const OfxRectI bounds = srcImg->getBounds();
		const int sx = std::clamp( bounds.x1 + x, bounds.x1, bounds.x2 - 1 );
		const int sy = std::clamp( bounds.y1 + y, bounds.y1, bounds.y2 - 1 );

		const PIX* pix = static_cast< const PIX* >(
			const_cast< OFX::Image* >( srcImg )->getPixelAddress( sx, sy ) );
		if( pix == nullptr )
			return Rgba();

		Rgba out;
		out.r = static_cast< float >( pix[ 0 ] ) / static_cast< float >( maxValue );
		out.g = static_cast< float >( pix[ 1 ] ) / static_cast< float >( maxValue );
		out.b = static_cast< float >( pix[ 2 ] ) / static_cast< float >( maxValue );
		out.a = nComponents == 4 ? static_cast< float >( pix[ 3 ] ) / static_cast< float >( maxValue ) : 1.0f;

		if( !premultiplied && nComponents == 4 )
		{
			out.r *= out.a;
			out.g *= out.a;
			out.b *= out.a;
		}
		return out;
	}

	static PIX quantise( float v )
	{
		if( maxValue == 1 )
			return PIX( v );
		v = clamp01f( v );
		return PIX( v * maxValue + 0.5f );
	}
};

class TilterPlugin : public OFX::ImageEffect
{
public:
	explicit TilterPlugin( OfxImageEffectHandle handle ) :
		OFX::ImageEffect( handle )
	{
		dstClip = fetchClip( kOfxImageEffectOutputClipName );
		srcClip = fetchClip( kOfxImageEffectSimpleSourceClipName );

		preset        = fetchChoiceParam( kParamPreset );
		geometry      = fetchChoiceParam( kParamGeometry );
		focusX        = fetchDoubleParam( kParamFocusX );
		focusY        = fetchDoubleParam( kParamFocusY );
		angle         = fetchDoubleParam( kParamAngle );
		width         = fetchDoubleParam( kParamWidth );
		feather       = fetchDoubleParam( kParamFeather );
		ellipseAspect = fetchDoubleParam( kParamEllipseAspect );
		horizon       = fetchDoubleParam( kParamHorizon );
		tilt          = fetchDoubleParam( kParamTilt );
		rate          = fetchDoubleParam( kParamRate );
		depthBias     = fetchDoubleParam( kParamDepthBias );
		depthContrast = fetchDoubleParam( kParamDepthContrast );
		invert        = fetchBooleanParam( kParamInvert );
		blurModel     = fetchChoiceParam( kParamBlurModel );
		blurAmount    = fetchDoubleParam( kParamBlurAmount );
		quality       = fetchChoiceParam( kParamQuality );
		blades        = fetchChoiceParam( kParamBlades );
		bladeRotation = fetchDoubleParam( kParamBladeRotation );
		highlight     = fetchDoubleParam( kParamHighlight );
		saturation    = fetchDoubleParam( kParamSaturation );
		contrast      = fetchDoubleParam( kParamContrast );
		vignette      = fetchDoubleParam( kParamVignette );
		aberration    = fetchDoubleParam( kParamAberration );
		showFocus     = fetchBooleanParam( kParamShowFocus );
		mix           = fetchDoubleParam( kParamMix );
	}

	void render( const OFX::RenderArguments& args ) override
	{
		std::unique_ptr< OFX::Image > dst( dstClip->fetchImage( args.time ) );
		std::unique_ptr< OFX::Image > src( srcClip->fetchImage( args.time ) );

		const OFX::BitDepthEnum depth       = dst->getPixelDepth();
		const OFX::PixelComponentEnum comps = dst->getPixelComponents();

		if( comps != OFX::ePixelComponentRGBA && comps != OFX::ePixelComponentRGB )
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );

		const OfxRectI bounds = src->getBounds();
		const double par      = src->getPixelAspectRatio() > 0.0 ? src->getPixelAspectRatio() : 1.0;
		const double aspect   = double( bounds.x2 - bounds.x1 ) * par / double( bounds.y2 - bounds.y1 );

		const Settings settings = settingsAtTime( args.time, static_cast< float >( aspect ),
		                                          static_cast< float >( bounds.y2 - bounds.y1 ) );
		const bool premultiplied = srcClip->getPreMultiplication() == OFX::eImagePreMultiplied;

		switch( depth )
		{
			case OFX::eBitDepthUByte:
				comps == OFX::ePixelComponentRGBA
					? run< TilterProcessor< unsigned char, 4, 255 > >( args, dst.get(), src.get(), settings, premultiplied )
					: run< TilterProcessor< unsigned char, 3, 255 > >( args, dst.get(), src.get(), settings, premultiplied );
				break;
			case OFX::eBitDepthUShort:
				comps == OFX::ePixelComponentRGBA
					? run< TilterProcessor< unsigned short, 4, 65535 > >( args, dst.get(), src.get(), settings, premultiplied )
					: run< TilterProcessor< unsigned short, 3, 65535 > >( args, dst.get(), src.get(), settings, premultiplied );
				break;
			case OFX::eBitDepthFloat:
				comps == OFX::ePixelComponentRGBA
					? run< TilterProcessor< float, 4, 1 > >( args, dst.get(), src.get(), settings, premultiplied )
					: run< TilterProcessor< float, 3, 1 > >( args, dst.get(), src.get(), settings, premultiplied );
				break;
			default:
				OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
		}
	}

	void changedParam( const OFX::InstanceChangedArgs& args, const std::string& paramName ) override
	{
		using namespace tilter::presets;

		if( paramName == kParamPreset )
		{
			int chosen = 0;
			preset->getValue( chosen );
			if( chosen <= 0 || chosen > kCount || applyingPreset )
				return;

			// The copy IS the preset -- same table as the FFGL build, same 0..1
			// space. One edit block so undo takes the whole preset back at once.
			const Preset& p = kPresets[ chosen - 1 ];
			applyingPreset  = true;
			beginEditBlock( "Preset" );
			for( int i = 0; i < kParamCount; ++i )
				setParam( kPresetParamNames[ i ], p.v[ i ] );
			endEditBlock();
			applyingPreset = false;
			return;
		}

		// Editing a covered control while a preset is active hands control back
		// to the sliders. Judged by value, not by the change reason: hosts are
		// not consistent about reasons, but "no longer equal to the preset" is
		// unambiguous and absorbs the host echoing our own setValues.
		if( applyingPreset || args.reason == OFX::eChangeTime )
			return;

		int active = 0;
		preset->getValue( active );
		if( active <= 0 || active > kCount )
			return;

		const Preset& p = kPresets[ active - 1 ];
		for( int i = 0; i < kParamCount; ++i )
		{
			if( paramName == kPresetParamNames[ i ] && paramDiffers( kPresetParamNames[ i ], p.v[ i ] ) )
			{
				applyingPreset = true;
				preset->setValue( 0 );
				applyingPreset = false;
				return;
			}
		}
	}

	bool isIdentity( const OFX::IsIdentityArguments& args, OFX::Clip*& identityClip,
	                 double& identityTime ) override
	{
		// No blur, a neutral grade and nothing else switched on is the picture
		// that came in. Worth declaring: a host that knows a frame is untouched
		// can skip the whole render.
		const bool neutral =
			blurAmount->getValueAtTime( args.time ) <= 0.0
			&& std::fabs( saturation->getValueAtTime( args.time ) - 0.5 ) < 1e-9
			&& std::fabs( contrast->getValueAtTime( args.time ) - 0.5 ) < 1e-9
			&& vignette->getValueAtTime( args.time ) <= 0.0
			&& !showFocus->getValueAtTime( args.time );

		if( neutral || mix->getValueAtTime( args.time ) <= 0.0 )
		{
			identityClip = srcClip;
			identityTime = args.time;
			return true;
		}
		return false;
	}

private:
	Settings settingsAtTime( double time, float aspect, float frameHeight ) const
	{
		tilter::controls::HostValues host;

		int choice = 0;
		geometry->getValueAtTime( time, choice );
		host.geometry = static_cast< float >( choice );
		blurModel->getValueAtTime( time, choice );
		host.blurModel = static_cast< float >( choice );
		quality->getValueAtTime( time, choice );
		host.quality = static_cast< float >( choice );
		blades->getValueAtTime( time, choice );
		host.blades = static_cast< float >( choice );

		host.focusX        = static_cast< float >( focusX->getValueAtTime( time ) );
		host.focusY        = static_cast< float >( focusY->getValueAtTime( time ) );
		host.angle         = static_cast< float >( angle->getValueAtTime( time ) );
		host.width         = static_cast< float >( width->getValueAtTime( time ) );
		host.feather       = static_cast< float >( feather->getValueAtTime( time ) );
		host.ellipseAspect = static_cast< float >( ellipseAspect->getValueAtTime( time ) );
		host.horizon       = static_cast< float >( horizon->getValueAtTime( time ) );
		host.tilt          = static_cast< float >( tilt->getValueAtTime( time ) );
		host.rate          = static_cast< float >( rate->getValueAtTime( time ) );
		host.depthBias     = static_cast< float >( depthBias->getValueAtTime( time ) );
		host.depthContrast = static_cast< float >( depthContrast->getValueAtTime( time ) );
		host.invert        = invert->getValueAtTime( time ) ? 1.0f : 0.0f;
		host.blur          = static_cast< float >( blurAmount->getValueAtTime( time ) );
		host.bladeRotation = static_cast< float >( bladeRotation->getValueAtTime( time ) );
		host.highlight     = static_cast< float >( highlight->getValueAtTime( time ) );
		host.saturation    = static_cast< float >( saturation->getValueAtTime( time ) );
		host.contrast      = static_cast< float >( contrast->getValueAtTime( time ) );
		host.vignette      = static_cast< float >( vignette->getValueAtTime( time ) );
		host.aberration    = static_cast< float >( aberration->getValueAtTime( time ) );
		host.showFocus     = showFocus->getValueAtTime( time ) ? 1.0f : 0.0f;
		host.mix           = static_cast< float >( mix->getValueAtTime( time ) );

		Settings out;
		// The SAME conversion the FFGL build uses. That is the whole point of
		// Controls.cpp existing.
		out.field       = tilter::controls::field( host, aspect );
		out.lens        = tilter::controls::lens( host, frameHeight );
		out.aspectRatio = aspect;
		return out;
	}

	/// The preset table is plain floats; each parameter type reads one its own
	/// way. Option values are element indices, booleans are strictly 0 or 1.
	void setParam( const char* name, float value )
	{
		if( OFX::ChoiceParam* c = tryChoice( name ) )
		{
			const int v = static_cast< int >( std::lround( value ) );
			int current = 0;
			c->getValue( current );
			if( current != v )
				c->setValue( v );
			return;
		}
		if( OFX::BooleanParam* b = tryBoolean( name ) )
		{
			const bool v = value >= 0.5f;
			bool current = false;
			b->getValue( current );
			if( current != v )
				b->setValue( v );
			return;
		}
		if( OFX::DoubleParam* d = tryDouble( name ) )
		{
			double current = 0.0;
			d->getValue( current );
			if( std::fabs( current - double( value ) ) > 1e-6 )
				d->setValue( double( value ) );
		}
	}

	bool paramDiffers( const char* name, float value ) const
	{
		if( OFX::ChoiceParam* c = tryChoice( name ) )
		{
			int current = 0;
			c->getValue( current );
			return current != static_cast< int >( std::lround( value ) );
		}
		if( OFX::BooleanParam* b = tryBoolean( name ) )
		{
			bool current = false;
			b->getValue( current );
			return current != ( value >= 0.5f );
		}
		if( OFX::DoubleParam* d = tryDouble( name ) )
		{
			double current = 0.0;
			d->getValue( current );
			return std::fabs( current - double( value ) ) > 1e-6;
		}
		return false;
	}

	OFX::ChoiceParam* tryChoice( const std::string& name ) const
	{
		if( name == kParamGeometry ) return geometry;
		if( name == kParamBlurModel ) return blurModel;
		if( name == kParamBlades ) return blades;
		return nullptr;
	}
	OFX::BooleanParam* tryBoolean( const std::string& name ) const
	{
		if( name == kParamInvert ) return invert;
		if( name == kParamShowFocus ) return showFocus;
		return nullptr;
	}
	OFX::DoubleParam* tryDouble( const std::string& name ) const
	{
		if( name == kParamFocusX ) return focusX;
		if( name == kParamFocusY ) return focusY;
		if( name == kParamAngle ) return angle;
		if( name == kParamWidth ) return width;
		if( name == kParamFeather ) return feather;
		if( name == kParamEllipseAspect ) return ellipseAspect;
		if( name == kParamHorizon ) return horizon;
		if( name == kParamTilt ) return tilt;
		if( name == kParamRate ) return rate;
		if( name == kParamDepthBias ) return depthBias;
		if( name == kParamDepthContrast ) return depthContrast;
		if( name == kParamBlurAmount ) return blurAmount;
		if( name == kParamBladeRotation ) return bladeRotation;
		if( name == kParamHighlight ) return highlight;
		if( name == kParamSaturation ) return saturation;
		if( name == kParamContrast ) return contrast;
		if( name == kParamVignette ) return vignette;
		if( name == kParamAberration ) return aberration;
		if( name == kParamMix ) return mix;
		return nullptr;
	}

	template< class Processor >
	void run( const OFX::RenderArguments& args, OFX::Image* dst, OFX::Image* src,
	          const Settings& settings, bool premultiplied )
	{
		Processor processor( *this );
		processor.setDstImg( dst );
		processor.setup( src, settings, premultiplied );
		processor.setRenderWindow( args.renderWindow );
		processor.process();
	}

	OFX::Clip* dstClip = nullptr;
	OFX::Clip* srcClip = nullptr;

	OFX::ChoiceParam* preset        = nullptr;
	OFX::ChoiceParam* geometry      = nullptr;
	OFX::DoubleParam* focusX        = nullptr;
	OFX::DoubleParam* focusY        = nullptr;
	OFX::DoubleParam* angle         = nullptr;
	OFX::DoubleParam* width         = nullptr;
	OFX::DoubleParam* feather       = nullptr;
	OFX::DoubleParam* ellipseAspect = nullptr;
	OFX::DoubleParam* horizon       = nullptr;
	OFX::DoubleParam* tilt          = nullptr;
	OFX::DoubleParam* rate          = nullptr;
	OFX::DoubleParam* depthBias     = nullptr;
	OFX::DoubleParam* depthContrast = nullptr;
	OFX::BooleanParam* invert       = nullptr;
	OFX::ChoiceParam* blurModel     = nullptr;
	OFX::DoubleParam* blurAmount    = nullptr;
	OFX::ChoiceParam* quality       = nullptr;
	OFX::ChoiceParam* blades        = nullptr;
	OFX::DoubleParam* bladeRotation = nullptr;
	OFX::DoubleParam* highlight     = nullptr;
	OFX::DoubleParam* saturation    = nullptr;
	OFX::DoubleParam* contrast      = nullptr;
	OFX::DoubleParam* vignette      = nullptr;
	OFX::DoubleParam* aberration    = nullptr;
	OFX::BooleanParam* showFocus    = nullptr;
	OFX::DoubleParam* mix           = nullptr;

	/// True while our own setValues are in flight, so the resulting
	/// changedParam callbacks are not mistaken for the operator editing.
	bool applyingPreset = false;
};

OFX::DoubleParamDescriptor* defineSlider( OFX::ImageEffectDescriptor& desc,
                                          OFX::PageParamDescriptor* page,
                                          const char* name, const char* label,
                                          const char* hint, double def )
{
	OFX::DoubleParamDescriptor* p = desc.defineDoubleParam( name );
	p->setLabels( label, label, label );
	p->setHint( hint );
	p->setRange( 0.0, 1.0 );
	p->setDisplayRange( 0.0, 1.0 );
	p->setDefault( def );
	page->addChild( *p );
	return p;
}

} // namespace

mDeclarePluginFactory( TilterPluginFactory, {}, {} );

void TilterPluginFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	desc.setLabels( kPluginName, kPluginName, kPluginName );
	desc.setPluginGrouping( kPluginGrouping );
	desc.setPluginDescription( kPluginDescription );

	desc.addSupportedContext( OFX::eContextFilter );
	desc.addSupportedContext( OFX::eContextGeneral );

	desc.addSupportedBitDepth( OFX::eBitDepthUByte );
	desc.addSupportedBitDepth( OFX::eBitDepthUShort );
	desc.addSupportedBitDepth( OFX::eBitDepthFloat );

	// A blur gathers from anywhere within its radius, so it cannot render from
	// tiles. Frames are still independent of each other and of render order --
	// this plugin has no clock and no history.
	desc.setSupportsTiles( false );
	desc.setTemporalClipAccess( false );
	desc.setRenderThreadSafety( OFX::eRenderFullySafe );
	desc.setSupportsMultiResolution( true );
}

void TilterPluginFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum )
{
	OFX::ClipDescriptor* srcClip = desc.defineClip( kOfxImageEffectSimpleSourceClipName );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGB );
	srcClip->setSupportsTiles( false );

	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	// Same parameters, same 0..1 ranges, same defaults as the FFGL build, so
	// the two inspectors read identically and the docs cover both.
	OFX::PageParamDescriptor* page = desc.definePageParam( "Controls" );
	const tilter::controls::HostValues defaults;

	OFX::ChoiceParamDescriptor* presetParam = desc.defineChoiceParam( kParamPreset );
	presetParam->setLabels( "Preset", "Preset", "Preset" );
	presetParam->setHint( "Factory lenses. Picking one sets the look controls; editing any "
	                      "of them afterwards falls back to Custom." );
	presetParam->appendOption( "Custom" );
	for( int i = 0; i < tilter::presets::kCount; ++i )
		presetParam->appendOption( tilter::presets::kPresets[ i ].name );
	presetParam->setDefault( 0 );
	presetParam->setIsPersistant( true );
	presetParam->setEvaluateOnChange( false );//the copied values re-render; the label does not
	presetParam->setAnimates( false );
	page->addChild( *presetParam );

	//------------------------------------------------------------------ Focus
	OFX::GroupParamDescriptor* focusGroup = desc.defineGroupParam( "Focus" );
	focusGroup->setLabels( "Focus", "Focus", "Focus" );

	OFX::ChoiceParamDescriptor* geometryParam = desc.defineChoiceParam( kParamGeometry );
	geometryParam->setLabels( "Focus Shape", "Focus Shape", "Focus Shape" );
	geometryParam->setHint( "What is in focus. Tilted Plane is the one with a real lens's "
	                        "falloff: the blur stops growing at the horizon." );
	for( int i = 0; i < tilter::controls::geometryCount(); ++i )
		geometryParam->appendOption( tilter::controls::geometryLabel( i ) );
	geometryParam->setDefault( 0 );
	geometryParam->setParent( *focusGroup );
	page->addChild( *geometryParam );

	defineSlider( desc, page, kParamFocusX, "Focus X",
	              "Centre of the sharp region across the frame.", defaults.focusX )
		->setParent( *focusGroup );
	defineSlider( desc, page, kParamFocusY, "Focus Y",
	              "Centre of the sharp region up the frame; 0 is the top.", defaults.focusY )
		->setParent( *focusGroup );
	defineSlider( desc, page, kParamAngle, "Angle",
	              "Rotation of the band, ellipse or horizon. Plus or minus 90 degrees -- "
	              "a band is its own mirror image at 180.", defaults.angle )
		->setParent( *focusGroup );
	defineSlider( desc, page, kParamWidth, "Focus Width",
	              "Half-width of the fully sharp zone, in fractions of the frame height.",
	              defaults.width )
		->setParent( *focusGroup );
	defineSlider( desc, page, kParamFeather, "Feather",
	              "How far it takes to ramp from sharp to full blur.", defaults.feather )
		->setParent( *focusGroup );
	defineSlider( desc, page, kParamEllipseAspect, "Iris Aspect",
	              "Radial only: the ellipse's shape. 0.5 is a circle.", defaults.ellipseAspect )
		->setParent( *focusGroup );
	defineSlider( desc, page, kParamHorizon, "Horizon",
	              "Tilted Plane only: where infinity sits. Past it the blur stops growing.",
	              defaults.horizon )
		->setParent( *focusGroup );
	defineSlider( desc, page, kParamTilt, "Tilt",
	              "Tilted Plane only: swings the focal plane so the sharp zone converges. "
	              "0.5 is no swing.", defaults.tilt )
		->setParent( *focusGroup );
	defineSlider( desc, page, kParamRate, "Falloff Rate",
	              "Tilted Plane only: how fast blur grows with distance. The aperture, "
	              "in effect. 0.5 is unity.", defaults.rate )
		->setParent( *focusGroup );
	defineSlider( desc, page, kParamDepthBias, "Depth Focus",
	              "Image Depth only: which guessed depth is in focus.", defaults.depthBias )
		->setParent( *focusGroup );
	defineSlider( desc, page, kParamDepthContrast, "Depth Contrast",
	              "Image Depth only: how hard the guess separates near from far.",
	              defaults.depthContrast )
		->setParent( *focusGroup );

	OFX::BooleanParamDescriptor* invertParam = desc.defineBooleanParam( kParamInvert );
	invertParam->setLabels( "Invert Focus", "Invert Focus", "Invert Focus" );
	invertParam->setHint( "Swap the sharp and blurred regions." );
	invertParam->setDefault( false );
	invertParam->setParent( *focusGroup );
	page->addChild( *invertParam );

	//------------------------------------------------------------------- Lens
	OFX::GroupParamDescriptor* lensGroup = desc.defineGroupParam( "Lens" );
	lensGroup->setLabels( "Lens", "Lens", "Lens" );

	OFX::ChoiceParamDescriptor* blurParam = desc.defineChoiceParam( kParamBlurModel );
	blurParam->setLabels( "Blur", "Blur", "Blur" );
	blurParam->setHint( "Bokeh Disc gathers over a real aperture and makes highlights bloom "
	                    "into aperture shapes. Gaussian is cheaper and smears them." );
	for( int i = 0; i < tilter::controls::blurModelCount(); ++i )
		blurParam->appendOption( tilter::controls::blurModelLabel( i ) );
	blurParam->setDefault( 0 );
	blurParam->setParent( *lensGroup );
	page->addChild( *blurParam );

	defineSlider( desc, page, kParamBlurAmount, "Blur Amount",
	              "Maximum blur radius, as a fraction of the frame height -- so it looks "
	              "the same at any resolution.", defaults.blur )
		->setParent( *lensGroup );

	OFX::ChoiceParamDescriptor* qualityParam = desc.defineChoiceParam( kParamQuality );
	qualityParam->setLabels( "Quality", "Quality", "Quality" );
	qualityParam->setHint( "How finely the blur is sampled. Never how big it is." );
	for( int i = 0; i < tilter::controls::qualityCount(); ++i )
		qualityParam->appendOption( tilter::controls::qualityLabel( i ) );
	qualityParam->setDefault( 1 );
	qualityParam->setParent( *lensGroup );
	page->addChild( *qualityParam );

	OFX::ChoiceParamDescriptor* bladesParam = desc.defineChoiceParam( kParamBlades );
	bladesParam->setLabels( "Aperture", "Aperture", "Aperture" );
	bladesParam->setHint( "The shape of the hole the light comes through, and therefore of "
	                      "every out-of-focus highlight." );
	for( int i = 0; i < tilter::controls::bladesCount(); ++i )
		bladesParam->appendOption( tilter::controls::bladesLabel( i ) );
	bladesParam->setDefault( 0 );
	bladesParam->setParent( *lensGroup );
	page->addChild( *bladesParam );

	defineSlider( desc, page, kParamBladeRotation, "Aperture Angle",
	              "Rotates the aperture polygon.", defaults.bladeRotation )
		->setParent( *lensGroup );
	defineSlider( desc, page, kParamHighlight, "Highlights",
	              "How strongly bright points dominate the blur. This is what makes them "
	              "bloom into discs rather than smear.", defaults.highlight )
		->setParent( *lensGroup );

	//------------------------------------------------------------ Photograph
	OFX::GroupParamDescriptor* photoGroup = desc.defineGroupParam( "Photograph" );
	photoGroup->setLabels( "Photograph", "Photograph", "Photograph" );

	defineSlider( desc, page, kParamSaturation, "Saturation", "0.5 is unity.", defaults.saturation )
		->setParent( *photoGroup );
	defineSlider( desc, page, kParamContrast, "Contrast", "0.5 is unity.", defaults.contrast )
		->setParent( *photoGroup );
	defineSlider( desc, page, kParamVignette, "Vignette", "Corner falloff.", defaults.vignette )
		->setParent( *photoGroup );
	defineSlider( desc, page, kParamAberration, "Aberration",
	              "Lateral colour fringing. Grows with defocus and reverses across the "
	              "plane of focus, the way real glass does.", defaults.aberration )
		->setParent( *photoGroup );

	//---------------------------------------------------------------- Output
	OFX::GroupParamDescriptor* outputGroup = desc.defineGroupParam( "Output" );
	outputGroup->setLabels( "Output", "Output", "Output" );

	OFX::BooleanParamDescriptor* showParam = desc.defineBooleanParam( kParamShowFocus );
	showParam->setLabels( "Show Focus", "Show Focus", "Show Focus" );
	showParam->setHint( "Paint the focus field over the picture: warm is the near side of "
	                    "focus, cold is the far side. For placing a focal plane by eye." );
	showParam->setDefault( false );
	showParam->setParent( *outputGroup );
	page->addChild( *showParam );

	defineSlider( desc, page, kParamMix, "Mix", "Wet/dry against the untouched input.", defaults.mix )
		->setParent( *outputGroup );
}

OFX::ImageEffect* TilterPluginFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new TilterPlugin( handle );
}

void OFX::Plugin::getPluginIDs( OFX::PluginFactoryArray& ids )
{
	// Deliberately leaked: a by-value static would register an exit-time
	// destructor inside this module, and a host that dlclose()s the bundle
	// before process exit then jumps through a dangling pointer.
	static TilterPluginFactory* factory =
		new TilterPluginFactory( kPluginIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	ids.push_back( factory );
}
