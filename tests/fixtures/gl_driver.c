// minimal GL dispatch fixture for checking hook diagnostics without a GPU
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// capture the shader that reached the downstream driver
void glShaderSource(unsigned int shader, int count, const char *const *strings, const int *lengths) {
    (void)shader;
    FILE *fp = fopen(getenv("VSR_TEST_SOURCE"), "w");
    if (!fp) {
        return;
    }
    for (int i = 0; i < count; i++) {
        size_t length = lengths && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]);
        fwrite(strings[i], 1, length, fp);
    }
    fclose(fp);
}

// no compilation is performed by the test fixture
void glCompileShader(unsigned int shader) {
    (void)shader;
}

// return an injected compile result for the hook's diagnostic query
void glGetShaderiv(unsigned int shader, unsigned int pname, int *params) {
    (void)shader;
    *params = pname == 0x8B81 ? getenv("VSR_TEST_FAIL") == NULL : 64;
}

// provide a bounded diagnostic message for the failed compilation branch
void glGetShaderInfoLog(unsigned int shader, int maxlen, int *length, char *log) {
    (void)shader;
    int written = snprintf(log, (size_t)maxlen, "fixture compile failure");
    if (length) {
        *length = written;
    }
}
