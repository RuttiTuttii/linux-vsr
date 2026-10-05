#include "vsr/patcher.h"
#include <assert.h>
#include <string.h>
#include <stdlib.h>

// unit test for string replacement engine
void test_patcher_replace_all(void) {
    // test multiple replacements
    const char *orig = "foo bar foo baz foo";
    char *res = vsr_patcher_replace_all(orig, "foo", "qux");
    assert(res != NULL);
    assert(strcmp(res, "qux bar qux baz qux") == 0);
    free(res);

    // test when target string is not present
    char *res2 = vsr_patcher_replace_all(orig, "missing", "found");
    assert(res2 != NULL);
    assert(strcmp(res2, orig) == 0);
    free(res2);
}

// unit test for chunk consolidation
void test_patcher_combine_chunks(void) {
    // test multiple string chunks
    const char *chunks[] = {"chunk1 ", "chunk2 ", "chunk3"};
    size_t total = 0;
    char *combined = vsr_patcher_combine_chunks(3, chunks, NULL, &total);
    assert(combined != NULL);
    assert(strcmp(combined, "chunk1 chunk2 chunk3") == 0);
    assert(total == strlen("chunk1 chunk2 chunk3"));
    free(combined);
}

// unit test for webrender shader identification
void test_patcher_target_identification(void) {
    // mock webrender video fragment shader
    const char *valid_yuv =
        "#version 300 es\n"
        "vec4 sample_yuv(int format) {\n"
        "    ycbcr_sample.x = TEX_SAMPLE(sColor0, uv_y).r;\n"
        "    return vec4(1.0);\n"
        "}\n";

    // assert valid shader is recognized
    assert(vsr_patcher_is_target_shader(valid_yuv) == true);

    // mock standard ui shader
    const char *ui_shader =
        "#version 300 es\n"
        "void main() {\n"
        "    gl_FragColor = texture2D(sColor, uv);\n"
        "}\n";

    // assert ui shader is ignored
    assert(vsr_patcher_is_target_shader(ui_shader) == false);
}

// unit test for upscaler injection
void test_patcher_injection(void) {
    // mock target shader snippet
    const char *sample_shader =
        "// header\n"
        "vec4 sample_yuv(int format) {\n"
        "    ycbcr_sample.x = TEX_SAMPLE(sColor0, uv_y).r;\n"
        "    return vec4(1.0);\n"
        "}\n";

    // run patcher
    char *patched = vsr_patcher_inject_upscaler(sample_shader, 0.22f);
    assert(patched != NULL);

    // assert injected cas function is present
    assert(strstr(patched, "sample_luma_cas") != NULL);

    // assert sampling call was redirected
    assert(strstr(patched, "ycbcr_sample.x = sample_luma_cas(sColor0, uv_y, uv_bounds_y);") != NULL);

    // assert old naive texture call was removed
    assert(strstr(patched, "TEX_SAMPLE(sColor0, uv_y).r") == NULL);

    free(patched);
}
