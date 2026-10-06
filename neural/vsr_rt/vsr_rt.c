#define _GNU_SOURCE
#include "vsr_rt.h"
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// nvidia status codes shared by nvcv and nvvfx apis
#define VSR_NVCV_SUCCESS 0
#define VSR_NVCV_ERR_SELECTOR -5
#define VSR_NVCV_ERR_MISMATCH -8

// pixel geometry constants from maxine nvcvimage api
#define VSR_NVCV_RGBA 6
#define VSR_NVCV_F32 7
#define VSR_NVCV_RGB 4
#define VSR_NVCV_CHUNKY 0
#define VSR_NVCV_PLANAR 1
#define VSR_NVCV_GPU 1

// effect and parameter selectors from vfx sdk
#define VSR_FX_SUPER_RES "VideoSuperRes"
#define VSR_PARAM_QUALITY "QualityLevel"
#define VSR_PARAM_STRENGTH "Strength"
#define VSR_PARAM_IN "SrcImage0"
#define VSR_PARAM_OUT "DstImage0"
#define VSR_PARAM_STREAM "CudaStream"

// image descriptor mirrors nvidia nvcvimage struct layout
typedef struct {
    unsigned int width;
    unsigned int height;
    int pitch;
    int pixel_format;
    int component_type;
    unsigned char pixel_bytes;
    unsigned char component_bytes;
    unsigned char num_components;
    unsigned char planar;
    unsigned char gpu_mem;
    unsigned char colorspace;
    unsigned char reserved[2];
    void *pixels;
    void *delete_ptr;
    void (*delete_proc)(void *p);
    unsigned long long buffer_bytes;
} vsr_nvcv_image_t;

// cuda stream handle is opaque driver pointer
typedef struct CUstream_st *vsr_cu_stream_t;
typedef void *vsr_effect_t;

// resolved host symbols from videofx library
typedef struct {
    int (*create)(const char *, vsr_effect_t *);
    int (*destroy)(vsr_effect_t);
    int (*set_u32)(vsr_effect_t, const char *, unsigned int);
    int (*set_f32)(vsr_effect_t, const char *, float);
    int (*set_image)(vsr_effect_t, const char *, vsr_nvcv_image_t *);
    int (*set_stream)(vsr_effect_t, const char *, vsr_cu_stream_t);
    int (*load)(vsr_effect_t);
    int (*run)(vsr_effect_t, int);
    int (*stream_create)(vsr_cu_stream_t *);
    int (*stream_destroy)(vsr_cu_stream_t);
    int (*img_alloc)(vsr_nvcv_image_t *, unsigned, unsigned, int, int, unsigned, unsigned, unsigned);
    int (*img_free)(vsr_nvcv_image_t *);
    const char *(*err_str)(int);
} vsr_host_api_t;

// session state with persistent vram resident effect
struct vsr_rt_session {
    void *sdk_handle;
    vsr_host_api_t api;
    vsr_effect_t effect;
    vsr_cu_stream_t stream;
    vsr_nvcv_image_t img_in;
    vsr_nvcv_image_t img_out;
    int has_in;
    int has_out;
    int loaded;
    int quality;
    unsigned out_w;
    unsigned out_h;
    char sdk_path[1024];
};

// last resolved sdk path for diagnostics
static char g_sdk_path[1024] = {0};

// resolve sdk library path for diagnostics
const char *vsr_rt_sdk_path(void) {
    // expose cached path string
    return g_sdk_path;
}

// translate status code to readable string
const char *vsr_rt_strerror(vsr_rt_status_t status) {
    // map each code to message
    switch (status) {
        case VSR_RT_OK: return "ok";
        case VSR_RT_ERR_ARGS: return "invalid arguments";
        case VSR_RT_ERR_SDK_NOT_FOUND: return "sdk library not found, set VSR_RT_SDK_DIR";
        case VSR_RT_ERR_SYMBOL: return "required sdk symbol missing";
        case VSR_RT_ERR_EFFECT: return "effect creation failed";
        case VSR_RT_ERR_PARAM: return "parameter rejected by effect";
        case VSR_RT_ERR_GEOMETRY: return "no working image geometry found";
        case VSR_RT_ERR_LOAD: return "model load into vram failed";
        case VSR_RT_ERR_RUN: return "inference run failed";
        case VSR_RT_ERR_CUDA: return "cuda operation failed";
        default: return "unknown error";
    }
}

