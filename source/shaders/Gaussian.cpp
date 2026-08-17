#include "../Shaders.h"

namespace tilter::shaders
{
/*
    Separable blur with a per-pixel radius. The cheap path.

    ------------------------------------------------------- what it gets right

    **The tap reach test.** A pixel's colour comes from the scene points whose
    circles of confusion cover it -- so a tap contributes only if *its own* CoC
    reaches this far. Without that test a sharp subject against a blurred
    background collects background colour and grows a halo, which is the single
    most recognisable sign of a fake depth of field.

    Because the test asks about the tap rather than the centre, it works in both
    directions on its own: a blurred foreground has a large CoC, so it does
    reach across and spread over a sharp background; a sharp background has a
    small one, so it does not bleed into the blur beside it.

    **The dilated gather radius.** Sample offsets scaled by the centre pixel's
    own radius would mean a sharp pixel never looks far enough to find the
    blurred foreground that ought to be spreading onto it. So the radius is the
    largest CoC found in a handful of probes along the blur axis, not the
    centre's.

    -------------------------------------------------------- what it gets wrong

    **It is not truly separable.** A two-pass separable blur is exact only for a
    constant radius; here the radius varies per pixel, so the second pass blurs
    pixels that were blurred by a different amount in the first. The error shows
    up as a slight cross-shaped stretch where the radius changes fastest. It is
    the standard approximation and it is why Bokeh exists as the other option --
    that one is a single gather and has no such error.

    **Highlights smear rather than form discs.** A Gaussian conserves the
    picture's energy but spreads a bright point into a soft blob. That is the
    honest reason this is the cheap mode and not the good one.
*/
const char* const kGaussianFragment = R"(#version 410 core
uniform sampler2D SourceTexture;
uniform sampler2D CoCTexture;

uniform vec2 SourceMaxUV;
uniform vec2 SourceHalfTexel;

uniform vec2 Direction;  //unit vector: (1,0) horizontal pass, (0,1) vertical
uniform vec2 FrameSize;  //pixels, for converting a radius into picture space
uniform float MaxRadius; //pixels at full defocus
uniform int Taps;        //per side

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
//measured non-monotonicity in `tiltest --blur`, on the theory that a sharp
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

/// Radius in pixels at a point. The CoC buffer is ours, so it has no padding.
float radiusAt( vec2 p )
{
	//Mirrored to match fetch(): the radius used at a tap must come from the
	//same place the colour did, or the reach test is comparing a tap against
	//somebody else's circle of confusion.
	return abs( texture( CoCTexture, mirrorUV( p ) ).r ) * MaxRadius;
}

void main()
{
	vec2 pixelToUV = 1.0 / FrameSize;

	//The gather radius is the largest CoC anywhere a blurred neighbour could be
	//reaching us from, not the centre's own. See the header.
	float radius = radiusAt( uv );
	for( int i = 1; i <= 2; ++i )
	{
		vec2 probe = Direction * ( MaxRadius * float( i ) * 0.5 ) * pixelToUV;
		radius = max( radius, radiusAt( uv + probe ) );
		radius = max( radius, radiusAt( uv - probe ) );
	}

	//Below half a pixel there is nothing a blur could do that the source has
	//not already done, and the whole sharp region takes this branch.
	if( radius < 0.5 )
	{
		fragColor = fetch( uv );
		return;
	}

	//The centre tap is unconditional: its own distance is zero, so it always
	//reaches itself, and it guarantees the weight sum can never be zero.
	vec4 sum = fetch( uv );
	float weightSum = 1.0;

	float step = radius / float( Taps );

	for( int i = 1; i <= Taps; ++i )
	{
		float distance = step * float( i );
		vec2 offset = Direction * distance * pixelToUV;

		//Three sigma at the far tap, so the kernel has actually decayed by the
		//time it is truncated rather than being chopped off mid-slope.
		float t = float( i ) / float( Taps );
		float gaussian = exp( -0.5 * ( t * 3.0 ) * ( t * 3.0 ) );

		//Two taps, one either side, each tested against its own CoC.
		for( int side = 0; side < 2; ++side )
		{
			vec2 at = side == 0 ? uv + offset : uv - offset;

			//Does this tap's circle of confusion actually reach us? The half
			//pixel of softness stops the test itself becoming a visible edge.
			float reach = clamp( ( radiusAt( at ) - distance ) * 0.5 + 0.5, 0.0, 1.0 );
			float w = gaussian * reach;

			sum += fetch( at ) * w;
			weightSum += w;
		}
	}

	fragColor = sum / weightSum;
}
)";
} // namespace tilter::shaders
