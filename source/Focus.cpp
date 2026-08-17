#include "Focus.h"

#include <algorithm>
#include <cmath>

namespace tilter
{
namespace focus
{
namespace
{
float clamp01( float x )
{
	return x < 0.0f ? 0.0f : ( x > 1.0f ? 1.0f : x );
}

float smoothstep01( float x )
{
	x = clamp01( x );
	return x * x * ( 3.0f - 2.0f * x );
}

/**
    The point in band-aligned, aspect-corrected coordinates.

    `along` runs parallel to the band, `across` along its normal, both in units
    of **frame height**. The aspect correction is on the u axis only, which is
    what makes a circle a circle and keeps a band the same thickness when the
    composition changes shape.
*/
void bandCoords( const Field& field, float u, float v, float& along, float& across )
{
	const float px = ( u - field.centreU ) * field.aspectRatio;
	const float py = ( v - field.centreV );

	const float c = std::cos( field.angle );
	const float s = std::sin( field.angle );

	along  = px * c + py * s;
	across = -px * s + py * c;
}
} // namespace

float shape( const Field& field, float raw )
{
	//A feather of zero is a legitimate thing to ask for -- a hard-edged focus
	//region is a look -- so it is floored rather than rejected, and the floor is
	//small enough that the ramp is a single pixel at any sane resolution.
	const float feather = std::max( field.feather, 1e-4f );

	float magnitude = smoothstep01( ( std::fabs( raw ) - field.width ) / feather );

	if( field.invert )
		magnitude = 1.0f - magnitude;

	//The sign is which side of focus, and it survives inversion: a pixel does
	//not move to the other side of the focal plane because the operator asked
	//for the sharp and blurred regions to be swapped.
	return raw < 0.0f ? -magnitude : magnitude;
}

float guessedDepth( const Field& field, float luma, float detail )
{
	//Aerial perspective, and nothing cleverer. Distance scatters light into the
	//line of sight: contrast washes out and luminance lifts toward the haze. So
	//a smooth bright region reads as far away and a detailed dark one as near.
	//
	//The two cues are weighted equally because neither is good enough to lead.
	//Luma alone calls every white wall the sky; detail alone calls every patch
	//of clear sky the nearest thing in frame. Together they are wrong less
	//often, which is the most that can be claimed for this.
	const float far = 0.5f * clamp01( luma ) + 0.5f * ( 1.0f - clamp01( detail ) );

	//Contrast pivots about the midpoint so that turning it up separates near
	//from far, rather than pushing the whole picture to one end.
	return clamp01( 0.5f + ( far - 0.5f ) * std::max( field.depthContrast, 0.0f ) );
}

float defocus( const Field& field, float u, float v, float depthLuma, float depthDetail )
{
	float along  = 0.0f;
	float across = 0.0f;
	bandCoords( field, u, v, along, across );

	switch( field.geometry )
	{
		case kRadial:
		{
			//The ellipse is measured in the band-aligned frame, so Angle
			//rotates it and Aspect stretches it along its own axes rather than
			//the frame's.
			const float a  = std::max( field.aspect, 1e-3f );
			const float ry = across / a;
			//Always positive: everything outside the ellipse is on the same
			//side of focus, so there is no near and far here to give a sign to.
			return shape( field, std::sqrt( along * along + ry * ry ) );
		}

		case kTiltedPlane:
		{
			//Inverse depth of a plane under perspective is an affine function
			//of image position -- and it has a floor, because nothing is
			//further away than infinity. `horizon` is where that floor bites.
			//
			//That floor is the whole point of this mode. On the far side the
			//blur ramps up and then STOPS; past the horizon every pixel shares
			//one blur. On the near side there is no limit.
			const float sceneW = std::max( 0.0f, across - field.horizon );
			const float focusW = std::max( 0.0f, -field.horizon );

			//Tilt swings the plane of focus about the viewing axis, so the
			//focused depth varies ALONG the band as well as across it. The
			//in-focus locus stops being parallel to the horizon and the sharp
			//zone converges the way it does in a real tilt-shift frame. At
			//tilt = 0 this term vanishes and the mode is a band with a
			//plateau.
			const float lensW = focusW + field.tilt * along;

			return shape( field, field.rate * ( sceneW - lensW ) );
		}

		case kImageDepth:
		{
			//Here `width` and `feather` are in units of guessed depth (0..1),
			//not frame height. There is no way around that -- the axis being
			//measured is not a distance in the picture -- and it is why this
			//mode's defaults are set separately from the other three.
			return shape( field, guessedDepth( field, depthLuma, depthDetail ) - field.depthBias );
		}

		case kLinearBand:
		default:
			return shape( field, across );
	}
}

const char* geometryName( int geometry )
{
	switch( geometry )
	{
		case kRadial: return "Radial";
		case kTiltedPlane: return "Tilted Plane";
		//Named as a guess in the host's own dropdown, because it is one and an
		//operator picking it should know that before they wonder why a bright
		//foreground went soft.
		case kImageDepth: return "Image Depth (guess)";
		case kLinearBand:
		default: return "Linear Band";
	}
}

} // namespace focus
} // namespace tilter
