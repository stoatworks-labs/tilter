#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace tilter
{
namespace controls
{
namespace
{
float lerp( float a, float b, float t )
{
	return a + ( b - a ) * t;
}

/// A ratio control: 0.5 is unity, and equal distances either side are
/// reciprocal factors of each other. `span` is the factor at the top end.
float ratio( float x, float span )
{
	return std::exp( ( x - 0.5f ) * 2.0f * std::log( span ) );
}

const char* const kGeometryLabels[ focus::kGeometryCount ] = {
	"Linear Band",
	"Radial",
	"Tilted Plane",
	//Named as a guess in the operator's own dropdown, because it is one.
	"Image Depth (guess)"
};

const char* const kBlurModelLabels[ kBlurModelCount ] = {
	"Gaussian",
	"Bokeh Disc"
};

/// Quality is a tap budget and nothing else -- it changes how finely the same
/// blur is sampled, never how big it is. Draft exists so a 4K composition with
/// four instances of this on it stays playable.
struct QualityStep
{
	const char* label;
	int gaussianTaps;
	int bokehSamples;
};

const QualityStep kQuality[] = {
	{ "Draft", 4, 24 },
	{ "Good", 8, 48 },
	{ "Best", 16, 96 },
};

constexpr int kQualityCount = int( sizeof( kQuality ) / sizeof( kQuality[ 0 ] ) );

struct BladeStep
{
	const char* label;
	int blades;
};

//Real aperture blade counts. Five and six are the common ones and the reason
//everybody recognises a pentagonal or hexagonal bokeh; a wide-open lens has no
//polygon at all, which is what Circular is.
const BladeStep kBlades[] = {
	{ "Circular", 0 },
	{ "5 Blades", 5 },
	{ "6 Blades", 6 },
	{ "7 Blades", 7 },
	{ "8 Blades", 8 },
	{ "9 Blades", 9 },
};

constexpr int kBladesCount = int( sizeof( kBlades ) / sizeof( kBlades[ 0 ] ) );
} // namespace

int geometryCount()
{
	return focus::kGeometryCount;
}

const char* geometryLabel( int index )
{
	if( index < 0 || index >= focus::kGeometryCount )
		return kGeometryLabels[ 0 ];
	return kGeometryLabels[ index ];
}

int blurModelCount()
{
	return kBlurModelCount;
}

const char* blurModelLabel( int index )
{
	if( index < 0 || index >= kBlurModelCount )
		return kBlurModelLabels[ 0 ];
	return kBlurModelLabels[ index ];
}

int qualityCount()
{
	return kQualityCount;
}

const char* qualityLabel( int index )
{
	if( index < 0 || index >= kQualityCount )
		return kQuality[ 1 ].label;
	return kQuality[ index ].label;
}

int bladesCount()
{
	return kBladesCount;
}

const char* bladesLabel( int index )
{
	if( index < 0 || index >= kBladesCount )
		return kBlades[ 0 ].label;
	return kBlades[ index ].label;
}

int bladesValue( int index )
{
	if( index < 0 || index >= kBladesCount )
		return 0;
	return kBlades[ index ].blades;
}

int option( float value, int elementCount )
{
	const int chosen = static_cast< int >( std::lround( value ) );
	return std::clamp( chosen, 0, std::max( 0, elementCount - 1 ) );
}

focus::Field field( const HostValues& host, float aspectRatio )
{
	focus::Field out;

	out.geometry = option( host.geometry, focus::kGeometryCount );

	out.centreU = host.focusX;
	out.centreV = host.focusY;

	//Plus or minus a quarter turn. A band is its own mirror image at 180
	//degrees, so a full turn would mean every angle appeared twice -- which
	//also makes a slider sweep from one end to the other render the same
	//picture at both ends and report a working control as dead.
	out.angle = ( host.angle - 0.5f ) * 3.14159265f;

	//Frame-height units for the first three geometries; units of guessed depth
	//for Image Depth. Same number, different axis -- see Focus.h.
	out.width   = host.width * 0.5f;
	out.feather = lerp( 0.001f, 0.6f, host.feather );

	out.aspect = ratio( host.ellipseAspect, 4.0f );

	//Negative is above the sharp zone, which is where a horizon usually is.
	out.horizon = lerp( -0.6f, 0.6f, host.horizon );
	out.tilt    = ( host.tilt - 0.5f ) * 2.0f;
	out.rate    = ratio( host.rate, 4.0f );

	out.depthBias     = host.depthBias;
	out.depthContrast = host.depthContrast * 3.0f;

	out.invert      = host.invert >= 0.5f;
	out.aspectRatio = aspectRatio;

	return out;
}

Lens lens( const HostValues& host, float frameHeight )
{
	Lens out;

	out.model = option( host.blurModel, kBlurModelCount );

	//A radius in pixels would mean a composition resized from 1080p to 4K got
	//half the blur it had, so the control is a fraction of the frame height and
	//the pixels are worked out here. Eight per cent of the height is a very
	//shallow lens indeed and is deliberately past what anybody needs.
	out.maxRadius = host.blur * 0.08f * std::max( frameHeight, 1.0f );

	const int quality = option( host.quality, kQualityCount );
	out.gaussianTaps  = kQuality[ quality ].gaussianTaps;
	out.bokehSamples  = kQuality[ quality ].bokehSamples;

	out.blades = bladesValue( option( host.blades, kBladesCount ) );
	//A quarter turn covers every distinct orientation of any polygon with four
	//or more sides, and is plenty for a five.
	out.bladeRotation = host.bladeRotation * 1.5707963f;
	out.highlight     = host.highlight * 8.0f;

	out.saturation = host.saturation * 2.0f;
	out.contrast   = host.contrast * 2.0f;
	out.vignette   = host.vignette;
	out.aberration = host.aberration;

	out.showFocus = host.showFocus >= 0.5f;
	out.mix       = host.mix;

	return out;
}

} // namespace controls
} // namespace tilter
