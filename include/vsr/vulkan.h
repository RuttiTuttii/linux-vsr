#ifndef VSR_VULKAN_H
#define VSR_VULKAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// identify a SPIR-V module containing a Fragment execution entry point
bool vsr_vulkan_spirv_is_fragment(const uint32_t *code, size_t word_count);

#endif // VSR_VULKAN_H
