/* miniaudio implementation, compiled once as C: stb_vorbis for OGG Vorbis (declarations before
   miniaudio, implementation after it) and verblib (Freeverb-style reverb, MIT-0) for reverb zones. */
#define STB_VORBIS_HEADER_ONLY
#include "extras/stb_vorbis.c"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#undef STB_VORBIS_HEADER_ONLY
#include "extras/stb_vorbis.c"

#define VERBLIB_IMPLEMENTATION
#include "verblib.h"
