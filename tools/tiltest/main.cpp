/**
    tiltest -- the offline harness.

    It drives **the real plugin class** through the real FFGL sequence in a
    headless core-profile context. Not a reimplementation of the lens and not a
    preview: the thing under test is `Tilter`, compiled from the same objects
    that go into the bundle, and every number below comes out of a frame it
    actually rendered.

        --out PATH        render a frame
        --scene PATH      write the synthetic test scene
        --list            parameters, with their types and defaults
        --set "Name=v"    set any parameter by its host-facing name
        --focus           the CoC field against Focus.cpp, all four geometries
        --blur            the blur actually blurs, and only where it should
        --aperture        the aperture shape reaches the picture
        --presets         every factory preset is distinct and non-degenerate
        --hosts           presets survive every host behaviour
        --sheet PATH      a contact sheet of every mode

    ## The synthetic scene

    Built to make blur *measurable* rather than to look nice. It carries, at
    every height in the frame:

    - **a fine line grid**, which is the highest frequency the scene holds and
      therefore the thing whose disappearance measures a blur;
    - **small clipped highlights**, because a bokeh disc only forms around a
      point bright enough to dominate a weighted mean, and a scene of mid-greys
      would let a broken highlight weighting pass;
    - **a smooth vertical gradient**, bright and detail-free at the top, so the
      Image Depth guess has something it should read as far away.

    ## What each test can and cannot catch

    `--focus` is the only thing standing between `Focus.cpp` and its GLSL mirror
    in `CoC.cpp`. It reads the CoC buffer the GPU actually wrote and compares it
    against the C++ at a few thousand points, per geometry. **It carries its own
    control**: the same comparison against a deliberately wrong geometry, which
    must FAIL. Rows of agreement are exactly when to ask whether the test can
    fail at all.

    `--blur` measures local detail inside the sharp band and far outside it. It
    catches a blur that does nothing, a blur that blurs everything, and a focus
    field wired to the blur backwards. It cannot catch a wrong *shape* of blur.

    `--aperture` is the one that can: it puts a single highlight in a fully
    defocused region and measures the lit area against the radius it should
    have, circular versus stopped down. A blade count that never reached the
    shader shows up here and nowhere else.

    `--presets` catches the degenerate ones -- a preset that renders black, or
    that is identical to another, or that does nothing at all.

    None of them catches a dead control. See `tools/sweep.py`.
*/

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "Controls.h"
#include "Focus.h"
#include "Presets.h"
#include "Tilter.h"

using namespace tilter;

