#include "vsr/shaders.h"
#include <stdio.h>
#include <stdlib.h>

// allocate and generate cas glsl function with specified sharpness weight
char* vsr_shader_generate_cas(float sharpness) {
    // allocate buffer for shader code
    char *buffer = malloc(2048);
    if (!buffer) {
        return NULL;
    }

    // format shader text with formatted float weight
    snprintf(buffer, 2048,
        "\n// === linux-vsr: contrast adaptive sharpening (fidelityfx cas) ===\n"
        "float sample_luma_cas(sampler2D tex, vec2 uv, vec4 bounds) {\n"
        "    vec2 size = vec2(textureSize(tex, 0));\n"
        "    vec2 texel = 1.0 / max(size, vec2(1.0));\n"
        "\n"
        "    // center sample\n"
        "    float e = texture(tex, uv).r;\n"
        "\n"
        "    // cardinal cross samples\n"
        "    float b = texture(tex, clamp(uv + vec2(0.0, -texel.y), bounds.xy, bounds.zw)).r;\n"
        "    float d = texture(tex, clamp(uv + vec2(-texel.x, 0.0), bounds.xy, bounds.zw)).r;\n"
        "    float f = texture(tex, clamp(uv + vec2( texel.x, 0.0), bounds.xy, bounds.zw)).r;\n"
        "    float h = texture(tex, clamp(uv + vec2(0.0,  texel.y), bounds.xy, bounds.zw)).r;\n"
        "\n"
        "    // diagonal corner samples\n"
        "    float a = texture(tex, clamp(uv + vec2(-texel.x, -texel.y), bounds.xy, bounds.zw)).r;\n"
        "    float c = texture(tex, clamp(uv + vec2( texel.x, -texel.y), bounds.xy, bounds.zw)).r;\n"
        "    float g = texture(tex, clamp(uv + vec2(-texel.x,  texel.y), bounds.xy, bounds.zw)).r;\n"
        "    float i = texture(tex, clamp(uv + vec2( texel.x,  texel.y), bounds.xy, bounds.zw)).r;\n"
        "\n"
        "    // find 3x3 local extrema\n"
        "    float mn = min(min(min(d, e), min(f, b)), h);\n"
        "    float mn2 = min(min(min(mn, a), min(c, g)), i);\n"
        "    mn = mn + mn2;\n"
        "\n"
        "    float mx = max(max(max(d, e), max(f, b)), h);\n"
        "    float mx2 = max(max(max(mx, a), max(c, g)), i);\n"
        "    mx = mx + mx2;\n"
        "\n"
        "    // contrast-adaptive weight calculation\n"
        "    float amp = clamp(min(mn, 2.0 - mx) / max(mx, 0.001), 0.0, 1.0);\n"
        "    float w = -sqrt(amp) * %.4ff;\n"
        "\n"
        "    // perform convolution\n"
        "    float res = (b + d + f + h) * w + e;\n"
        "    return clamp(res / (4.0 * w + 1.0), 0.0, 1.0);\n"
        "}\n"
        "// === end linux-vsr ===\n\n",
        sharpness
    );

    return buffer;
}
