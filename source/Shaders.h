#pragma once

/**
    The GLSL for each stage of the chain.

    Tilter is a lens, and the chain is the order a lens does things in: decide
    where the focal plane is, then fail to resolve everything that is not on it,
    then let the rest of the optics have their say.

    The stages, in order:

      Depth      Image Depth mode only. Luminance and local contrast at quarter
                 resolution -- the two cues the depth guess is built from.
      Smooth     A fixed-radius separable blur over that, because a depth field
                 with per-pixel detail in it produces a blur that shimmers, and
                 shimmer reads as a bug rather than as an effect.
      CoC        The circle of confusion: one signed number per pixel saying how
                 far from focus it is and on which side. The GLSL mirror of
                 Focus.cpp, and the only file in the repo that has a mirror.
      Gaussian   Separable blur, two passes, radius driven per pixel by CoC.
      Bokeh      Single-pass disc gather with a polygonal aperture. The
                 alternative to Gaussian, not a stage after it.
      Composite  The grade, the vignette, the aberration, the mix, and the
                 focus overlay.

    ------------------------------------------------------------ the mirror

    `CoC.cpp` carries the same arithmetic as `Focus.cpp`. It has to: the field
    is a function of every pixel, so the GPU has to evaluate it, and the OFX
    build and the harness need the C++ one. Every mirrored block is marked
    `//= mirrored` in both files, and `tiltest --focus` compares them across all
    four geometries at a few thousand points. That test is the only thing
    standing between the two copies and a silent drift, so if you change one,
    run it.

    Nothing else here is mirrored. The blurs are pure GPU, and the OFX build
    reimplements them on the CPU as a separate acknowledged copy.
*/
namespace tilter::shaders
{
/// Shared by every pass: draws the screen quad and scales UVs by MaxUV so the
/// same program works against a host texture with padding and against our own
/// framebuffers, which have none.
///
/// In this plugin MaxUV is ALWAYS set to 1 here and the scaling is done at each
/// fetch instead, because several passes read two textures with different
/// padding at once and a single vertex-stage scale cannot serve both.
extern const char* const kVertex;

extern const char* const kDepthFragment;
extern const char* const kSmoothFragment;
extern const char* const kCoCFragment;
extern const char* const kGaussianFragment;
extern const char* const kBokehFragment;
extern const char* const kCompositeFragment;

} // namespace tilter::shaders