namespace
{
//---------------------------------------------------------------------------
// PNG. zlib ships with the OS, so this is fifty lines rather than a dependency.
//---------------------------------------------------------------------------
void putBigEndian( std::vector< unsigned char >& out, unsigned int value )
{
	out.push_back( static_cast< unsigned char >( ( value >> 24 ) & 0xff ) );
	out.push_back( static_cast< unsigned char >( ( value >> 16 ) & 0xff ) );
	out.push_back( static_cast< unsigned char >( ( value >> 8 ) & 0xff ) );
	out.push_back( static_cast< unsigned char >( value & 0xff ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type,
               const unsigned char* data, size_t length )
{
	putBigEndian( out, static_cast< unsigned int >( length ) );
	const size_t crcStart = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data, data + length );
	const unsigned long crc = crc32( 0, out.data() + crcStart,
	                                 static_cast< unsigned int >( out.size() - crcStart ) );
	putBigEndian( out, static_cast< unsigned int >( crc ) );
}

bool writePng( const std::string& path, int width, int height,
               const std::vector< unsigned char >& rgba )
{
	//Each scanline gets a filter byte. Filter 0 (none) throughout: this is a
	//test artefact, not a delivery format, and a filter would only make the
	//file smaller at the cost of being wrong in a way nothing here would catch.
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(),
	               static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };

	std::vector< unsigned char > ihdr;
	putBigEndian( ihdr, static_cast< unsigned int >( width ) );
	putBigEndian( ihdr, static_cast< unsigned int >( height ) );
	ihdr.push_back( 8 );//bit depth
	ihdr.push_back( 6 );//truecolour with alpha
	ihdr.push_back( 0 );//deflate
	ihdr.push_back( 0 );//adaptive filtering
	ihdr.push_back( 0 );//no interlace
	putChunk( png, "IHDR", ihdr.data(), ihdr.size() );
	putChunk( png, "IDAT", compressed.data(), compressed.size() );
	putChunk( png, "IEND", nullptr, 0 );

	FILE* file = std::fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = std::fwrite( png.data(), 1, png.size(), file );
	std::fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// The synthetic scene.
//---------------------------------------------------------------------------
struct Rgba
{
	float r, g, b, a;
};

/// One scene pixel, in frame coordinates (0..1, y DOWN). Straight alpha.
///
/// Deliberately a pure function of position so a test can predict what should
/// be where without carrying a copy of the image around.
Rgba scenePixel( float fx, float fy, int width, int height )
{
	//A smooth vertical gradient: bright and flat at the top, darker and busier
	//at the bottom. That is the shape aerial perspective actually has, so the
	//Image Depth guess should read the top as far away -- and if it does not,
	//that is a real finding rather than a badly built scene.
	const float sky = 1.0f - fy;
	float r = 0.32f + 0.55f * sky * sky;
	float g = 0.36f + 0.52f * sky * sky;
	float b = 0.44f + 0.50f * sky;

	const int px = static_cast< int >( fx * static_cast< float >( width ) );
	const int py = static_cast< int >( fy * static_cast< float >( height ) );

	//A one-pixel line grid. The highest frequency the scene holds, and
	//therefore the thing whose disappearance measures a blur. Spaced 8 px so
	//that even a small blur radius crosses several of them.
	if( px % 8 == 0 || py % 8 == 0 )
	{
		r = std::min( 1.0f, r + 0.45f );
		g = std::min( 1.0f, g + 0.45f );
		b = std::min( 1.0f, b + 0.45f );
	}

	//Dark blocks, so the grid has something to sit against and the local
	//contrast measure has a floor as well as a ceiling.
	if( ( px / 32 + py / 32 ) % 2 == 0 && px % 8 != 0 && py % 8 != 0 )
	{
		r *= 0.35f;
		g *= 0.35f;
		b *= 0.38f;
	}

	//Clipped highlights on a coarse lattice. A bokeh disc only forms around a
	//point bright enough to dominate a weighted mean; a scene of mid-greys
	//would let a broken highlight weighting pass every test in this file.
	const int hx = px % 64;
	const int hy = py % 64;
	if( hx >= 30 && hx <= 32 && hy >= 30 && hy <= 32 )
	{
		r = g = b = 1.0f;
	}

	return { r, g, b, 1.0f };
}

std::vector< unsigned char > makeScene( int width, int height )
{
	std::vector< unsigned char > rgba( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			const float fx = ( static_cast< float >( x ) + 0.5f ) / static_cast< float >( width );
			const float fy = ( static_cast< float >( y ) + 0.5f ) / static_cast< float >( height );
			const Rgba c   = scenePixel( fx, fy, width, height );

			unsigned char* p = rgba.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			//Premultiplied on upload, because that is what a host hands over.
			p[ 0 ] = static_cast< unsigned char >( std::lround( c.r * c.a * 255.0f ) );
			p[ 1 ] = static_cast< unsigned char >( std::lround( c.g * c.a * 255.0f ) );
			p[ 2 ] = static_cast< unsigned char >( std::lround( c.b * c.a * 255.0f ) );
			p[ 3 ] = static_cast< unsigned char >( std::lround( c.a * 255.0f ) );
		}
	}
	return rgba;
}

/**
    One clipped highlight in the middle of an otherwise flat dark field.
    --aperture needs this: the shape of a bokeh disc is only legible when there
    is exactly one of them.

    ------------------------------------------------- why the highlight is 11px

    It started at 3px and every aperture measurement came back empty, which
    looked like a broken shader and is not. A gather blur estimates each output
    pixel by taking N samples of the disc around it, and a 3px highlight inside
    a 19px-radius disc covers under one per cent of that area -- so with 96
    samples, most output pixels inside the disc miss the highlight entirely and
    the few that hit it produce single bright specks. The disc never forms
    because the estimator's variance is larger than the thing being estimated.

    That is a real property of gather-based defocus, not a defect in this one:
    rendering a true point highlight as a clean disc needs a SCATTER pass, which
    draws a sprite per bright pixel, and that is a different architecture.

    So the probe is sized to what this test is actually for -- does the blade
    count reach the picture and does the radius control the spread -- rather
    than to the hardest input the method has. 11px is a realistic specular
    highlight and the limit around genuinely tiny ones is written down in
    AGENTS.md instead of being hidden behind a test that was quietly measuring
    something else.
*/
std::vector< unsigned char > makePointScene( int width, int height )
{
	std::vector< unsigned char > rgba( static_cast< size_t >( width ) * height * 4, 0 );
	for( size_t i = 0; i < rgba.size(); i += 4 )
	{
		rgba[ i + 0 ] = 8;
		rgba[ i + 1 ] = 8;
		rgba[ i + 2 ] = 10;
		rgba[ i + 3 ] = 255;
	}

	const int cx = width / 2;
	const int cy = height / 2;
	for( int y = cy - 5; y <= cy + 5; ++y )
	{
		for( int x = cx - 5; x <= cx + 5; ++x )
		{
			unsigned char* p = rgba.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			p[ 0 ] = p[ 1 ] = p[ 2 ] = p[ 3 ] = 255;
		}
	}
	return rgba;
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

struct Target
{
	GLuint texture = 0;
	GLuint fbo     = 0;
	int width      = 0;
	int height     = 0;
};

Target makeTarget( int width, int height )
{
	Target target;
	target.width  = width;
	target.height = height;

	glGenTextures( 1, &target.texture );
	glBindTexture( GL_TEXTURE_2D, target.texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glBindTexture( GL_TEXTURE_2D, 0 );

	glGenFramebuffers( 1, &target.fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.texture, 0 );
	return target;
}

void releaseTarget( Target& target )
{
	if( target.fbo != 0 )
		glDeleteFramebuffers( 1, &target.fbo );
	if( target.texture != 0 )
		glDeleteTextures( 1, &target.texture );
	target = Target();
}

GLuint uploadTexture( const std::vector< unsigned char >& rgba, int width, int height )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data() );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

/// Straight out of GL, **bottom row first**. Every sampler below takes frame
/// coordinates with y down and flips here, in one place.
std::vector< unsigned char > readBytes( const Target& target )
{
	std::vector< unsigned char > pixels( static_cast< size_t >( target.width ) * target.height * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, target.width, target.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
	return pixels;
}

std::vector< unsigned char > flipRows( const std::vector< unsigned char >& image, int width, int height )
{
	std::vector< unsigned char > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( flipped.data() + static_cast< size_t >( y ) * stride,
		             image.data() + static_cast< size_t >( height - 1 - y ) * stride, stride );
	return flipped;
}

/// Read a float texture back whole. Used for the CoC and depth buffers, which
/// are RGBA16F -- glGetTexImage converts to float for us.
std::vector< float > readFloatTexture( GLuint texture, int width, int height )
{
	std::vector< float > pixels( static_cast< size_t >( width ) * height * 4 );
	glBindTexture( GL_TEXTURE_2D, texture );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glGetTexImage( GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, pixels.data() );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return pixels;
}

/**
    Bilinear fetch from a float texture read, matching what GL does.

    Needed because `--focus` compares the CoC the GPU wrote against the C++, and
    in Image Depth mode the shader reads its two cues out of a quarter-
    resolution buffer through a **GL_LINEAR** sampler (FFGLFBO sets that on
    every colour texture it makes). Sampling that buffer with a nearest lookup
    on this side put a 0.4 disagreement into a comparison whose whole job is
    detecting disagreements of about 0.002 -- entirely from the interpolation,
    with nothing wrong in either copy of the arithmetic.

    The half-texel offsets are the part worth keeping: `texture()` puts texel
    centres at (i + 0.5) / size, so the sample position in texel units is
    uv * size - 0.5, and dropping that shifts everything by half a texel of the
    reduced buffer, which is two full-resolution pixels.
*/
void bilinearRG( const std::vector< float >& texture, int width, int height,
                 float u, float v, float& outR, float& outG )
{
	const float fx = u * static_cast< float >( width ) - 0.5f;
	const float fy = v * static_cast< float >( height ) - 0.5f;

	const int x0 = static_cast< int >( std::floor( fx ) );
	const int y0 = static_cast< int >( std::floor( fy ) );
	const float tx = fx - static_cast< float >( x0 );
	const float ty = fy - static_cast< float >( y0 );

	auto texel = [ & ]( int x, int y, int channel ) {
		const int cx = std::clamp( x, 0, width - 1 );
		const int cy = std::clamp( y, 0, height - 1 );
		return texture[ ( static_cast< size_t >( cy ) * width + cx ) * 4 + channel ];
	};

	for( int channel = 0; channel < 2; ++channel )
	{
		const float top    = texel( x0, y0, channel ) * ( 1.0f - tx ) + texel( x0 + 1, y0, channel ) * tx;
		const float bottom = texel( x0, y0 + 1, channel ) * ( 1.0f - tx ) + texel( x0 + 1, y0 + 1, channel ) * tx;
		const float value  = top * ( 1.0f - ty ) + bottom * ty;
		( channel == 0 ? outR : outG ) = value;
	}
}

/// One pixel of a bottom-up RGBA8 read, in frame coordinates (0..1, y down),
/// un-premultiplied.
Rgba samplePixel( const std::vector< unsigned char >& bottomUp, int width, int height,
                  float fx, float fy )
{
	const int x     = std::clamp( static_cast< int >( fx * static_cast< float >( width ) ), 0, width - 1 );
	const int yDown = std::clamp( static_cast< int >( fy * static_cast< float >( height ) ), 0, height - 1 );
	const int y     = height - 1 - yDown;

	const unsigned char* p = bottomUp.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
	const float a          = static_cast< float >( p[ 3 ] ) / 255.0f;
	if( a <= 0.001f )
		return { 0.0f, 0.0f, 0.0f, 0.0f };

	return { static_cast< float >( p[ 0 ] ) / 255.0f / a,
	         static_cast< float >( p[ 1 ] ) / 255.0f / a,
	         static_cast< float >( p[ 2 ] ) / 255.0f / a,
	         a };
}

float lumaOf( const Rgba& c )
{
	return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
}

/**
    Local detail in a band of rows, as the mean absolute difference between
    horizontally adjacent pixels.

    That measure and not a variance: a variance over a region also counts the
    scene's own large-scale gradient, which a blur does not remove, so a
    perfectly working blur would only drop it a little and the threshold would
    have to be set so loose that a broken blur passed too. Adjacent-pixel
    difference sees only the frequencies a blur actually attacks.
*/
float detailInRows( const std::vector< unsigned char >& bottomUp, int width, int height,
                    float fromY, float toY )
{
    const int y0 = std::clamp( static_cast< int >( fromY * static_cast< float >( height ) ), 0, height - 1 );
    const int y1 = std::clamp( static_cast< int >( toY * static_cast< float >( height ) ), 0, height - 1 );

	double total = 0.0;
	long count   = 0;

	for( int yDown = std::min( y0, y1 ); yDown <= std::max( y0, y1 ); ++yDown )
	{
		const int y = height - 1 - yDown;
		for( int x = 1; x < width; ++x )
		{
			const unsigned char* a = bottomUp.data() + ( static_cast< size_t >( y ) * width + x - 1 ) * 4;
			const unsigned char* b = bottomUp.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			for( int c = 0; c < 3; ++c )
				total += std::fabs( static_cast< double >( a[ c ] ) - static_cast< double >( b[ c ] ) );
			count += 3;
		}
	}

	return count > 0 ? static_cast< float >( total / static_cast< double >( count ) ) : 0.0f;
}

//---------------------------------------------------------------------------
// Driving the plugin.
//---------------------------------------------------------------------------
bool render( Tilter& plugin, const Target& target, GLuint input, int inputWidth, int inputHeight )
{
	FFGLViewportStruct viewport {};
	viewport.width  = static_cast< FFUInt32 >( target.width );
	viewport.height = static_cast< FFUInt32 >( target.height );

	FFGLTextureStruct inputStruct {};
	inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( inputWidth );
	inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( inputHeight );
	inputStruct.Handle                              = input;
	FFGLTextureStruct* inputs[ 1 ]                  = { &inputStruct };

	ProcessOpenGLStruct process {};
	process.numInputTextures = 1;
	process.inputTextures    = inputs;
	process.HostFBO          = target.fbo;

	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glViewport( 0, 0, target.width, target.height );
	glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	glClear( GL_COLOR_BUFFER_BIT );

	//The plugin reads its size out of the viewport its base class holds, and
	//InitGL is what sets it. Calling it per frame is what lets one harness
	//process render at several sizes without tearing the GL resources down.
	plugin.InitGL( &viewport );
	return plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
}

/// Every parameter's host-facing name, read out of the plugin itself.
///
/// Built at runtime rather than kept as a table beside Controls.h, and that is
/// not tidiness: a hand-written table is a second place for a name to live, and
/// the failure it produces is a `--set` that silently addresses nothing while
/// everything else about the run looks correct.
std::map< std::string, unsigned int > parameterIndex( Tilter& plugin )
{
	std::map< std::string, unsigned int > byName;
	for( unsigned int id = 0; id < Tilter::PT_COUNT; ++id )
	{
		const char* name = plugin.GetParamName( id );
		if( name != nullptr && name[ 0 ] != '\0' )
			byName[ name ] = id;
	}
	return byName;
}

//---------------------------------------------------------------------------
// Parameter automation for --pipe.
//
// A plain text file of `frame  Parameter Name  value` lines. Values are held
// before the first key and after the last, and linearly interpolated between,
// so the piece is edited by editing the cue sheet rather than by editing code.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}

	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );

		int frame = 0;
		if( !( in >> frame ) )
			continue;//blank or comment

		//The name is everything up to the last token, because parameters have
		//spaces in them and the value never does.
		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}

		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];

		tracks[ name ].emplace_back( frame, value );
	}

	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;

	for( size_t i = 1; i < track.size(); ++i )
	{
		if( frame <= track[ i ].first )
		{
			const auto& a = track[ i - 1 ];
			const auto& b = track[ i ];
			const float span = static_cast< float >( b.first - a.first );
			const float t = span > 0.0f ? ( static_cast< float >( frame - a.first ) / span ) : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	}
	return track.back().second;
}

