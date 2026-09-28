// Implementations of single-header libraries, compiled exactly once.
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO        // we load files ourselves (Unicode paths on Windows)
#define STBI_FAILURE_USERMSG
#include <stb_image.h>
