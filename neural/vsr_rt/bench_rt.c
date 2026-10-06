// vsr_rt bench: prove c-only realtime neural upscale without python
// usage: ./build/vsr_rt_bench [--w 1280] [--h 720] [--scale 2] [--iters 30]
// allocates cuda buffers via driver api, fills test pattern on host,
// runs timed inference loop, writes /tmp/vsr_rt_out.ppm for visual check

#define _GNU_SOURCE
#include "vsr_rt.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// driver api function table loaded from libcuda
typedef struct {
    int (*init)(unsigned int);
    int (*dev_get)(int *, int);
    int (*ctx_create)(void **, unsigned int, int);
    int (*mem_alloc)(unsigned long long *, size_t);
    int (*mem_free)(unsigned long long);
    int (*cpy_htod)(unsigned long long, const void *, size_t);
    int (*cpy_dtoh)(void *, unsigned long long, size_t);
    int (*ctx_sync)(void);
    int (*ctx_destroy)(void *);
} vsr_cuda_t;

// load driver entry points from system library
static int vsr_cuda_open(void *handle, vsr_cuda_t *cuda) {
    // resolve each symbol or fail fast
    cuda->init = dlsym(handle, "cuInit");
    cuda->dev_get = dlsym(handle, "cuDeviceGet");
    cuda->ctx_create = dlsym(handle, "cuCtxCreate_v2");
    cuda->mem_alloc = dlsym(handle, "cuMemAlloc_v2");
    cuda->mem_free = dlsym(handle, "cuMemFree_v2");
    cuda->cpy_htod = dlsym(handle, "cuMemcpyHtoD_v2");
    cuda->cpy_dtoh = dlsym(handle, "cuMemcpyDtoH_v2");
    cuda->ctx_sync = dlsym(handle, "cuCtxSynchronize");
    cuda->ctx_destroy = dlsym(handle, "cuCtxDestroy_v2");
    // validate full set resolved
    return cuda->init && cuda->dev_get && cuda->ctx_create && cuda->mem_alloc
        && cuda->mem_free && cuda->cpy_htod && cuda->cpy_dtoh
        && cuda->ctx_sync && cuda->ctx_destroy;
}

// monotonic milliseconds for bench timing
static double vsr_now_ms(void) {
    // read monotonic clock
    struct timespec ts = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

// fill host rgba f32 test pattern with edges and bars
static void vsr_fill_pattern(float *host, unsigned w, unsigned h) {
    // iterate rows and columns
    for (unsigned y = 0; y < h; y++) {
        for (unsigned x = 0; x < w; x++) {
            // normalize coordinates to 0..1 range
            float fx = (float)x / (float)(w - 1);
            float fy = (float)y / (float)(h - 1);
            // compute gradient plus repeating bars
            float bx = fx * 8.0f - (float)(int)(fx * 8.0f);
            float by = fy * 8.0f - (float)(int)(fy * 8.0f);
            size_t idx = ((size_t)y * w + x) * 4u;
            host[idx + 0] = fx;
            host[idx + 1] = fy;
            host[idx + 2] = (bx + by) * 0.5f;
            host[idx + 3] = 1.0f;
        }
    }
}

// write float rgba buffer as binary ppm for visual check
static int vsr_write_ppm(const char *path, const float *host, unsigned w, unsigned h) {
    // open output file
    FILE *fp = fopen(path, "wb");
    // handle open failure
    if (!fp) {
        return 0;
    }
    // write ppm header
    fprintf(fp, "P6\n%u %u\n255\n", w, h);
    // convert rows to uint8 rgb
    for (size_t i = 0; i < (size_t)w * h; i++) {
        unsigned char px[3];
        // clamp each channel into byte range
        for (int c = 0; c < 3; c++) {
            float v = host[i * 4u + (size_t)c];
            if (v < 0.0f) {
                v = 0.0f;
            }
            if (v > 1.0f) {
                v = 1.0f;
            }
            px[c] = (unsigned char)(v * 255.0f + 0.5f);
        }
        fwrite(px, 1, 3, fp);
    }
    // close output file
    fclose(fp);
    return 1;
}

// parse unsigned option with default fallback
static unsigned vsr_opt(int argc, char **argv, const char *name, unsigned fallback) {
    // scan argument pairs for match
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            return (unsigned)strtoul(argv[i + 1], NULL, 10);
        }
    }
    return fallback;
}