bool readExactly( void* into, size_t bytes )
{
	unsigned char* p = static_cast< unsigned char* >( into );
	size_t got = 0;
	while( got < bytes )
	{
		const size_t n = fread( p + got, 1, bytes - got, stdin );
		if( n == 0 )
			return false;//clean EOF, or a short final frame we cannot use
		got += n;
	}
	return true;
}

struct Options
{
	std::vector< std::pair< std::string, float > > sets;
	int width  = 640;
	int height = 360;
};

void applySets( Tilter& plugin, const Options& options )
{
	const auto byName = parameterIndex( plugin );
	for( const auto& set : options.sets )
	{
		const auto found = byName.find( set.first );
		if( found == byName.end() )
		{
			std::fprintf( stderr, "no parameter named \"%s\"\n", set.first.c_str() );
			continue;
		}
		plugin.SetFloatParameter( found->second, set.second );
	}
}

//---------------------------------------------------------------------------
// --list
//---------------------------------------------------------------------------
int listParameters()
{
	Tilter plugin;
	std::printf( "%-4s %-18s %-10s %s\n", "id", "name", "type", "default" );
	for( unsigned int id = 0; id < Tilter::PT_COUNT; ++id )
	{
		const char* name = plugin.GetParamName( id );
		if( name == nullptr || name[ 0 ] == '\0' )
			continue;

		const unsigned int type = plugin.GetParamType( id );
		const char* typeName    = "standard";
		if( type == FF_TYPE_BOOLEAN )
			typeName = "boolean";
		else if( type == FF_TYPE_TEXT )
			typeName = "text";
		else if( type == FF_TYPE_EVENT )
			typeName = "event";
		else if( type == FF_TYPE_OPTION )
			typeName = "option";

		std::printf( "%-4u %-18s %-10s %.4f\n", id, name, typeName, plugin.GetFloatParameter( id ) );
	}
	return 0;
}

//---------------------------------------------------------------------------
// --focus : the mirror test
//---------------------------------------------------------------------------

/// Compare the GPU's CoC field against Focus.cpp for one configuration.
/// `geometryOverride` is what the C++ side is asked for -- passing a different
/// geometry from the one the GPU rendered is how the control case works.
float compareField( Tilter& plugin, const Target& target, GLuint input,
                    int inputWidth, int inputHeight, int geometryOverride )
{
	if( !render( plugin, target, input, inputWidth, inputHeight ) )
		return -1.0f;

	const int w = target.width;
	const int h = target.height;

	const std::vector< float > coc = readFloatTexture( plugin.CoCTextureForTest(), w, h );

	focus::Field field = controls::field( plugin.hostValues(),
	                                      static_cast< float >( w ) / static_cast< float >( h ) );
	const bool needsDepth = field.geometry == focus::kImageDepth;
	field.geometry        = geometryOverride;

	//The depth pre-pass is GPU-only and has no C++ mirror, so its OUTPUT is
	//read back and fed into the C++ function. That keeps this comparison about
	//the geometry arithmetic, which is the part that is mirrored.
	std::vector< float > depth;
	int depthW = 0;
	int depthH = 0;
	if( needsDepth )
	{
		depthW = std::max( 1, w / 4 );
		depthH = std::max( 1, h / 4 );
		depth  = readFloatTexture( plugin.DepthTextureForTest(), depthW, depthH );
	}

	float worst = 0.0f;

	for( int y = 2; y < h - 2; y += 3 )
	{
		for( int x = 2; x < w - 2; x += 3 )
		{
			//The buffer is bottom-up like everything else out of GL; picture
			//space runs the other way.
			const float u = ( static_cast< float >( x ) + 0.5f ) / static_cast< float >( w );
			const float vUp = ( static_cast< float >( y ) + 0.5f ) / static_cast< float >( h );
			const float vDown = 1.0f - vUp;

			float luma   = 0.0f;
			float detail = 0.0f;
			if( needsDepth )
			{
				//Sampled exactly as the shader does: same uv, same bilinear
				//filter. See bilinearRG().
				bilinearRG( depth, depthW, depthH, u, vUp, luma, detail );
			}

			const float expected = focus::defocus( field, u, vDown, luma, detail );
			const float actual   = coc[ ( static_cast< size_t >( y ) * w + x ) * 4 ];

			worst = std::max( worst, std::fabs( expected - actual ) );
		}
	}

	return worst;
}

