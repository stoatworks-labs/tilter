#include "../Shaders.h"

namespace tilter::shaders
{
/*
    The circle of confusion field.

    This is the ONLY mirrored file in the repo: every block marked `//= mirrored`
    below has a twin in Focus.cpp, and `tiltest --focus` compares the two across
    all four geometries. Change one, change the other, run the test.

    --------------------------------------------------------- the v axis

    Focus.cpp works in picture space with **v running down**, because that is
    how an operator reads a frame and how the Focus Y control is labelled: 0 is
    the top. GL hands this shader a UV with **v running up**. The flip happens
    once, on the first line of main(), and nowhere else. Getting it wrong is
    invisible on a centred symmetrical band and wrong the moment anybody drags
    the focus off centre.
*/
const char* const kCoCFragment = R"(#version 410 core
uniform sampler2D DepthTexture;

uniform int Geometry;
uniform vec2 Centre;
uniform float Angle;
uniform float Width;
uniform float Feather;
uniform float EllipseAspect;
uniform float Horizon;
uniform float Tilt;
uniform float Rate;
uniform float DepthBias;
uniform float DepthContrast;
uniform float Invert;
uniform float FrameAspect;

in vec2 uv;
out vec4 fragColor;

//= mirrored: Focus.cpp bandCoords()
//`along` runs parallel to the band, `across` along its normal, both in units of
//FRAME HEIGHT. The aspect correction is on the u axis only, which is what keeps
//a circle circular and a band the same thickness when the composition changes
//shape.
void bandCoords( vec2 p, out float along, out float across )
{
	float px = ( p.x - Centre.x ) * FrameAspect;
	float py = ( p.y - Centre.y );

	float c = cos( Angle );
	float s = sin( Angle );

	along  =  px * c + py * s;
	across = -px * s + py * c;
}

//= mirrored: Focus.cpp shape()
float shape( float raw )
{
	float feather = max( Feather, 1e-4 );

	float magnitude = smoothstep( 0.0, 1.0, ( abs( raw ) - Width ) / feather );

	if( Invert > 0.5 )
		magnitude = 1.0 - magnitude;

	//The sign is which side of focus, and it survives inversion.
	return raw < 0.0 ? -magnitude : magnitude;
}

//= mirrored: Focus.cpp guessedDepth()
float guessedDepth( float luma, float detail )
{
	float far = 0.5 * clamp( luma, 0.0, 1.0 ) + 0.5 * ( 1.0 - clamp( detail, 0.0, 1.0 ) );
	return clamp( 0.5 + ( far - 0.5 ) * max( DepthContrast, 0.0 ), 0.0, 1.0 );
}

void main()
{
	//GL's v runs up, Focus.cpp's runs down. Once, here.
	vec2 p = vec2( uv.x, 1.0 - uv.y );

	float along;
	float across;
	bandCoords( p, along, across );

	//The depth guess is reported even when it is not driving anything, so the
	//focus overlay can show what the mode would do before it is switched on.
	vec2 cues = texture( DepthTexture, uv ).rg;
	float depth = guessedDepth( cues.r, cues.g );

	float signedDefocus;

	//= mirrored: Focus.cpp defocus()
	if( Geometry == 1 )
	{
		//Radial. Measured in the band-aligned frame, so Angle rotates the
		//ellipse and Aspect stretches it along its own axes.
		float a  = max( EllipseAspect, 1e-3 );
		float ry = across / a;
		//Always positive: outside an ellipse there is no near and far to sign.
		signedDefocus = shape( sqrt( along * along + ry * ry ) );
	}
	else if( Geometry == 2 )
	{
		//Tilted plane. Inverse depth of a plane under perspective is affine in
		//image position and has a FLOOR -- nothing is further than infinity --
		//and that floor is the whole point of this mode. Past the horizon every
		//pixel shares one blur; on the near side there is no limit.
		float sceneW = max( 0.0, across - Horizon );
		float focusW = max( 0.0, -Horizon );

		//Tilt swings the focal plane about the viewing axis, so the focused
		//depth varies ALONG the band too and the sharp zone converges.
		float lensW = focusW + Tilt * along;

		signedDefocus = shape( Rate * ( sceneW - lensW ) );
	}
	else if( Geometry == 3 )
	{
		//Image depth. Width and Feather are in units of guessed depth here, not
		//frame height -- the axis being measured is not a distance in the
		//picture.
		signedDefocus = shape( depth - DepthBias );
	}
	else
	{
		signedDefocus = shape( across );
	}

	fragColor = vec4( signedDefocus, depth, 0.0, 1.0 );
}
)";
} // namespace tilter::shaders
