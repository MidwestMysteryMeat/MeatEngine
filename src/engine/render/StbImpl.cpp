// The single translation unit that hosts the stb_image implementation.
// Everything else includes <stb_image.h> declarations only.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244 4456 4457 4996) // third-party code, not held to /W4
#endif

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#ifdef _MSC_VER
#pragma warning(pop)
#endif
