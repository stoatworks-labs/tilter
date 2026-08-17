#pragma once

namespace tilter
{
/**
    Where the picture is sharp, and by how much it is not.

    ------------------------------------------------------------- the one idea

    **The plugin decides how far each pixel is from focus. The blur decides what
    that looks like.** Those two are completely independent, which is why there
    are four focus geometries and two blur models rather than eight effects: a
    geometry produces one number per pixel and a blur consumes it, and neither
    knows anything about the other.

    That number is the *signed normalised defocus*: 0 exactly at the plane of
    focus, +/-1 at full blur, and the sign says which side of focus the pixel
    fell on. The sign is not decoration -- chromatic aberration reverses across
    focus on a real lens, and the near and far sides of a real lens do not blur
    at the same rate.

    ------------------------------------------------------------- the shaping

    All four geometries produce a *raw* signed distance from focus, in their own
    natural units, and then one shared function turns that into the normalised
    value above by applying the sharp zone's half-width and its feather. So
    "Focus Width" and "Feather" mean the same thing in every mode and there is
    one ramp to get wrong instead of four.

    ------------------------------------------- what Tilted Plane actually buys

    Worth knowing before reaching for it, because the obvious expectation is
    wrong. For a **planar** subject -- a street, a table top, anything flat --
    perspective maps inverse depth to an affine function of image position, so
    the in-focus region of a real tilted lens is *exactly a straight band*. Mode
    1 already draws that. Tilted Plane is not a differently-shaped band.

    What it buys is the **falloff**, which is the half people get wrong when
    they fake this. A real lens blurs by an amount proportional to the
    difference in *inverse* depth, and inverse depth has a floor: nothing is
    further away than infinity. So on the far side the blur ramps up and then
    **stops** at the horizon, and everything past it -- sky, distant hills, the
    backs of buildings -- shares one blur. On the near side there is no such
    limit and it grows without bound. A linear band ramps symmetrically and
    forever in both directions, which is the tell.

    ------------------------------------------------------------ Image Depth

    Mode 4 guesses depth from the picture's own content: haze lowers contrast
    and lifts luminance with distance, so smooth bright regions are treated as
    far and detailed dark ones as near. This is a **heuristic with no ground
    truth**. It is right often enough to be useful on landscape and cityscape
    footage and it is wrong on anything with a bright foreground or a dark sky.
    It is labelled as a guess in the UI for that reason, and unlike the other
    three there is nothing it can be verified against beyond looking at it.
*/
namespace focus
{
/// The four ways of deciding what is in focus. The order is the order of the
/// host-facing dropdown, and a saved composition stores the index, so append
/// only.
enum Geometry
{
	kLinearBand = 0,
	kRadial,
	kTiltedPlane,
	kImageDepth,
	kGeometryCount
};

/// Everything the field needs, in the units it works in.
///
/// Distances are in **units of frame height**, not in normalised u or v. A
/// width of 0.25 is a quarter of the frame's height whatever the aspect ratio
/// is, so a band keeps its thickness when the composition goes from 16:9 to
/// 4:3 and a circle stays a circle. Getting this wrong is invisible on a square
/// test render and wrong on every real output -- see AGENTS.md.
struct Field
{
	int geometry = kLinearBand;

	/// Centre of the sharp zone, in picture space: 0..1 across each axis, v
	/// down. Not aspect-corrected -- this is a position, not a distance.
	float centreU = 0.5f;
	float centreV = 0.5f;

	/// Rotation of the band, the ellipse, or the horizon. Radians, 0 =
	/// horizontal band (the classic tilt-shift framing).
	float angle = 0.0f;

	/// Half-width of the fully sharp zone, and the distance over which it ramps
	/// to full blur. Frame-height units.
	float width = 0.12f;
	float feather = 0.25f;

	/// Radial only: the ellipse's y/x ratio. 1 is a circle.
	float aspect = 1.0f;

	/// Tilted Plane only.
	///
	/// `horizon` is the signed **across** coordinate of the horizon line --
	/// measured from the focus centre along the band normal, in frame-height
	/// units, negative for a horizon above the sharp zone. Expressed that way
	/// rather than as a picture-space v so that it rotates with Angle instead
	/// of staying stubbornly horizontal when the band does not.
	float horizon = -0.2f;
	float tilt = 0.0f;///< Swings the focal plane about the viewing axis, so the sharp zone converges.
	float rate = 1.0f;///< How fast blur grows per unit of inverse depth. The aperture, in effect.

	/// Image Depth only.
	float depthBias = 0.5f;    ///< Which guessed depth is in focus.
	float depthContrast = 1.0f;///< How hard the guess separates near from far.

	/// Swap sharp and blurred.
	bool invert = false;

	/// Frame width / height. Only used to make the distance units isotropic.
	float aspectRatio = 16.0f / 9.0f;
};

/// Shape a raw signed distance from focus into the signed normalised defocus
/// the blur consumes. Shared by all four geometries; see the header comment.
float shape( const Field& field, float raw );

/// The signed normalised defocus at one point, -1..1.
///
/// `u`, `v` are picture space, v down. `depthLuma` and `depthDetail` are the
/// Image Depth pre-pass's two channels -- smoothed luminance and smoothed local
/// contrast, both 0..1. The other three geometries ignore them, so anything may
/// be passed when they are not in use.
float defocus( const Field& field, float u, float v, float depthLuma, float depthDetail );

/// The guessed inverse depth, 0 (near) to 1 (far), from the pre-pass channels.
/// Split out because the Image Depth mode's whole honesty rests on this one
/// function and it deserves to be testable on its own.
float guessedDepth( const Field& field, float luma, float detail );

/// Human-readable names for the dropdown. Index with a Geometry.
const char* geometryName( int geometry );

} // namespace focus
} // namespace tilter