// resolve one symbol or fail the whole set
static int vsr_resolve_one(void *handle, void **slot, const char *name) {
    // clear stale loader errors
    dlerror();
    // look symbol up in sdk library
    *slot = dlsym(handle, name);
    // validate resolution result
    if (!*slot) {
        return 0;
    }
    return 1;
}

// candidate sdk library filenames to probe
static const char *vsr_sdk_candidates[] = {
    "libVideoFX.so",
    NULL
};

// build candidate full path from directory and filename
static void vsr_join_path(char *out, size_t len, const char *dir, const char *file) {
    // handle empty directory as bare filename
    if (!dir || !dir[0]) {
        snprintf(out, len, "%s", file);
        return;
    }
    // join with separator
    snprintf(out, len, "%s/%s", dir, file);
}

// open sdk library from env dir or loader search path
static void *vsr_open_sdk(char *path_out, size_t path_len) {
    // read override directory from environment
    const char *dir = getenv("VSR_RT_SDK_DIR");
    // try each candidate filename
    for (size_t i = 0; vsr_sdk_candidates[i]; i++) {
        // try explicit directory first when set
        if (dir && dir[0]) {
            vsr_join_path(path_out, path_len, dir, vsr_sdk_candidates[i]);
            void *h = dlopen(path_out, RTLD_NOW | RTLD_LOCAL);
            if (h) {
                return h;
            }
        }
        // fall back to loader search path
        void *h = dlopen(vsr_sdk_candidates[i], RTLD_NOW | RTLD_LOCAL);
        if (h) {
            vsr_join_path(path_out, path_len, "", vsr_sdk_candidates[i]);
            return h;
        }
    }
    return NULL;
}

