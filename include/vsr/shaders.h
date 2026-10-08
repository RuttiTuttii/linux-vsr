#ifndef VSR_SHADERS_H
#define VSR_SHADERS_H

#include <stddef.h>
#include <stdbool.h>

// generate cas glsl shader function string with specified sharpness
char* vsr_shader_generate_cas(float sharpness);

// generate cas with watermark badge baked in
char* vsr_shader_generate_cas_full(float sharpness, bool watermark, float opacity, float size_frac);

// generate easu with watermark badge baked in
char* vsr_shader_generate_easu_full(bool watermark, float opacity, float size_frac);

// generate vendor-neutral directional sharpen fragment
char* vsr_shader_generate_directional_full(float sharpness, bool watermark, float opacity, float size_frac);

// dispatch generator by mode name (cas/easu/directional/off)
char* vsr_shader_generate_upscaler(const char *mode, float sharpness, bool watermark, float opacity, float size_frac);

#endif // VSR_SHADERS_H
