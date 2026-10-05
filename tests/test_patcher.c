#include "vsr/patcher.h"
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

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

// unit test for replacement safety guards
void test_patcher_replace_safety(void) {
    // test null inputs return null
    assert(vsr_patcher_replace_all(NULL, "a", "b") == NULL);
    assert(vsr_patcher_replace_all("abc", NULL, "b") == NULL);
    // test empty target rejected
    assert(vsr_patcher_replace_all("abc", "", "b") == NULL);
    // test null replacement treated as empty
    char *r = vsr_patcher_replace_all("foo foo", "foo", NULL);
    assert(r != NULL);
    assert(strcmp(r, " ") == 0);
    free(r);
    // test shrinking replacement
    char *s = vsr_patcher_replace_all("aaaa", "aa", "b");
    assert(s != NULL);
    assert(strcmp(s, "bb") == 0);
    free(s);
    // test growing replacement
    char *g = vsr_patcher_replace_all("ab", "a", "xyz");
    assert(g != NULL);
    assert(strcmp(g, "xyzb") == 0);
    free(g);
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

// unit test for chunk safety guards
void test_patcher_combine_safety(void) {
    // test invalid counts
    assert(vsr_patcher_combine_chunks(0, NULL, NULL, NULL) == NULL);
    assert(vsr_patcher_combine_chunks(-1, NULL, NULL, NULL) == NULL);
    assert(vsr_patcher_combine_chunks(5000, NULL, NULL, NULL) == NULL);
    // test null array
    assert(vsr_patcher_combine_chunks(2, NULL, NULL, NULL) == NULL);
    // test null chunks skipped gracefully
    const char *with_null[] = {"hi ", NULL, "there"};
    size_t total = 0;
    char *c = vsr_patcher_combine_chunks(3, with_null, NULL, &total);
    assert(c != NULL);
    assert(strcmp(c, "hi there") == 0);
    free(c);
    // test explicit lengths
    const char *parts[] = {"hello world", "unused"};
    int lens[] = {5, 0};
    char *e = vsr_patcher_combine_chunks(2, parts, lens, &total);
    assert(e != NULL);
    assert(strncmp(e, "hello", 5) == 0);
    free(e);
    // test null chunk with explicit length rejected
    const char *bad[] = {NULL, "hi"};
    int bad_len[] = {5, 0};
    assert(vsr_patcher_combine_chunks(2, bad, bad_len, NULL) == NULL);
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
    // assert null and empty rejected
    assert(vsr_patcher_is_target_shader(NULL) == false);
    assert(vsr_patcher_is_target_shader("") == false);
    // assert already patched skipped
    const char *patched =
        "float sample_luma_cas() { return 1.0; }\n"
        "vec4 sample_yuv(int f) { ycbcr_sample.x = TEX_SAMPLE(sColor0, uv_y).r; }\n";
    assert(vsr_patcher_is_target_shader(patched) == false);
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

// unit test for injection safety and modes
void test_patcher_injection_safety(void) {
    // test null source
    assert(vsr_patcher_inject_upscaler(NULL, 0.22f) == NULL);
    assert(vsr_patcher_inject_upscaler_full(NULL, "cas", 0.22f, false, 0.45f, 0.06f) == NULL);
    // test empty source
    assert(vsr_patcher_inject_upscaler("", 0.22f) == NULL);
    // test missing anchor returns null
    assert(vsr_patcher_inject_upscaler("void main() {}", 0.22f) == NULL);
    // test missing sampling line returns null
    const char *no_sample = "vec4 sample_yuv(int f) { return vec4(1.0); }";
    assert(vsr_patcher_inject_upscaler(no_sample, 0.22f) == NULL);
    // test off mode returns null
    const char *valid =
        "vec4 sample_yuv(int f) {\n"
        "    ycbcr_sample.x = TEX_SAMPLE(sColor0, uv_y).r;\n"
        "}\n";
    assert(vsr_patcher_inject_upscaler_full(valid, "off", 0.22f, false, 0.45f, 0.06f) == NULL);
    // test double injection prevented
    char *first = vsr_patcher_inject_upscaler(valid, 0.22f);
    assert(first != NULL);
    char *second = vsr_patcher_inject_upscaler_full(first, "cas", 0.22f, false, 0.45f, 0.06f);
    assert(second == NULL);
    free(first);
    // test full with watermark but no bounds falls back safely
    char *wm = vsr_patcher_inject_upscaler_full(valid, "cas", 0.22f, true, 0.45f, 0.06f);
    assert(wm != NULL);
    // without uv_bounds_y watermark helpers must be absent
    assert(strstr(wm, "vsr_badge_mask") == NULL);
    free(wm);
    // test full with bounds keeps watermark
    const char *with_bounds =
        "uniform vec4 uv_bounds_y;\n"
        "vec4 sample_yuv(int f) {\n"
        "    ycbcr_sample.x = TEX_SAMPLE(sColor0, uv_y).r;\n"
        "}\n";
    char *wm2 = vsr_patcher_inject_upscaler_full(with_bounds, "cas", 0.22f, true, 0.45f, 0.06f);
    assert(wm2 != NULL);
    assert(strstr(wm2, "vsr_badge_mask") != NULL);
    free(wm2);
    // test easu mode path
    char *easu = vsr_patcher_inject_upscaler_full(valid, "easu", 0.22f, false, 0.45f, 0.06f);
    assert(easu != NULL);
    free(easu);
}
