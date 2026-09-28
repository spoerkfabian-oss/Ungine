#ifndef ENGINE_EXPOSURE_GLSL
#define ENGINE_EXPOSURE_GLSL
// Requires: #extension GL_EXT_buffer_reference : require
// Auto exposure state shared by the histogram, averaging and tone mapping passes.

layout(buffer_reference, std430, buffer_reference_align = 4) buffer LuminanceHistogram {
    uint bins[256]; // bin 0: (near) black pixels; 1..255: log2 luminance range
};

layout(buffer_reference, std430, buffer_reference_align = 4) buffer ExposureState {
    float adaptedLuminance; // <= 0: not initialized (snap on the next average)
    float exposure;         // multiplier applied before tone mapping
};

#endif
