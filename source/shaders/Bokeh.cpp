#include "../Shaders.h"

namespace tilter::shaders
{
/*
    A single-pass disc gather with a real aperture. The good path.

    ------------------------------------------------------------- why one pass

    A lens does not blur horizontally and then vertically. The out-of-focus
    image of a point is the shape of the hole the light came through, which is
    the aperture -- a circle when it is wide open, a polygon of however many
    blades when it is stopped down. That shape cannot be produced by two
    separable passes at all, so this gathers over the whole disc at once and
    pays for it.

    ------------------------------------------------------------ the sampling

    Golden-angle spiral: the i'th of N samples sits at radius sqrt((i+0.5)/N)
    and angle i * 2.39996 radians. That covers a disc evenly at any N, with no
    preferred direction for the eye to latch onto -- a square grid of taps makes
    a visible lattice in a large bokeh disc and a ring pattern makes visible
    rings.

    The polygon is applied by **scaling** the sample radius to the aperture's
    edge at that angle rather than by rejecting samples outside it. Rejection
    would throw away most of the tap budget at high blade counts and leave the
    disc noisier the more it was stopped down, which is backwards.

    ------------------------------------------------------------ the highlights

    A bright point in an out-of-focus region should become a *disc of that
    brightness*, not a faint smear. That only happens if the bright sample
    dominates the average, so each tap is weighted by a high power of its own
    luminance. At Highlight = 0 the weighting is flat and this is a plain
    aperture-shaped average; turning it up is what makes streetlights and
    specular hits bloom into visible aperture shapes, which is the entire reason
    anyone reaches for this mode.

    Note that this deliberately does NOT conserve the picture's average
    brightness -- a weighted mean biased toward highlights is brighter than the
    mean. That is what the real thing does too.
*/
const char* const kBokehFragment = R"(#version 410 core
uniform sampler2D SourceTexture;
uniform sampler2D CoCTexture;

uniform vec2 SourceMaxUV;
uniform vec2 SourceHalfTexel;

uniform vec2 FrameSize;
uniform float MaxRadius;
uniform int Samples;
uniform int Blades;         //0 = circular aperture, otherwise 3..9
uniform float BladeRotation;//radians
uniform float Highlight;

in vec2 uv;
out vec4 fragColor;


//Reflect a picture-space coordinate back inside the frame, rather than clamp.
//
//Under a clamp every tap falling off the edge returns the edge texel, so the
//outermost row is averaged largely with copies of itself and comes out less
//blurred than the middle of the frame. Mirroring gives it real picture to
//average with instead, so the blur has the same strength everywhere.
//
//Worth being straight about the evidence: this was changed while chasing a
//measured non-monotonicity in tiltest --blur, on the theory that a sharp
//edge rim growing with the radius was the cause. It was NOT -- swapping clamp
//for mirror moved whole-frame detail from 3.24/4.26 to 3.24/4.26, which is to
//say not at all. The real cause was the tap sum aliasing, fixed by the box
//prefilter in Downsample.cpp.
//
//Mirroring is kept because it is the better edge rule on its own merits, not
//because it fixed anything measured here.
float mirror1( float x )
{
	x = abs( x );
	x = mod( x, 2.0 );
	return x > 1.0 ? 2.0 - x : x;
}

vec2 mirrorUV( vec2 p )
{
	return vec2( mirror1( p.x ), mirror1( p.y ) );
}

vec4 fetch( vec2 p )
{
	vec2 t = mirrorUV( p ) * SourceMaxUV;
	return texture( SourceTexture, clamp( t, SourceHalfTexel, SourceMaxUV - SourceHalfTexel ) );
}

float radiusAt( vec2 p )
{
	//Mirrored to match fetch(): the radius used at a tap must come from the
	//same place the colour did, or the reach test is comparing a tap against
	//somebody else's circle of confusion.
	return abs( texture( CoCTexture, mirrorUV( p ) ).r ) * MaxRadius;
}

/// How far the aperture opening extends at this angle, as a fraction of the
/// circumscribed circle. 1.0 everywhere for a circular aperture.
float apertureEdge( float theta )
{
	if( Blades < 3 )
		return 1.0;

	float segment = 6.2831853072 / float( Blades );
	float halfSegment = segment * 0.5;
	//Distance from the centre of the nearest blade facet, folded into
	//-half..+half, then the flat side of the polygon at that angle.
	float a = mod( theta + BladeRotation, segment ) - halfSegment;
	return cos( halfSegment ) / cos( a );
}

void main()
{
	vec2 pixelToUV = 1.0 / FrameSize;

	//Same dilation as the Gaussian path, but probed on a ring rather than along
	//an axis, because this pass has no axis.
	float radius = radiusAt( uv );
	for( int i = 0; i < 4; ++i )
	{
		float a = float( i ) * 1.5707963268;
		vec2 probe = vec2( cos( a ), sin( a ) ) * ( MaxRadius * 0.6 ) * pixelToUV;
		radius = max( radius, radiusAt( uv + probe ) );
	}

	if( radius < 0.5 )
	{
		fragColor = fetch( uv );
		return;
	}

	vec4 sum = vec4( 0.0 );
	float weightSum = 0.0;

	for( int i = 0; i < Samples; ++i )
	{
		float fi = ( float( i ) + 0.5 ) / float( Samples );
		//sqrt for uniform area density: without it every tap crowds the centre
		//and the disc has a hot core.
		float rho = sqrt( fi );
		float theta = float( i ) * 2.3999632297;

		rho *= apertureEdge( theta );

		float distance = rho * radius;
		vec2 at = uv + vec2( cos( theta ), sin( theta ) ) * distance * pixelToUV;

		float reach = clamp( ( radiusAt( at ) - distance ) * 0.5 + 0.5, 0.0, 1.0 );
		if( reach <= 0.0 )
			continue;

		vec4 c = fetch( at );

		//Premultiplied in, so undo it before judging brightness or every
		//semi-transparent highlight is weighted as though it were dim.
		vec3 straight = c.a > 0.001 ? c.rgb / c.a : c.rgb;
		float luma = dot( straight, vec3( 0.2126, 0.7152, 0.0722 ) );

		//Fourth power: gentle enough that mid-tones are barely reweighted,
		//steep enough that a clipped highlight is worth an order of magnitude
		//more than its neighbours and therefore paints a whole disc.
		float w = reach * ( 1.0 + Highlight * luma * luma * luma * luma );

		sum += c * w;
		weightSum += w;
	}

	//Every tap can fail the reach test on a lone sharp pixel inside a blurred
	//neighbourhood. Falling back to the centre is right and, more to the point,
	//is not a divide by zero in somebody's programme output.
	fragColor = weightSum > 0.0 ? sum / weightSum : fetch( uv );
}
)";
} // namespace tilter::shaders