int checkFocus()
{
	const int w = 320;
	const int h = 180;

	Target target = makeTarget( w, h );
	const std::vector< unsigned char > sceneBytes = makeScene( w, h );
	const GLuint input = uploadTexture( sceneBytes, w, h );

	/*
	    The tolerance is DERIVED, not chosen.

	    Everything here round-trips through an RGBA16F buffer, and half
	    precision has an 11-bit mantissa, so a value near 0.5 quantises to
	    steps of 2^-11 = 0.000489. That is the floor, and it is exactly the
	    figure the first three geometries come out at -- which is itself the
	    evidence that they agree to the last bit the storage can hold.

	    Image Depth is the exception and needs a bigger number for a real
	    reason rather than because it would not pass otherwise. It reads its
	    two cues from a SECOND fp16 buffer, and the focus ramp then amplifies
	    that error: shape() is a smoothstep over the feather, whose steepest
	    slope is 1.5, so a depth wrong by one quantum comes out as a defocus
	    wrong by 1.5/feather quanta. Loosening the tolerance by exactly that
	    factor keeps the test as sharp as the arithmetic allows, and it means
	    a narrower feather automatically demands a tighter agreement rather
	    than quietly getting a free pass.
	*/
	const float kQuantum = 1.0f / 2048.0f;

	bool ok = true;

	for( int geometry = 0; geometry < focus::kGeometryCount; ++geometry )
	{
		Tilter plugin;
		plugin.SetFloatParameter( Tilter::PT_GEOMETRY, static_cast< float >( geometry ) );
		//Off centre and off axis on purpose: a centred horizontal band agrees
		//with almost any wrong transform, and a v-axis flip in particular is
		//completely invisible on one.
		plugin.SetFloatParameter( Tilter::PT_FOCUS_X, 0.42f );
		plugin.SetFloatParameter( Tilter::PT_FOCUS_Y, 0.63f );
		plugin.SetFloatParameter( Tilter::PT_ANGLE, 0.63f );
		plugin.SetFloatParameter( Tilter::PT_WIDTH, 0.2f );
		plugin.SetFloatParameter( Tilter::PT_FEATHER, 0.35f );
		plugin.SetFloatParameter( Tilter::PT_ELLIPSE_ASPECT, 0.66f );
		plugin.SetFloatParameter( Tilter::PT_HORIZON, 0.28f );
		plugin.SetFloatParameter( Tilter::PT_TILT, 0.62f );
		plugin.SetFloatParameter( Tilter::PT_RATE, 0.58f );
		plugin.SetFloatParameter( Tilter::PT_DEPTH_BIAS, 0.44f );
		plugin.SetFloatParameter( Tilter::PT_DEPTH_CONTRAST, 0.52f );

		const float worst = compareField( plugin, target, input, w, h, geometry );
		if( worst < 0.0f )
		{
			std::printf( "FAIL  %-22s render failed\n", focus::geometryName( geometry ) );
			ok = false;
			continue;
		}

		//One quantum for the CoC buffer, plus the amplified depth quantum when
		//there is a depth buffer in front of it.
		const focus::Field field = controls::field( plugin.hostValues(),
		                                            static_cast< float >( w ) / static_cast< float >( h ) );
		const float amplification = geometry == focus::kImageDepth
		                                ? 1.5f / std::max( field.feather, 1e-4f )
		                                : 0.0f;
		const float tolerance = kQuantum * ( 1.0f + amplification );

		const bool pass = worst <= tolerance;
		std::printf( "%-5s %-22s worst |GLSL - C++| = %.5f (limit %.5f)\n",
		             pass ? "ok" : "FAIL", focus::geometryName( geometry ), worst, tolerance );
		ok = ok && pass;
	}

	//The control. Same comparison, but the C++ is asked for the WRONG geometry,
	//so it must disagree. Without this the four rows above prove only that two
	//numbers were compared, not that the comparison can come out badly.
	{
		Tilter plugin;
		plugin.SetFloatParameter( Tilter::PT_GEOMETRY, static_cast< float >( focus::kRadial ) );
		plugin.SetFloatParameter( Tilter::PT_FOCUS_X, 0.42f );
		plugin.SetFloatParameter( Tilter::PT_FOCUS_Y, 0.63f );
		plugin.SetFloatParameter( Tilter::PT_WIDTH, 0.2f );

		const float worst = compareField( plugin, target, input, w, h, focus::kLinearBand );
		const bool pass   = worst > 0.05f;
		std::printf( "%-5s %-22s must DISAGREE, and does by %.5f\n",
		             pass ? "ok" : "FAIL", "control (wrong mode)", worst );
		ok = ok && pass;
	}

	glDeleteTextures( 1, &input );
	releaseTarget( target );

	std::printf( "%s\n", ok ? "focus: all geometries match their GLSL mirror" : "focus: FAILED" );
	return ok ? 0 : 1;
}

