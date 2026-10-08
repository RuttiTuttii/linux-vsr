// submit a known WebRender signature to the interposed GL dispatch fixture
void glShaderSource(unsigned int shader, int count, const char *const *strings, const int *lengths);
void glCompileShader(unsigned int shader);

int main(void) {
    const char *source = "vec4 sample_yuv(int f) {\n"
        "    ycbcr_sample.x = TEX_SAMPLE(sColor0, uv_y).r;\n"
        "}\n";
    glShaderSource(7, 1, &source, 0);
    glCompileShader(7);
    return 0;
}
