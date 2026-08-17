#include "../Shaders.h"

namespace tilter::shaders
{
/*
    A box downsample in front of the blur. The prefilter.

    ------------------------------------------------------------------ why

    A gather blur is a discrete sum: N taps, spaced radius/N apart. That is an
    estimate of an integral, and like any sparse sampling it ALIASES anything in
    the source finer than its spacing -- folding those frequencies down into
    coarse ones that were never in the picture.

    It bites hardest exactly where it is least expected: at LARGE radii. A
    one-pixel line has energy at every harmonic up to Nyquist, and the taps get
    further apart as the radius grows, so the aliased residue gets stronger the
    more the picture is supposed to be smoothed. `tiltest --blur` measured
    whole-frame detail at uniform full defocus going 3.24, 6.46, 4.26, 11.32 as
    Blur went 0.35, 0.50, 0.70, 1.00 -- rising, and not even monotonically,
    because it is a beat between the tap spacing and the scene's own pitch. On
    screen it is a fine stipple lying over a picture that should be perfectly
    smooth.

    Raising the tap count does not fix it. The radius here reaches eight per
    cent of the frame height, which is 173 pixels on a 4K composition, and no
    affordable budget keeps sub-pixel spacing over that.

    The fix is to remove the frequencies before sampling rather than to sample
    faster. This averages every texel of each block -- a real box prefilter,
    not a bilinear tap that reads two of the sixteen and calls it an average --
    so the blur that follows works on a picture that no longer contains anything
    its taps could alias.

    The second prize is speed: the blur then runs over a quarter or a sixteenth
    of the pixels, which is what makes a very large radius affordable at all.

    -------------------------------------------------------------- the cost

    The blur output is correspondingly lower resolution and is bilinearly
    upsampled by the composite. That is invisible -- it is the *blurred* half of
    the picture, and a blur is the one operation whose output has no detail to
    lose. The sharp half never passes through here: the composite blends toward
    the untouched full-resolution input wherever the circle of confusion is
    smaller than a pixel or so.
*/
const char* const kDownsampleFragment = R"(#version 410 core
uniform sampler2D InputTexture;
uniform vec2 SourceMaxUV;
uniform vec2 SourceHalfTexel;
uniform vec2 SourceTexel;//one full-resolution texel, in picture space
uniform int Scale;       //block edge, in full-resolution texels

in vec2 uv;
out vec4 fragColor;

vec4 fetch( vec2 p )
{
	vec2 t = p * SourceMaxUV;
	return texture( InputTexture, clamp( t, SourceHalfTexel, SourceMaxUV - SourceHalfTexel ) );
}

void main()
{
	if( Scale <= 1 )
	{
		fragColor = fetch( uv );
		return;
	}

	//Every texel of the block, weighted equally. The block's top-left corner in
	//picture space is the output texel's centre minus half a block.
	vec2 origin = uv - SourceTexel * ( float( Scale ) * 0.5 - 0.5 );

	vec4 sum = vec4( 0.0 );
	for( int y = 0; y < Scale; ++y )
	{
		for( int x = 0; x < Scale; ++x )
		{
			sum += fetch( origin + SourceTexel * vec2( float( x ), float( y ) ) );
		}
	}

	fragColor = sum / float( Scale * Scale );
}
)";
} // namespace tilter::shaders
