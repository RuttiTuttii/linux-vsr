#include "vsr/vulkan.h"
#include <assert.h>
#include <stdint.h>

// unit test for a minimal valid vertex module
void test_vulkan_vertex_module(void) {
    const uint32_t module[] = {
        0x07230203u, 0x00010000u, 0, 8, 0,
        (1u << 16) | 19u,
    };
    assert(vsr_vulkan_spirv_is_fragment(module, sizeof(module) / sizeof(module[0])) == false);
}

// unit test for Fragment OpEntryPoint detection
void test_vulkan_fragment_module(void) {
    const uint32_t module[] = {
        0x07230203u, 0x00010000u, 0, 8, 0,
        (3u << 16) | 15u, 4u, 1u,
    };
    assert(vsr_vulkan_spirv_is_fragment(module, sizeof(module) / sizeof(module[0])) == true);
}

// unit test for malformed module guards
void test_vulkan_guards(void) {
    const uint32_t bad[] = {0, 0, 0, 0, 0};
    assert(vsr_vulkan_spirv_is_fragment(NULL, 0) == false);
    assert(vsr_vulkan_spirv_is_fragment(bad, sizeof(bad) / sizeof(bad[0])) == false);
}
