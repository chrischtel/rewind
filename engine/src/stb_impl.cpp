// Single translation unit that owns the stb_image implementation.
// Only PNG is needed (sprites, atlases); the rest stays out of the binary.

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO_WRITE
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244 4996 4100 4456 4457 4505)
#endif
#include <stb_image.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>
// stb_vorbis.c carries its implementation unless STB_VORBIS_HEADER_ONLY is set.
#if defined(_MSC_VER)
#pragma warning(disable : 4245 4267 4701 4702 4127 4189 4459 4244)
#endif
#define STB_VORBIS_NO_PUSHDATA_API
#include <stb_vorbis.c>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