// create session, load sdk and create effect (no vram models yet)
vsr_rt_status_t vsr_rt_init(vsr_rt_session_t **out, int quality) {
    // validate output slot
    if (!out) {
        return VSR_RT_ERR_ARGS;
    }
    *out = NULL;
    // validate quality range
    if (quality < 0 || quality > 23) {
        return VSR_RT_ERR_ARGS;
    }
    // allocate zeroed session state
    vsr_rt_session_t *session = (vsr_rt_session_t *)calloc(1, sizeof(*session));
    // handle allocation failure
    if (!session) {
        return VSR_RT_ERR_ARGS;
    }
    // open sdk shared library
    session->sdk_handle = vsr_open_sdk(session->sdk_path, sizeof(session->sdk_path));
    // handle missing library
    if (!session->sdk_handle) {
        free(session);
        return VSR_RT_ERR_SDK_NOT_FOUND;
    }
    // remember path for diagnostics
    strncpy(g_sdk_path, session->sdk_path, sizeof(g_sdk_path) - 1);
    // resolve required entry points
    vsr_host_api_t *api = &session->api;
    int ok = 1;
    ok &= vsr_resolve_one(session->sdk_handle, (void **)&api->create, "NvVFX_CreateEffect");
    ok &= vsr_resolve_one(session->sdk_handle, (void **)&api->destroy, "NvVFX_DestroyEffect");
    ok &= vsr_resolve_one(session->sdk_handle, (void **)&api->set_u32, "NvVFX_SetU32");
    ok &= vsr_resolve_one(session->sdk_handle, (void **)&api->set_f32, "NvVFX_SetF32");
    ok &= vsr_resolve_one(session->sdk_handle, (void **)&api->set_image, "NvVFX_SetImage");
    ok &= vsr_resolve_one(session->sdk_handle, (void **)&api->load, "NvVFX_Load");
    ok &= vsr_resolve_one(session->sdk_handle, (void **)&api->run, "NvVFX_Run");
    ok &= vsr_resolve_one(session->sdk_handle, (void **)&api->img_alloc, "NvCVImage_Alloc");
    ok &= vsr_resolve_one(session->sdk_handle, (void **)&api->img_free, "NvCVImage_Dealloc");
    // optional symbols resolve best effort
    vsr_resolve_one(session->sdk_handle, (void **)&api->set_stream, "NvVFX_SetCudaStream");
    vsr_resolve_one(session->sdk_handle, (void **)&api->stream_create, "NvVFX_CudaStreamCreate");
    vsr_resolve_one(session->sdk_handle, (void **)&api->stream_destroy, "NvVFX_CudaStreamDestroy");
    vsr_resolve_one(session->sdk_handle, (void **)&api->err_str, "NvCV_GetErrorStringFromCode");
    // handle missing required symbols
    if (!ok) {
        dlclose(session->sdk_handle);
        free(session);
        return VSR_RT_ERR_SYMBOL;
    }
    // create super resolution effect instance
    int status = api->create(VSR_FX_SUPER_RES, &session->effect);
    // handle creation failure
    if (status != VSR_NVCV_SUCCESS || !session->effect) {
        dlclose(session->sdk_handle);
        free(session);
        return VSR_RT_ERR_EFFECT;
    }
    // apply quality level
    status = api->set_u32(session->effect, VSR_PARAM_QUALITY, (unsigned int)quality);
    // handle rejected quality value
    if (status != VSR_NVCV_SUCCESS) {
        api->destroy(session->effect);
        dlclose(session->sdk_handle);
        free(session);
        return VSR_RT_ERR_PARAM;
    }
    // apply full effect strength
    api->set_f32(session->effect, VSR_PARAM_STRENGTH, 1.0f);
    // create dedicated cuda stream when supported
    if (api->stream_create) {
        if (api->stream_create(&session->stream) == VSR_NVCV_SUCCESS && session->stream && api->set_stream) {
            api->set_stream(session->effect, VSR_PARAM_STREAM, session->stream);
        }
    }
    // store quality and publish session
    session->quality = quality;
    *out = session;
    return VSR_RT_OK;
}

// candidate input geometries to probe in order
static const struct {
    int format;
    int type;
    unsigned layout;
} vsr_in_candidates[] = {
    { VSR_NVCV_RGBA, VSR_NVCV_F32, VSR_NVCV_CHUNKY },
    { VSR_NVCV_RGB, VSR_NVCV_F32, VSR_NVCV_PLANAR },
};

