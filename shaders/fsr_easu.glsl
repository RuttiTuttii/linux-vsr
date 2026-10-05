// =============================================================================
// Edge-Adaptive Spatial Filter (Directional Interpolation)
// Reconstructs diagonal edges and high-frequency content in YUV Luma
// =============================================================================

float sample_luma_easu(sampler2D tex, vec2 uv, vec4 bounds) {
    vec2 size = vec2(textureSize(tex, 0));
    vec2 texel = 1.0 / max(size, vec2(1.0));

    // Sample 4 cardinal points
    float tc = texture(tex, clamp(uv + vec2(0.0, -texel.y), bounds.xy, bounds.zw)).r;
    float bc = texture(tex, clamp(uv + vec2(0.0,  texel.y), bounds.xy, bounds.zw)).r;
    float ml = texture(tex, clamp(uv + vec2(-texel.x, 0.0), bounds.xy, bounds.zw)).r;
    float mr = texture(tex, clamp(uv + vec2( texel.x, 0.0), bounds.xy, bounds.zw)).r;
    float cc = texture(tex, uv).r;

    // Gradient estimation
    float grad_x = abs(mr - ml);
    float grad_y = abs(bc - tc);

    // Directional weighting
    float weight_x = 1.0 / (grad_x + 0.001);
    float weight_y = 1.0 / (grad_y + 0.001);
    float norm = weight_x + weight_y + 2.0;

    return (ml * weight_x + mr * weight_x + tc * weight_y + bc * weight_y + 2.0 * cc) / (2.0 * (weight_x + weight_y) + 2.0);
}