//---------------------------------------------------------------------------
// --blur
//---------------------------------------------------------------------------
int checkBlur()
{
	const int w = 640;
	const int h = 360;

	Target target = makeTarget( w, h );
	const std::vector< unsigned char > sceneBytes = makeScene( w, h );
	const GLuint input = uploadTexture( sceneBytes, w, h );

	bool ok = true;

	//What the untouched scene measures, through the plugin at zero blur. Taken
	//through the plugin rather than off the uploaded bytes so that the grade
	//and the resample are in both numbers and cancel.
	float sharpReference = 0.0f;
	{
		Tilter plugin;
		plugin.SetFloatParameter( Tilter::PT_BLUR, 0.0f );
		plugin.SetFloatParameter( Tilter::PT_SATURATION, 0.5f );
		plugin.SetFloatParameter( Tilter::PT_CONTRAST, 0.5f );
		plugin.SetFloatParameter( Tilter::PT_VIGNETTE, 0.0f );
		plugin.SetFloatParameter( Tilter::PT_ABERRATION, 0.0f );
		if( !render( plugin, target, input, w, h ) )
		{
			std::printf( "FAIL  reference render failed\n" );
			return 1;
		}
		const auto pixels = readBytes( target );
		sharpReference    = detailInRows( pixels, w, h, 0.12f, 0.22f );
		std::printf( "      reference detail at zero blur = %.2f\n", sharpReference );
	}

	for( int model = 0; model < controls::kBlurModelCount; ++model )
	{
		Tilter plugin;
		plugin.SetFloatParameter( Tilter::PT_BLUR_MODEL, static_cast< float >( model ) );
		//A horizontal band across the middle: sharp at v = 0.5, blurred at the
		//top and bottom of the frame.
		plugin.SetFloatParameter( Tilter::PT_GEOMETRY, static_cast< float >( focus::kLinearBand ) );
		plugin.SetFloatParameter( Tilter::PT_FOCUS_Y, 0.5f );
		plugin.SetFloatParameter( Tilter::PT_ANGLE, 0.5f );
		plugin.SetFloatParameter( Tilter::PT_WIDTH, 0.12f );
		plugin.SetFloatParameter( Tilter::PT_FEATHER, 0.25f );
		plugin.SetFloatParameter( Tilter::PT_BLUR, 0.5f );
		plugin.SetFloatParameter( Tilter::PT_QUALITY, 2.0f );
		plugin.SetFloatParameter( Tilter::PT_SATURATION, 0.5f );
		plugin.SetFloatParameter( Tilter::PT_CONTRAST, 0.5f );
		plugin.SetFloatParameter( Tilter::PT_VIGNETTE, 0.0f );
		plugin.SetFloatParameter( Tilter::PT_ABERRATION, 0.0f );
		plugin.SetFloatParameter( Tilter::PT_HIGHLIGHT, 0.0f );

		if( !render( plugin, target, input, w, h ) )
		{
			std::printf( "FAIL  %s render failed\n", controls::blurModelLabel( model ) );
			ok = false;
			continue;
		}

		const auto pixels = readBytes( target );

		const float inBand  = detailInRows( pixels, w, h, 0.47f, 0.53f );
		const float outBand = detailInRows( pixels, w, h, 0.12f, 0.22f );

		//The sharp band must still be sharp. Not "identical" -- it goes through
		//a resample and the composite -- but it must keep most of its detail.
		const bool keptSharp = inBand > sharpReference * 0.75f;
		//And the far field must have lost most of its.
		const bool blurred = outBand < sharpReference * 0.35f;

		std::printf( "%-5s %-12s band detail %.2f (>= %.2f), far detail %.2f (<= %.2f)\n",
		             ( keptSharp && blurred ) ? "ok" : "FAIL",
		             controls::blurModelLabel( model ),
		             inBand, sharpReference * 0.75f,
		             outBand, sharpReference * 0.35f );

		ok = ok && keptSharp && blurred;
	}

	/*
	    More blur must mean less detail, measured over the WHOLE frame with the
	    whole frame defocused.

	    The obvious version of this test -- a focus band, and detail measured in
	    a fixed strip of the blurred region -- does not work, and the way it
	    fails is worth writing down because it looks exactly like a bug in the
	    blur.

	    Detail in a fixed strip is NOT monotonic in the radius, because a wider
	    blur drags structure INTO the strip from outside it. This scene has
	    clipped highlights on a 64-pixel lattice; at a 10px radius the row of
	    them above the strip stays out of it, and at 20px it spreads in and adds
	    a lattice of bright blobs. Measured that way the numbers went 9.11, 2.89,
	    3.92 and read as a blur that gives up past halfway. Measured over the
	    same picture in a strip 40 rows lower they went the other way, 5.79 down
	    to 2.83, because there the structure was moving out.

	    So the test defocuses everything -- a radial field of zero width, which
	    puts every pixel at full blur -- and measures the entire frame. Structure
	    can then move around inside the measured region as much as it likes
	    without entering or leaving it, and the quantity is monotonic for the
	    reason it ought to be.
	*/
	{
		float previous = 1e9f;
		bool monotonic = true;
		for( float amount : { 0.15f, 0.35f, 0.7f } )
		{
			Tilter plugin;
			plugin.SetFloatParameter( Tilter::PT_GEOMETRY, static_cast< float >( focus::kRadial ) );
			plugin.SetFloatParameter( Tilter::PT_FOCUS_X, 0.5f );
			plugin.SetFloatParameter( Tilter::PT_FOCUS_Y, 0.5f );
			plugin.SetFloatParameter( Tilter::PT_WIDTH, 0.0f );
			plugin.SetFloatParameter( Tilter::PT_FEATHER, 0.0f );
			plugin.SetFloatParameter( Tilter::PT_BLUR, amount );
			plugin.SetFloatParameter( Tilter::PT_VIGNETTE, 0.0f );
			plugin.SetFloatParameter( Tilter::PT_ABERRATION, 0.0f );
			plugin.SetFloatParameter( Tilter::PT_SATURATION, 0.5f );
			plugin.SetFloatParameter( Tilter::PT_CONTRAST, 0.5f );
			render( plugin, target, input, w, h );

			const auto pixels  = readBytes( target );
			const float detail = detailInRows( pixels, w, h, 0.0f, 1.0f );
			std::printf( "      blur %.2f -> whole-frame detail %.2f\n", amount, detail );
			monotonic = monotonic && detail < previous;
			previous  = detail;
		}
		std::printf( "%-5s detail falls as blur rises\n", monotonic ? "ok" : "FAIL" );
		ok = ok && monotonic;
	}

	glDeleteTextures( 1, &input );
	releaseTarget( target );

	std::printf( "%s\n", ok ? "blur: blurs where it should and only there" : "blur: FAILED" );
	return ok ? 0 : 1;
}

//---------------------------------------------------------------------------
// --aperture
//---------------------------------------------------------------------------

/// Area, in pixels, of the region a single highlight was spread over.
///
/// Threshold relative to the frame's own peak rather than absolute: the
/// highlight weighting deliberately does not conserve brightness, so an
/// absolute threshold would measure the weighting rather than the shape.
long litArea( const std::vector< unsigned char >& bottomUp, int width, int height )
{
	unsigned char peak = 0;
	for( size_t i = 0; i < bottomUp.size(); i += 4 )
		peak = std::max( peak, bottomUp[ i + 1 ] );

	if( peak < 32 )
		return 0;

	const unsigned char threshold = static_cast< unsigned char >( peak / 2 );

	long count = 0;
	for( size_t i = 0; i < bottomUp.size(); i += 4 )
		if( bottomUp[ i + 1 ] >= threshold )
			++count;

	return count;
}

