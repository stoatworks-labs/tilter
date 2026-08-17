#include "../Shaders.h"

namespace tilter::shaders
{
/*
    The two cues the depth guess is built from.

    Runs at quarter resolution, and not only to be cheap: the question is "how
    hazy is this region", which is a property of a region rather than of a
    pixel. Asked at full resolution the answer is dominated by film grain and
    compression noise.

    Only ever run for the Image Depth geometry. The other three never read it.
*/
const char* const kDepthFragment = R"(#version 410 core
uniform sampler2D InputTexture;
uniform vec2 SourceMaxUV;
uniform vec2 SourceHalfTexel;
uniform vec2 RingStep;

in vec2 uv;
out vec4 fragColor;

//The input can be bigger than the picture -- MaxUV is the fraction of it that
//was really drawn. Geometry is done in picture space and the scaling happens
//here, at the fetch, and the clamp stays half a texel inside because GL_LINEAR
//at the very edge takes half its weight from undrawn padding.
vec4 fetch( vec2 p )
{
	vec2 t = p * SourceMaxUV;
	return texture( InputTexture, clamp( t, SourceHalfTexel, SourceMaxUV - SourceHalfTexel ) );
}

float lumaAt( vec2 p )
{
	vec4 c = fetch( p );
	//The host hands over premultiplied alpha. Undo it before measuring
	//brightness, or every semi-transparent region reads as dark and therefore
	//as near.
	vec3 straight = c.a > 0.001 ? c.rgb / c.a : c.rgb;
	return dot( straight, vec3( 0.2126, 0.7152, 0.0722 ) );
}

void main()
{
	float centre = lumaAt( uv );

	//Local contrast as the range over a ring, not an edge detector. "Is there a
	//line here" is a different question: a hazy distant hillside has a smooth
	//gradient with no edges in it and plenty of range, and it is the range that
	//carries the distance.
	float lo = centre;
	float hi = centre;

	for( int i = 0; i < 8; ++i )
	{
		float a = float( i ) * 0.7853981634;//2*pi/8
		vec2 offset = vec2( cos( a ), sin( a ) ) * RingStep;
		float l = lumaAt( uv + offset );
		lo = min( lo, l );
		hi = max( hi, l );
	}

	//Scaled so a quarter of full swing already counts as fully detailed. Real
	//footage almost never puts a 0..1 range inside one small neighbourhood, and
	//unscaled the detail cue sits near zero everywhere and stops contributing.
	float detail = clamp( ( hi - lo ) * 4.0, 0.0, 1.0 );

	fragColor = vec4( centre, detail, 0.0, 1.0 );
}
)";
} // namespace tilter::shaders
