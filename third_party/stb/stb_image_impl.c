/* Single compilation unit for stb_image.
 * Only formats we intend to support are enabled; everything is decoded from
 * memory (no stdio), because asset bytes come through the engine's file API. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_FAILURE_USERMSG
#include "stb_image.h"
