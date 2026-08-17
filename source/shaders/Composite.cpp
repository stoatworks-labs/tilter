#include "../Shaders.h"

namespace tilter::shaders
{
/*
    Everything after the blur: the grade, the vignette, the aberration, the
    mix, and the focus overlay.

    ------------------------------------------------- what applies where, and why

    Faking a miniature is two separate claims, and they are graded differently.

    **Saturation and contrast are global.** They are not optical -- nothing about
    a shallow depth of field makes colours stronger. They are here because the
    thing being imitated is a *photograph of a model*, which is a small brightly
    lit object photographed close up, and that is what small brightly lit
    objects look like. Applying them only where the picture is blurred would
    make the sharp band a different-looking photograph from the rest of the
    frame.

    **Aberration scales with defocus, and reverses across it.** That one IS
    optical: a real lens brings the three wavelengths to focus at slightly
    different distances, so the coloured fringing grows with how far from focus
    a point is, and it swaps sides as you cross the plane of focus. Hence the
    signed CoC -- the sign is the whole reason the field is signed rather than a
    magnitude.

    **The vignette is global**, because it belongs to the lens rather than to
    the focus.

    ------------------------------------------------------------ the overlay

    Show Focus paints the CoC field over a desaturated picture: the sharp band
    keeps its colour, everything else takes a tint that gets stronger with the
    blur, and the two sides of focus tint differently. It exists because setting
    up a tilted plane by looking at the blurred output means judging a small
    difference in softness by eye, which is miserable. It is also how the field
    is checked -- a mode that is wrong is obvious here and subtle everywhere
    else.
*/
const char* const kCompositeFragment = R"(#version 410 core
uniform sampler2D BlurredTexture;
uniform sampler2D InputTexture;
uniform sampler2D CoCTexture;

uniform vec2 SourceMaxUV;
uniform vec2 SourceHalfTexel;

uniform float Saturation;
uniform float Contrast;
uniform float Vignette;
uniform float Aberration;
uniform float Mix;
uniform float ShowFocus;
uniform float FrameAspect;
uniform float MaxRadius;//composition pixels at full defocus

in vec2 uv;
out vec4 fragColor;

vec4 fetchInput( vec2 p )
{
	vec2 t = p * SourceMaxUV;
	return texture( InputTexture, clamp( t, SourceHalfTexel, SourceMaxUV - SourceHalfTexel ) );
}

vec4 fetchBlurred( vec2 p )
{
	return texture( BlurredTexture, clamp( p, vec2( 0.0 ), vec2( 1.0 ) ) );
}

void main()
{
	float signedDefocus = texture( CoCTexture, uv ).r;
	float defocus = abs( signedDefocus );

	//Distance from the frame centre, aspect corrected, for both the aberration
	//direction and the vignette.
	vec2 fromCentre = vec2( ( uv.x - 0.5 ) * FrameAspect, uv.y - 0.5 );
	float radial = length( fromCentre );

	vec4 colour;

	if( Aberration > 0.0001 && defocus > 0.0001 )
	{
		//Lateral aberration: the three channels are magnified slightly
		//differently, so they are fetched at slightly different distances from
		//the frame centre. Signed, so the fringe swaps sides across focus.
		vec2 direction = radial > 0.0001 ? fromCentre / radial : vec2( 0.0 );
		vec2 shift = direction * ( Aberration * signedDefocus * 0.01 );

		vec4 mid = fetchBlurred( uv );
		colour.r = fetchBlurred( uv + shift ).r;
		colour.g = mid.g;
		colour.b = fetchBlurred( uv - shift ).b;
		colour.a = mid.a;
	}
	else
	{
		colour = fetchBlurred( uv );
	}

	/*
	    Bring the sharp picture back where the lens is sharp.

	    The blurred texture is a LOWER RESOLUTION buffer (Downsample.cpp), so
	    taking it everywhere would cost the in-focus band the very sharpness the
	    plugin exists to preserve. Instead the two are blended by how big the
	    circle of confusion actually is: under about a pixel there is nothing for
	    a blur to do and the untouched full-resolution input is exactly right,
	    and by two pixels the blurred copy has taken over completely.

	    The threshold is in pixels rather than in normalised defocus on purpose.
	    "The blur is smaller than a pixel" is the honest test for whether a blur
	    is visible at all, and it holds at any resolution and any Blur setting,
	    where a fixed defocus threshold would not.
	*/
	float radiusPixels = defocus * MaxRadius;
	colour = mix( fetchInput( uv ), colour, smoothstep( 0.5, 2.0, radiusPixels ) );

	//Grade in straight alpha. Contrast on a premultiplied pixel pushes the
	//colour and the coverage in the same direction and turns every soft edge
	//into a hard one.
	float alpha = colour.a;
	vec3 straight = alpha > 0.001 ? colour.rgb / alpha : colour.rgb;

	float luma = dot( straight, vec3( 0.2126, 0.7152, 0.0722 ) );
	straight = mix( vec3( luma ), straight, Saturation );
	straight = ( straight - 0.5 ) * Contrast + 0.5;

	if( Vignette > 0.0001 )
	{
		//Falls off from about the middle of the frame outward. The 0.7 is
		//roughly the radius of a 16:9 frame's corner, so a Vignette of 1 puts
		//the corner at zero and the centre untouched.
		float v = smoothstep( 0.7, 0.25, radial );
		straight *= mix( 1.0, v, Vignette );
	}

	straight = max( straight, vec3( 0.0 ) );
	colour = vec4( straight * alpha, alpha );

	if( ShowFocus > 0.5 )
	{
		//Sharp keeps its colour; blurred goes grey and takes a tint that says
		//which side of focus it fell on. Warm for the near side, cold for the
		//far side -- the same way a photographer would think about it.
		//
		//POSITIVE is the near side, and the sign is not arbitrary. In Tilted
		//Plane the raw quantity is scene inverse depth minus lens inverse
		//depth, and inverse depth grows as things get closer -- so positive
		//means the scene is nearer than the plane of focus. Linear Band agrees
		//by construction: positive is below the band, which is the near end of
		//a frame shot looking down. Radial has no near and far at all and is
		//always positive, so its single tint carries no meaning; that is fine,
		//it only has to be uniform.
		//
		//This was the wrong way round until the contact sheet showed the top of
		//a downward-looking frame -- the horizon, the furthest thing in shot --
		//painted with the near-side colour. Every automated check passed with
		//it inverted, because the field itself was correct and only the label
		//on it was not.
		vec3 tint = signedDefocus > 0.0 ? vec3( 1.0, 0.35, 0.15 ) : vec3( 0.15, 0.5, 1.0 );
		float grey = dot( straight, vec3( 0.2126, 0.7152, 0.0722 ) );
		vec3 marked = mix( straight, mix( vec3( grey ), tint, 0.6 ), defocus );
		colour = vec4( marked * alpha, alpha );
	}

	//Wet/dry against the untouched input. Last, so it undoes everything above
	//it rather than some of it.
	fragColor = mix( fetchInput( uv ), colour, Mix );
}
)";
} // namespace tilter::shaders
