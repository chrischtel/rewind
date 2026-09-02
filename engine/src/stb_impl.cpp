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
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
