// =============================================================================
// AMD FidelityFX Contrast Adaptive Sharpening (CAS) for Video Luma (Y)
// Adapted for WebRender GLSL Fragment Pipeline
// =============================================================================

// Normalizes texture coordinates and samples 3x3 neighbourhood
// around current texel on the Luma channel (sColor0 in NV12 / P010 / Planar)
float sample_luma_cas(sampler2D tex, vec2 uv, vec4 bounds) {
    vec2 size = vec2(textureSize(tex, 0));
    vec2 texel = 1.0 / max(size, vec2(1.0));

    // Center pixel (bilinear sample)
    float e = texture(tex, uv).r;

    // Cross pattern samples (top, left, right, bottom)
    float b = texture(tex, clamp(uv + vec2(0.0, -texel.y), bounds.xy, bounds.zw)).r;
    float d = texture(tex, clamp(uv + vec2(-texel.x, 0.0), bounds.xy, bounds.zw)).r;
    float f = texture(tex, clamp(uv + vec2( texel.x, 0.0), bounds.xy, bounds.zw)).r;
    float h = texture(tex, clamp(uv + vec2(0.0,  texel.y), bounds.xy, bounds.zw)).r;

    // Corner samples (top-left, top-right, bottom-left, bottom-right)
    float a = texture(tex, clamp(uv + vec2(-texel.x, -texel.y), bounds.xy, bounds.zw)).r;
    float c = texture(tex, clamp(uv + vec2( texel.x, -texel.y), bounds.xy, bounds.zw)).r;
    float g = texture(tex, clamp(uv + vec2(-texel.x,  texel.y), bounds.xy, bounds.zw)).r;
    float i = texture(tex, clamp(uv + vec2( texel.x,  texel.y), bounds.xy, bounds.zw)).r;

    // Soft minimum and maximum of 3x3 window
    float mn = min(min(min(d, e), min(f, b)), h);
    float mn2 = min(min(min(mn, a), min(c, g)), i);
    mn = mn + mn2;

    float mx = max(max(max(d, e), max(f, b)), h);
    float mx2 = max(max(max(mx, a), max(c, g)), i);
    mx = mx + mx2;

    // Contrast-adaptive weight calculation (prevents ringing / haloing on high-contrast edges)
    // Smooth limit: min(mn, 2.0 - mx) / mx
    float amp = clamp(min(mn, 2.0 - mx) / max(mx, 0.001), 0.0, 1.0);
    
    // Sharpness control (-0.15 = mild, -0.22 = balanced, -0.25 = maximum crisp)
    float w = -sqrt(amp) * 0.22;

    // Filter convolution
    float res = (b + d + f + h) * w + e;
    return clamp(res / (4.0 * w + 1.0), 0.0, 1.0);
}