int checkAperture()
{
	const int w = 480;
	const int h = 480;

	Target target = makeTarget( w, h );
	const std::vector< unsigned char > sceneBytes = makePointScene( w, h );
	const GLuint input = uploadTexture( sceneBytes, w, h );

	bool ok = true;

	//Everything fully defocused: invert a band that is nowhere, so the whole
	//frame sits at full blur and the single highlight is the only structure.
	auto configure = []( Tilter& plugin, float blur, int blades ) {
		plugin.SetFloatParameter( Tilter::PT_GEOMETRY, static_cast< float >( focus::kRadial ) );
		plugin.SetFloatParameter( Tilter::PT_FOCUS_X, 0.5f );
		plugin.SetFloatParameter( Tilter::PT_FOCUS_Y, 0.5f );
		plugin.SetFloatParameter( Tilter::PT_WIDTH, 0.0f );
		plugin.SetFloatParameter( Tilter::PT_FEATHER, 0.0f );
		plugin.SetFloatParameter( Tilter::PT_BLUR_MODEL, static_cast< float >( controls::kBokeh ) );
		plugin.SetFloatParameter( Tilter::PT_BLUR, blur );
		plugin.SetFloatParameter( Tilter::PT_QUALITY, 2.0f );
		plugin.SetFloatParameter( Tilter::PT_BLADES, static_cast< float >( blades ) );
		plugin.SetFloatParameter( Tilter::PT_HIGHLIGHT, 0.6f );
		plugin.SetFloatParameter( Tilter::PT_SATURATION, 0.5f );
		plugin.SetFloatParameter( Tilter::PT_CONTRAST, 0.5f );
		plugin.SetFloatParameter( Tilter::PT_VIGNETTE, 0.0f );
		plugin.SetFloatParameter( Tilter::PT_ABERRATION, 0.0f );
	};

	long circleArea = 0;
	{
		Tilter plugin;
		configure( plugin, 0.5f, 0 );//circular
		render( plugin, target, input, w, h );
		const auto pixels = readBytes( target );
		circleArea        = litArea( pixels, w, h );

		//What a disc of the radius the controls asked for should measure.
		const controls::Lens lens = controls::lens( plugin.hostValues(), static_cast< float >( h ) );
		const double expected     = 3.14159265 * lens.maxRadius * lens.maxRadius;
		const double ratio        = expected > 0.0 ? static_cast< double >( circleArea ) / expected : 0.0;

		//Loose on purpose. This is asking "is the spread the size the radius
		//says", not "is it a circle to the pixel" -- the half-peak threshold
		//and the highlight weighting both move the boundary a little.
		const bool pass = ratio > 0.4 && ratio < 2.2;
		std::printf( "%-5s circular: lit %ld px, disc of r=%.0f would be %.0f (ratio %.2f)\n",
		             pass ? "ok" : "FAIL", circleArea, lens.maxRadius, expected, ratio );
		ok = ok && pass;
	}

	{
		Tilter plugin;
		configure( plugin, 0.5f, 2 );//6 blades
		render( plugin, target, input, w, h );
		const auto pixels  = readBytes( target );
		const long hexArea = litArea( pixels, w, h );

		//A hexagon inscribed the way apertureEdge draws one has less area than
		//the circle it sits in, and visibly so. If Blades never reached the
		//shader these two are the same number.
		const double ratio = circleArea > 0 ? static_cast< double >( hexArea ) / static_cast< double >( circleArea ) : 0.0;
		const bool pass    = ratio > 0.55 && ratio < 0.97;
		std::printf( "%-5s 6 blades: lit %ld px, %.2f of the circle's area\n",
		             pass ? "ok" : "FAIL", hexArea, ratio );
		ok = ok && pass;
	}

	//And the spread must scale with the radius.
	{
		Tilter plugin;
		configure( plugin, 0.25f, 0 );
		render( plugin, target, input, w, h );
		const auto pixels   = readBytes( target );
		const long smallArea = litArea( pixels, w, h );

		//Half the radius is a quarter of the area, so anything near half the
		//big one means the radius is not reaching the gather.
		const double ratio = circleArea > 0 ? static_cast< double >( smallArea ) / static_cast< double >( circleArea ) : 0.0;
		const bool pass    = ratio > 0.10 && ratio < 0.45;
		std::printf( "%-5s half radius: lit %ld px, %.2f of full radius (expect about 0.25)\n",
		             pass ? "ok" : "FAIL", smallArea, ratio );
		ok = ok && pass;
	}

	glDeleteTextures( 1, &input );
	releaseTarget( target );

	std::printf( "%s\n", ok ? "aperture: shape and size reach the picture" : "aperture: FAILED" );
	return ok ? 0 : 1;
}

//---------------------------------------------------------------------------
// --presets
//---------------------------------------------------------------------------
/// Presets survive every host behaviour.
///
/// The host owns parameter state, and what it does with the values a preset
/// writes is not a thing the plugin gets to decide. Three behaviours matter:
/// a host that consumes the value events and pushes our own numbers back
/// ("honours"), one that ignores them and carries on pushing what it still
/// believes ("ignores"), and one that honours them but hands back a rounded
/// copy ("quantises"). Resolume is the second and third.
///
/// This is the check that was missing here. Tilter carried the fleet's preset
/// shape but compared an incoming value against `params[]` alone, which only
/// ever recognises the "honours" case -- so a host restating what it still
/// believed read as an operator edit and the dropdown snapped back to Custom
/// the instant a preset was chosen. Escapement #2, in a fifth plugin.
int checkHosts()
{
	std::printf( "  preset                    honours   ignores   quantises\n" );

	int count               = 0;
	const unsigned int* ids = Tilter::PresetParamIDsForTest( count );

	enum Behaviour
	{
		Honours,
		Ignores,
		Quantises,
		BehaviourCount
	};

	int failures = 0;
	for( int i = 1; i <= presets::kCount; ++i )
	{
		bool result[ BehaviourCount ] = {};

		for( int b = 0; b < BehaviourCount; ++b )
		{
			Tilter plugin;

			// What the host believes before the preset is chosen.
			std::vector< float > believed( static_cast< size_t >( count ) );
			for( int j = 0; j < count; ++j )
				believed[ j ] = plugin.GetFloatParameter( ids[ j ] );

			plugin.SetFloatParameter( Tilter::PT_PRESET, static_cast< float >( i ) );

			// Twice, because a host that pushes every frame pushes more than
			// once and the bug this guards against only needed one.
			for( int pass = 0; pass < 2; ++pass )
			{
				for( int j = 0; j < count; ++j )
				{
					float push = 0.0f;
					switch( b )
					{
						case Honours: push = plugin.GetFloatParameter( ids[ j ] ); break;
						case Ignores: push = believed[ j ]; break;
						default:
						{
							const float value = plugin.GetFloatParameter( ids[ j ] );
							push              = std::round( value * 1000.0f ) / 1000.0f;
							break;
						}
					}
					plugin.SetFloatParameter( ids[ j ], push );
				}
			}

			bool ok = std::lround( plugin.GetFloatParameter( Tilter::PT_PRESET ) ) == i;
			for( int j = 0; j < count && ok; ++j )
			{
				const float expected = presets::kPresets[ i - 1 ].v[ j ];
				if( expected < 0.0f )
					continue;//not covered by this preset

				//The same quantisation allowance the plugin itself uses.
				if( std::fabs( plugin.GetFloatParameter( ids[ j ] ) - expected ) > 1.5e-3f )
					ok = false;
			}
			result[ b ] = ok;
			if( !ok )
				++failures;
		}

		std::printf( "  %-24s %8s %9s %11s\n", presets::kPresets[ i - 1 ].name,
		             result[ Honours ] ? "ok" : "FAIL",
		             result[ Ignores ] ? "ok" : "FAIL",
		             result[ Quantises ] ? "ok" : "FAIL" );
	}

	std::printf( "\n  %s\n", failures == 0 ? "PASS" : "FAIL" );
	return failures == 0 ? 0 : 1;
}

