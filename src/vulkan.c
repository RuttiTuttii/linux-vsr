#include "vsr/vulkan.h"

// SPIR-V binary header and OpEntryPoint constants
#define VSR_SPIRV_MAGIC 0x07230203u
#define VSR_SPIRV_OP_ENTRY_POINT 15u
#define VSR_SPIRV_EXEC_FRAGMENT 4u

// identify a fragment entry point without depending on SPIRV-Tools
bool vsr_vulkan_spirv_is_fragment(const uint32_t *code, size_t word_count) {
    // validate the module header and bounded word count
    if (!code || word_count < 5 || code[0] != VSR_SPIRV_MAGIC) {
        return false;
    }
    // walk instructions after the five-word module header
    size_t offset = 5;
    while (offset < word_count) {
        uint32_t instruction = code[offset];
        size_t length = (size_t)(instruction >> 16);
        uint32_t opcode = instruction & 0xffffu;
        if (length == 0 || offset + length > word_count) {
            return false;
        }
        if (opcode == VSR_SPIRV_OP_ENTRY_POINT && length >= 3
            && code[offset + 1] == VSR_SPIRV_EXEC_FRAGMENT) {
            return true;
        }
        offset += length;
    }
    return false;
}
