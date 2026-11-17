// Single translation unit holding the stb implementations used by Bulkhead.
#define _CRT_SECURE_NO_WARNINGS
#pragma warning(push, 0)
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#pragma warning(pop)