int checkPresets()
{
	const int w = 320;
	const int h = 180;

	Target target = makeTarget( w, h );
	const std::vector< unsigned char > sceneBytes = makeScene( w, h );
	const GLuint input = uploadTexture( sceneBytes, w, h );

	std::vector< std::vector< unsigned char > > renders;
	bool ok = true;

	for( int i = 0; i <= presets::kCount; ++i )
	{
		Tilter plugin;
		plugin.SetFloatParameter( Tilter::PT_PRESET, static_cast< float >( i ) );
		if( !render( plugin, target, input, w, h ) )
		{
			std::printf( "FAIL  preset %d render failed\n", i );
			ok = false;
			continue;
		}

		auto pixels = readBytes( target );

		//Not black, not white: a preset that renders a flat field is a preset
		//with a value in the wrong units, and it is the failure a contact sheet
		//would catch by eye and nothing else here would.
		double mean = 0.0;
		for( size_t p = 0; p < pixels.size(); p += 4 )
			mean += pixels[ p + 1 ];
		mean /= static_cast< double >( pixels.size() / 4 );

		const float detail = detailInRows( pixels, w, h, 0.0f, 1.0f );
		const bool alive   = mean > 8.0 && mean < 247.0 && detail > 0.5f;

		const char* name = i == 0 ? "Custom" : presets::kPresets[ i - 1 ].name;
		std::printf( "%-5s %-20s mean %.1f, detail %.2f\n",
		             alive ? "ok" : "FAIL", name, mean, detail );
		ok = ok && alive;

		renders.push_back( std::move( pixels ) );
	}

	//Every preset must differ from every other. Two identical rows mean a
	//preset table row that never reached the parameters.
	for( size_t a = 0; a < renders.size(); ++a )
	{
		for( size_t b = a + 1; b < renders.size(); ++b )
		{
			long differing = 0;
			for( size_t p = 0; p < renders[ a ].size(); p += 4 )
				if( std::abs( static_cast< int >( renders[ a ][ p + 1 ] ) - static_cast< int >( renders[ b ][ p + 1 ] ) ) > 3 )
					++differing;

			if( differing < 200 )
			{
				const char* nameA = a == 0 ? "Custom" : presets::kPresets[ a - 1 ].name;
				const char* nameB = b == 0 ? "Custom" : presets::kPresets[ b - 1 ].name;
				std::printf( "FAIL  \"%s\" and \"%s\" render the same picture (%ld px differ)\n",
				             nameA, nameB, differing );
				ok = false;
			}
		}
	}

	glDeleteTextures( 1, &input );
	releaseTarget( target );

	std::printf( "%s\n", ok ? "presets: all distinct and none degenerate" : "presets: FAILED" );
	return ok ? 0 : 1;
}

//---------------------------------------------------------------------------
// --sheet
//---------------------------------------------------------------------------

/// A contact sheet of every mode, and the most valuable thing in this file.
///
/// It asserts nothing. Every real bug found in this fleet's plugins was found
/// by looking at one of these rather than by a number coming out wrong, because
/// a picture that is subtly the wrong shape passes every threshold anybody
/// thinks to write.
int writeSheet( const std::string& path )
{
	const int cellW = 320;
	const int cellH = 180;
	const int cols  = 4;
	const int rows  = 3;

	Target target = makeTarget( cellW, cellH );
	const std::vector< unsigned char > sceneBytes = makeScene( cellW, cellH );
	const GLuint input = uploadTexture( sceneBytes, cellW, cellH );

	std::vector< unsigned char > sheet( static_cast< size_t >( cellW * cols ) * ( cellH * rows ) * 4, 0 );

	struct Cell
	{
		int geometry;
		int model;
		bool showFocus;
	};

	//Row 1: each geometry through the bokeh path. Row 2: the same fields shown
	//as fields, which is where a wrong one is obvious. Row 3: the Gaussian.
	std::vector< Cell > cells;
	for( int g = 0; g < focus::kGeometryCount; ++g )
		cells.push_back( { g, controls::kBokeh, false } );
	for( int g = 0; g < focus::kGeometryCount; ++g )
		cells.push_back( { g, controls::kBokeh, true } );
	for( int g = 0; g < focus::kGeometryCount; ++g )
		cells.push_back( { g, controls::kGaussian, false } );

	for( size_t i = 0; i < cells.size(); ++i )
	{
		Tilter plugin;
		plugin.SetFloatParameter( Tilter::PT_GEOMETRY, static_cast< float >( cells[ i ].geometry ) );
		plugin.SetFloatParameter( Tilter::PT_BLUR_MODEL, static_cast< float >( cells[ i ].model ) );
		plugin.SetFloatParameter( Tilter::PT_SHOW_FOCUS, cells[ i ].showFocus ? 1.0f : 0.0f );
		plugin.SetFloatParameter( Tilter::PT_BLUR, 0.55f );
		plugin.SetFloatParameter( Tilter::PT_QUALITY, 2.0f );
		plugin.SetFloatParameter( Tilter::PT_BLADES, 2.0f );
		plugin.SetFloatParameter( Tilter::PT_HIGHLIGHT, 0.5f );
		//Tilt and horizon set away from neutral, or the Tilted Plane cell draws
		//the same picture as the Linear Band one and the sheet says nothing.
		plugin.SetFloatParameter( Tilter::PT_TILT, 0.68f );
		plugin.SetFloatParameter( Tilter::PT_HORIZON, 0.28f );

		if( !render( plugin, target, input, cellW, cellH ) )
		{
			std::fprintf( stderr, "cell %zu failed to render\n", i );
			continue;
		}

		const auto pixels = flipRows( readBytes( target ), cellW, cellH );

		const int col = static_cast< int >( i ) % cols;
		const int row = static_cast< int >( i ) / cols;
		for( int y = 0; y < cellH; ++y )
		{
			unsigned char* dst = sheet.data()
			                     + ( ( static_cast< size_t >( row * cellH + y ) ) * ( cellW * cols )
			                         + static_cast< size_t >( col * cellW ) ) * 4;
			std::memcpy( dst, pixels.data() + static_cast< size_t >( y ) * cellW * 4,
			             static_cast< size_t >( cellW ) * 4 );
		}
	}

	glDeleteTextures( 1, &input );
	releaseTarget( target );

	if( !writePng( path, cellW * cols, cellH * rows, sheet ) )
	{
		std::fprintf( stderr, "could not write %s\n", path.c_str() );
		return 1;
	}

	std::printf( "wrote %s (%d x %d)\n", path.c_str(), cellW * cols, cellH * rows );
	std::printf( "  row 1: the four focus shapes, bokeh\n" );
	std::printf( "  row 2: the same four as focus fields\n" );
	std::printf( "  row 3: the four again, Gaussian\n" );
	return 0;
}

//---------------------------------------------------------------------------
// --out
//---------------------------------------------------------------------------
int renderFrame( const std::string& path, const Options& options )
{
	Target target = makeTarget( options.width, options.height );
	const std::vector< unsigned char > sceneBytes = makeScene( options.width, options.height );
	const GLuint input = uploadTexture( sceneBytes, options.width, options.height );

	Tilter plugin;
	applySets( plugin, options );

	if( !render( plugin, target, input, options.width, options.height ) )
	{
		std::fprintf( stderr, "render failed\n" );
		return 1;
	}

	const auto pixels = flipRows( readBytes( target ), options.width, options.height );

	glDeleteTextures( 1, &input );
	releaseTarget( target );

	if( !writePng( path, options.width, options.height, pixels ) )
	{
		std::fprintf( stderr, "could not write %s\n", path.c_str() );
		return 1;
	}

	std::printf( "wrote %s (%d x %d)\n", path.c_str(), options.width, options.height );
	return 0;
}

int writeScene( const std::string& path, const Options& options )
{
	const auto bytes = makeScene( options.width, options.height );
	if( !writePng( path, options.width, options.height, bytes ) )
	{
		std::fprintf( stderr, "could not write %s\n", path.c_str() );
		return 1;
	}
	std::printf( "wrote %s (%d x %d)\n", path.c_str(), options.width, options.height );
	return 0;
}

