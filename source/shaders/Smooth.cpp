#include "../Shaders.h"

namespace tilter::shaders
{
/*
    A fixed-radius separable blur, used only on the depth cues.

    Nothing to do with the lens. A depth field carrying per-pixel detail gives a
    blur radius that changes from pixel to pixel and frame to frame, and the
    result shimmers -- which reads as a broken plugin rather than as a shallow
    depth of field. Smoothing the cues is what makes the guess behave like a
    depth.
*/
const char* const kSmoothFragment = R"(#version 410 core
uniform sampler2D SourceTexture;
uniform vec2 Direction;

in vec2 uv;
out vec4 fragColor;

void main()
{
	//Nine taps, sigma about two of them. Wide enough to kill grain, narrow
	//enough that a real depth boundary -- a rooftop against sky -- does not
	//migrate far enough to put the blur on the wrong side of it.
	//
	//The source is one of our own buffers, so there is no MaxUV padding to
	//scale by and the clamp is the plain 0..1 edge.
	float weights[ 5 ] = float[ 5 ]( 0.2270270270, 0.1945945946, 0.1216216216, 0.0540540541, 0.0162162162 );

	vec4 sum = texture( SourceTexture, uv ) * weights[ 0 ];

	for( int i = 1; i < 5; ++i )
	{
		vec2 offset = Direction * float( i );
		sum += texture( SourceTexture, clamp( uv + offset, vec2( 0.0 ), vec2( 1.0 ) ) ) * weights[ i ];
		sum += texture( SourceTexture, clamp( uv - offset, vec2( 0.0 ), vec2( 1.0 ) ) ) * weights[ i ];
	}

	fragColor = sum;
}
)";
} // namespace tilter::shaders