int main(int argc, char **argv) {
    // parse bench dimensions
    unsigned w = vsr_opt(argc, argv, "--w", 1280);
    unsigned h = vsr_opt(argc, argv, "--h", 720);
    unsigned scale = vsr_opt(argc, argv, "--scale", 2);
    unsigned iters = vsr_opt(argc, argv, "--iters", 30);
    // validate parsed values
    if (!w || !h || !scale || !iters || w > 4096 || h > 4096) {
        fprintf(stderr, "invalid dims\n");
        return 2;
    }
    // derive output dimensions
    unsigned ow = w * scale;
    unsigned oh = h * scale;
    // open cuda driver library
    void *cuda_handle = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
    // handle missing driver library
    if (!cuda_handle) {
        fprintf(stderr, "libcuda.so.1 not found\n");
        return 3;
    }
    // resolve driver entry points
    vsr_cuda_t cuda = {0};
    if (!vsr_cuda_open(cuda_handle, &cuda)) {
        fprintf(stderr, "cuda symbols missing\n");
        return 3;
    }
    // init driver and create context on device zero
    int dev = 0;
    void *ctx = NULL;
    if (cuda.init(0) || cuda.dev_get(&dev, 0) || cuda.ctx_create(&ctx, 0, dev)) {
        fprintf(stderr, "cuda init failed\n");
        return 3;
    }
    // allocate host pattern buffer
    float *host_in = (float *)malloc((size_t)w * h * 4u * sizeof(float));
    float *host_out = (float *)malloc((size_t)ow * oh * 4u * sizeof(float));
    // handle host allocation failure
    if (!host_in || !host_out) {
        fprintf(stderr, "host alloc failed\n");
        return 4;
    }
    // fill test pattern
    vsr_fill_pattern(host_in, w, h);
    // allocate device buffers for input and output
    unsigned long long dev_in = 0, dev_out = 0;
    if (cuda.mem_alloc(&dev_in, (size_t)w * h * 4u * sizeof(float))
        || cuda.mem_alloc(&dev_out, (size_t)ow * oh * 4u * sizeof(float))) {
        fprintf(stderr, "device alloc failed\n");
        return 4;
    }
    // upload pattern to device
    if (cuda.cpy_htod(dev_in, host_in, (size_t)w * h * 4u * sizeof(float))) {
        fprintf(stderr, "upload failed\n");
        return 4;
    }
    // init neural session at high quality
    vsr_rt_session_t *session = NULL;
    vsr_rt_status_t status = vsr_rt_init(&session, VSR_RT_HIGH);
    // handle session failure
    if (status != VSR_RT_OK) {
        fprintf(stderr, "vsr_rt_init: %s (sdk: %s)\n", vsr_rt_strerror(status), vsr_rt_sdk_path());
        return 5;
    }
    // load models into vram, timed once
    double t0 = vsr_now_ms();
    status = vsr_rt_load(session, ow, oh);
    double load_ms = vsr_now_ms() - t0;
    // handle load failure
    if (status != VSR_RT_OK) {
        fprintf(stderr, "vsr_rt_load: %s\n", vsr_rt_strerror(status));
        vsr_rt_destroy(session);
        return 5;
    }
    // warm up once outside timing
    status = vsr_rt_upscale(session, dev_in, w, h, dev_out);
    cuda.ctx_sync();
    // handle warmup failure
    if (status != VSR_RT_OK) {
        fprintf(stderr, "vsr_rt_upscale: %s\n", vsr_rt_strerror(status));
        vsr_rt_destroy(session);
        return 6;
    }
    // timed inference loop
    t0 = vsr_now_ms();
    for (unsigned i = 0; i < iters; i++) {
        status = vsr_rt_upscale(session, dev_in, w, h, dev_out);
        // abort loop on inference failure
        if (status != VSR_RT_OK) {
            break;
        }
        cuda.ctx_sync();
    }
    double total_ms = vsr_now_ms() - t0;
    // handle loop failure
    if (status != VSR_RT_OK) {
        fprintf(stderr, "vsr_rt_upscale: %s\n", vsr_rt_strerror(status));
        vsr_rt_destroy(session);
        return 6;
    }
    // download final frame for visual check
    cuda.cpy_dtoh(host_out, dev_out, (size_t)ow * oh * 4u * sizeof(float));
    cuda.ctx_sync();
    // write output image
    vsr_write_ppm("/tmp/vsr_rt_out.ppm", host_out, ow, oh);
    // report timings
    printf("load_ms=%.0f avg_ms=%.2f in=%ux%u out=%ux%u iters=%u\n",
        load_ms, total_ms / (double)iters, w, h, ow, oh, iters);
    // release neural session
    vsr_rt_destroy(session);
    // release device and host buffers
    cuda.mem_free(dev_in);
    cuda.mem_free(dev_out);
    free(host_in);
    free(host_out);
    // destroy cuda context
    cuda.ctx_destroy(ctx);
    return 0;
}