/**
    --pipe: real footage through the real plugin, on a cue sheet.

    Frames arrive as raw RGBA on stdin and leave as raw RGBA on stdout, so
    ffmpeg does the decoding and the encoding and this does the lens. That is
    how the project video is made, and it is a render rather than a screen
    recording for a reason worth stating: an FFGL plugin has no window and no UI
    of its own -- its control surface IS Resolume's inspector -- so "filming the
    app" would mean filming Arena, whose clip grid and effects browser are
    custom-drawn with nothing in the accessibility tree to address.

    What is on screen is genuinely this plugin's output, from the same class
    Resolume loads. It is just not a photograph of Resolume, and the end card
    says so.
*/
int runPipe( int width, int height, const std::string& scriptPath, const Options& options )
{
	std::map< std::string, Track > tracks;
	if( !scriptPath.empty() )
	{
		std::string error;
		tracks = loadScript( scriptPath, error );
		if( !error.empty() )
		{
			std::fprintf( stderr, "tiltest: %s\n", error.c_str() );
			return 1;
		}
	}

	Target target = makeTarget( width, height );

	Tilter plugin;
	applySets( plugin, options );

	//Resolve the cue sheet's names once, against the plugin itself. A cue for a
	//parameter that does not exist is a silent no-op otherwise, and the first
	//sign of it is a beat in the finished video where nothing happens.
	const auto byName = parameterIndex( plugin );
	std::vector< std::pair< unsigned int, const Track* > > bound;
	for( const auto& entry : tracks )
	{
		const auto found = byName.find( entry.first );
		if( found == byName.end() )
		{
			std::fprintf( stderr, "tiltest: no parameter named \"%s\" in the script\n",
			              entry.first.c_str() );
			return 1;
		}
		bound.emplace_back( found->second, &entry.second );
	}

	GLuint input = 0;
	glGenTextures( 1, &input );
	glBindTexture( GL_TEXTURE_2D, input );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );

	const size_t frameBytes = static_cast< size_t >( width ) * height * 4;
	std::vector< unsigned char > incoming( frameBytes );

	int frame = 0;
	while( readExactly( incoming.data(), frameBytes ) )
	{
		for( const auto& track : bound )
			plugin.SetFloatParameter( track.first, valueAt( *track.second, frame ) );

		//ffmpeg hands over rows top-down; GL wants them bottom-up, and the
		//composite reads Focus Y as 0 at the top. Flipping on the way in and
		//again on the way out keeps every coordinate in this file meaning what
		//it says everywhere else.
		const std::vector< unsigned char > flipped = flipRows( incoming, width, height );

		glBindTexture( GL_TEXTURE_2D, input );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE,
		                 flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );

		if( !render( plugin, target, input, width, height ) )
		{
			std::fprintf( stderr, "tiltest: render failed at frame %d\n", frame );
			return 1;
		}

		const std::vector< unsigned char > out = flipRows( readBytes( target ), width, height );
		if( fwrite( out.data(), 1, frameBytes, stdout ) != frameBytes )
		{
			std::fprintf( stderr, "tiltest: short write at frame %d\n", frame );
			return 1;
		}

		++frame;
	}

	fflush( stdout );
	std::fprintf( stderr, "tiltest: %d frames\n", frame );

	glDeleteTextures( 1, &input );
	releaseTarget( target );
	return 0;
}

void usage()
{
	std::printf(
		"tiltest -- the Tilter offline harness\n"
		"\n"
		"  --out PATH        render a frame of the synthetic scene\n"
		"  --scene PATH      write the synthetic scene itself\n"
		"  --sheet PATH      contact sheet of every focus shape and blur\n"
		"  --list            every parameter, with type and default\n"
		"  --set \"Name=v\"    set a parameter by its host-facing name\n"
		"  --size WxH        render size (default 640x360)\n"
		"  --focus           CoC field against Focus.cpp, all four geometries\n"
		"  --blur            the blur blurs, and only where it should\n"
		"  --aperture        the aperture shape reaches the picture\n"
		"  --presets         every factory preset distinct and non-degenerate\n"
		"  --hosts           presets survive every host behaviour\n"
		"\n"
		"  --pipe            raw RGBA frames in on stdin, out on stdout\n"
		"  --script PATH     parameter automation: `frame Parameter Name value`\n" );
}
} // namespace

int main( int argc, char** argv )
{
	if( argc < 2 )
	{
		usage();
		return 1;
	}

	//--list needs no GL at all, and running it without a context means it still
	//works over ssh and in a container.
	for( int i = 1; i < argc; ++i )
		if( std::strcmp( argv[ i ], "--list" ) == 0 )
			return listParameters();

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL 4.1 core context\n" );
		return 1;
	}

	Options options;
	std::string outPath;
	std::string scenePath;
	std::string sheetPath;
	bool pipeMode = false;
	std::string scriptPath;
	bool doFocus    = false;
	bool doBlur     = false;
	bool doAperture = false;
	bool doPresets  = false;
	bool doHosts   = false;

	for( int i = 1; i < argc; ++i )
	{
		const std::string arg = argv[ i ];
		if( arg == "--out" && i + 1 < argc )
			outPath = argv[ ++i ];
		else if( arg == "--scene" && i + 1 < argc )
			scenePath = argv[ ++i ];
		else if( arg == "--sheet" && i + 1 < argc )
			sheetPath = argv[ ++i ];
		else if( arg == "--pipe" )
			pipeMode = true;
		else if( arg == "--script" && i + 1 < argc )
			scriptPath = argv[ ++i ];
		else if( arg == "--focus" )
			doFocus = true;
		else if( arg == "--blur" )
			doBlur = true;
		else if( arg == "--aperture" )
			doAperture = true;
		else if( arg == "--presets" )
			doPresets = true;
		else if( arg == "--hosts" )
			doHosts = true;
		else if( arg == "--size" && i + 1 < argc )
		{
			int w = 0;
			int h = 0;
			if( std::sscanf( argv[ ++i ], "%dx%d", &w, &h ) == 2 && w > 0 && h > 0 )
			{
				options.width  = w;
				options.height = h;
			}
		}
		else if( arg == "--set" && i + 1 < argc )
		{
			const std::string spec = argv[ ++i ];
			const size_t equals    = spec.find( '=' );
			if( equals != std::string::npos )
				options.sets.emplace_back( spec.substr( 0, equals ),
				                           std::strtof( spec.c_str() + equals + 1, nullptr ) );
		}
		else
		{
			std::fprintf( stderr, "unrecognised argument: %s\n", arg.c_str() );
			usage();
			CGLDestroyContext( context );
			return 1;
		}
	}

	if( pipeMode )
	{
		const int status = runPipe( options.width, options.height, scriptPath, options );
		CGLDestroyContext( context );
		return status;
	}

	int status = 0;
	if( !scenePath.empty() )
		status |= writeScene( scenePath, options );
	if( !outPath.empty() )
		status |= renderFrame( outPath, options );
	if( !sheetPath.empty() )
		status |= writeSheet( sheetPath );
	if( doFocus )
		status |= checkFocus();
	if( doBlur )
		status |= checkBlur();
	if( doAperture )
		status |= checkAperture();
	if( doPresets )
		status |= checkPresets();
	if( doHosts )
		status |= checkHosts();

	CGLDestroyContext( context );
	return status;
}