// configure output size and load models into vram (slow once)
vsr_rt_status_t vsr_rt_load(vsr_rt_session_t *session, unsigned out_w, unsigned out_h) {
    // validate session and dimensions
    if (!session || !session->effect || out_w == 0 || out_h == 0) {
        return VSR_RT_ERR_ARGS;
    }
    // release previous images on reconfigure
    if (session->has_out) {
        session->api.img_free(&session->img_out);
        memset(&session->img_out, 0, sizeof(session->img_out));
        session->has_out = 0;
    }
    if (session->has_in) {
        session->api.img_free(&session->img_in);
        memset(&session->img_in, 0, sizeof(session->img_in));
        session->has_in = 0;
    }
    session->loaded = 0;
    // allocate output image at target size
    int status = session->api.img_alloc(&session->img_out, out_w, out_h,
        VSR_NVCV_RGBA, VSR_NVCV_F32, VSR_NVCV_CHUNKY, VSR_NVCV_GPU, 1);
    // handle allocation failure
    if (status != VSR_NVCV_SUCCESS) {
        return VSR_RT_ERR_CUDA;
    }
    session->has_out = 1;
    // bind output image to effect
    status = session->api.set_image(session->effect, VSR_PARAM_OUT, &session->img_out);
    // handle rejected output binding
    if (status != VSR_NVCV_SUCCESS) {
        return VSR_RT_ERR_GEOMETRY;
    }
    // probe input geometries until binding accepted
    int bound = 0;
    for (size_t i = 0; i < sizeof(vsr_in_candidates) / sizeof(vsr_in_candidates[0]); i++) {
        // use output dims as probe size, real dims set per run
        status = session->api.img_alloc(&session->img_in, out_w, out_h,
            vsr_in_candidates[i].format, vsr_in_candidates[i].type,
            vsr_in_candidates[i].layout, VSR_NVCV_GPU, 1);
        // skip failed allocations
        if (status != VSR_NVCV_SUCCESS) {
            continue;
        }
        // try binding candidate to effect
        status = session->api.set_image(session->effect, VSR_PARAM_IN, &session->img_in);
        // keep first accepted geometry
        if (status == VSR_NVCV_SUCCESS) {
            session->has_in = 1;
            bound = 1;
            break;
        }
        // release rejected candidate
        session->api.img_free(&session->img_in);
        memset(&session->img_in, 0, sizeof(session->img_in));
    }
    // handle no accepted geometry
    if (!bound) {
        return VSR_RT_ERR_GEOMETRY;
    }
    // load models into vram, slow on first call
    status = session->api.load(session->effect);
    // handle load failure
    if (status != VSR_NVCV_SUCCESS) {
        return VSR_RT_ERR_LOAD;
    }
    // store dimensions and mark ready
    session->out_w = out_w;
    session->out_h = out_h;
    session->loaded = 1;
    return VSR_RT_OK;
}

// upscale one rgba f32 frame, device pointers stay on gpu, no copies inside
vsr_rt_status_t vsr_rt_upscale(vsr_rt_session_t *session,
    unsigned long long dev_in, unsigned in_w, unsigned in_h,
    unsigned long long dev_out) {
    // validate session state and pointers
    if (!session || !session->effect || !session->loaded) {
        return VSR_RT_ERR_ARGS;
    }
    if (!dev_in || !dev_out || in_w == 0 || in_h == 0) {
        return VSR_RT_ERR_ARGS;
    }
    // point input descriptor at caller buffer without copying
    session->img_in.width = in_w;
    session->img_in.height = in_h;
    session->img_in.pixels = (void *)(size_t)dev_in;
    // bind input descriptor for this frame size
    int status = session->api.set_image(session->effect, VSR_PARAM_IN, &session->img_in);
    // handle rejected input binding
    if (status != VSR_NVCV_SUCCESS) {
        return VSR_RT_ERR_RUN;
    }
    // point output descriptor at caller buffer without copying
    session->img_out.pixels = (void *)(size_t)dev_out;
    // bind output descriptor for this frame
    status = session->api.set_image(session->effect, VSR_PARAM_OUT, &session->img_out);
    // handle rejected output binding
    if (status != VSR_NVCV_SUCCESS) {
        return VSR_RT_ERR_RUN;
    }
    // run synchronous inference on device
    status = session->api.run(session->effect, 0);
    // handle inference failure
    if (status != VSR_NVCV_SUCCESS) {
        return VSR_RT_ERR_RUN;
    }
    return VSR_RT_OK;
}

// release session and sdk handles
void vsr_rt_destroy(vsr_rt_session_t *session) {
    // handle null session gracefully
    if (!session) {
        return;
    }
    // destroy stream when created
    if (session->stream && session->api.stream_destroy) {
        session->api.stream_destroy(session->stream);
    }
    // destroy effect instance
    if (session->effect && session->api.destroy) {
        session->api.destroy(session->effect);
    }
    // release internal images
    if (session->has_in) {
        session->api.img_free(&session->img_in);
    }
    if (session->has_out) {
        session->api.img_free(&session->img_out);
    }
    // unload sdk library
    if (session->sdk_handle) {
        dlclose(session->sdk_handle);
    }
    // clear path cache when it belongs to this session
    if (strncmp(g_sdk_path, session->sdk_path, sizeof(g_sdk_path) - 1) == 0) {
        g_sdk_path[0] = '\0';
    }
    // release session memory
    free(session);
}
