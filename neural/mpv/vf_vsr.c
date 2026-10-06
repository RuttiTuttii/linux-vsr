// vf_vsr: native neural video super resolution for mpv via libvsr_rt
// chain: decoded cpu frame -> rgb24 -> cuda upload -> vsr inference -> rgb24
// realtime path with one host upload and one download per frame

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

#include "common/msg.h"
#include "filters/f_autoconvert.h"
#include "filters/filter.h"
#include "filters/filter_internal.h"
#include "filters/user_filters.h"
#include "video/img_format.h"
#include "video/mp_image.h"
#include "video/mp_image_pool.h"
#include <libavutil/frame.h>

// roi struct for npp geometry calls
struct vsr_nppi_size {
    int width;
    int height;
};

// npp entry point for nv12 to rgba conversion on device
struct vsr_npp {
    void *lib;
    int (*nv12_to_rgba)(const unsigned char *const *, int, unsigned char *, int, struct vsr_nppi_size);
};

#include "options/m_option.h"

// resolved neural runtime entry points from libvsr_rt
struct vsr_api {
    void *lib;
    int (*init)(void **, int);
    int (*load)(void *, unsigned, unsigned);
    int (*upscale)(void *, unsigned long long, unsigned, unsigned, unsigned long long);
    void (*destroy)(void *);
    const char *(*strerror)(int);
};

// cuda driver entry points loaded from system library
struct vsr_cuda {
    void *lib;
    int (*init)(unsigned int);
    int (*dev_get)(int *, int);
    int (*ctx_create)(void **, unsigned int, int);
    int (*mem_alloc)(unsigned long long *, size_t);
    int (*mem_free)(unsigned long long);
    int (*cpy_htod)(unsigned long long, const void *, size_t);
    int (*cpy_dtoh)(void *, unsigned long long, size_t);
    int (*ctx_sync)(void);
    int (*ctx_destroy)(void *);
};

// filter private state with persistent vram session
struct priv {
    struct mp_autoconvert *conv;
    struct vsr_api api;
    struct vsr_cuda cuda;
    struct vsr_npp npp;
    int npp_ready;
    void *session;
    void *cuda_ctx;
    unsigned long long dev_in;
    unsigned long long dev_out;
    unsigned long long dev_rgba;
    unsigned char *host_tmp;
    size_t dev_cap;
    size_t rgba_cap;
    int cur_w;
    int cur_h;
    int out_w;
    int out_h;
    int loaded;
    int quality;
    int scale;
    int cuda_ready;
    int api_ready;
};

// filter options from user string
struct vf_vsr_opts {
    int quality;
    int scale;
};

// load neural runtime library once per filter instance
static int vsr_api_open(struct vsr_api *api) {
    // resolve library path from env or default name
    const char *path = getenv("VSR_RT_LIB");
    if (!path || !path[0]) {
        path = "libvsr_rt.so";
    }
    // open shared runtime
    api->lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    // handle missing library
    if (!api->lib) {
        return 0;
    }
    // resolve symbols one by one
    api->init = dlsym(api->lib, "vsr_rt_init");
    api->load = dlsym(api->lib, "vsr_rt_load");
    api->upscale = dlsym(api->lib, "vsr_rt_upscale");
    api->destroy = dlsym(api->lib, "vsr_rt_destroy");
    api->strerror = dlsym(api->lib, "vsr_rt_strerror");
    // validate full set
    if (!api->init || !api->load || !api->upscale || !api->destroy || !api->strerror) {
        dlclose(api->lib);
        memset(api, 0, sizeof(*api));
        return 0;
    }
    return 1;
}

// load cuda driver entry points once
static int vsr_cuda_open(struct vsr_cuda *cuda) {
    // open system driver library
    cuda->lib = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
    // handle missing driver
    if (!cuda->lib) {
        return 0;
    }
    // resolve symbols one by one
    cuda->init = dlsym(cuda->lib, "cuInit");
    cuda->dev_get = dlsym(cuda->lib, "cuDeviceGet");
    cuda->ctx_create = dlsym(cuda->lib, "cuCtxCreate_v2");
    cuda->mem_alloc = dlsym(cuda->lib, "cuMemAlloc_v2");
    cuda->mem_free = dlsym(cuda->lib, "cuMemFree_v2");
    cuda->cpy_htod = dlsym(cuda->lib, "cuMemcpyHtoD_v2");
    cuda->cpy_dtoh = dlsym(cuda->lib, "cuMemcpyDtoH_v2");
    cuda->ctx_sync = dlsym(cuda->lib, "cuCtxSynchronize");
    cuda->ctx_destroy = dlsym(cuda->lib, "cuCtxDestroy_v2");
    // validate full set
    if (!cuda->init || !cuda->dev_get || !cuda->ctx_create || !cuda->mem_alloc
        || !cuda->mem_free || !cuda->cpy_htod || !cuda->cpy_dtoh
        || !cuda->ctx_sync || !cuda->ctx_destroy) {
        dlclose(cuda->lib);
        memset(cuda, 0, sizeof(*cuda));
        return 0;
    }
    return 1;
}

// load npp color conversion entry point once
static int vsr_npp_open(struct mp_filter *f, struct priv *priv) {
    // skip when already resolved
    if (priv->npp_ready) {
        return 1;
    }
    // open npp color library from loader path
    priv->npp.lib = dlopen("libnppicc.so.12", RTLD_NOW | RTLD_LOCAL);
    // handle missing library
    if (!priv->npp.lib) {
        MP_ERR(f, "vsr: libnppicc.so.12 missing\n");
        return 0;
    }
    // resolve conversion symbol
    priv->npp.nv12_to_rgba = dlsym(priv->npp.lib, "nppiNV12ToRGBA_8u_P2C4R");
    // handle missing symbol
    if (!priv->npp.nv12_to_rgba) {
        MP_ERR(f, "vsr: npp convert symbol missing\n");
        return 0;
    }
    priv->npp_ready = 1;
    return 1;
}

// upscale device rgba buffer and wrap result as output frame
// mutates frame payload on success, returns zero on success
static int vsr_run_and_wrap(struct mp_filter *f, struct priv *priv,
    struct mp_frame *frame, struct mp_image *img, unsigned long long dev_src) {
    // derive frame dimensions
    int w = img->w;
    int h = img->h;
    // run neural upscale on device
    if (priv->api.upscale(priv->session, dev_src, (unsigned)w, (unsigned)h, priv->dev_out)) {
        return 1;
    }
    priv->cuda.ctx_sync();
    // download upscaled frame reusing host scratch buffer
    int ow = priv->out_w;
    int oh = priv->out_h;
    priv->cuda.cpy_dtoh(priv->host_tmp, priv->dev_out, (size_t)ow * oh * 4u);
    priv->cuda.ctx_sync();
    // allocate output image at upscaled size
    struct mp_image *out = mp_image_alloc(IMGFMT_RGB24, ow, oh);
    // handle allocation failure
    if (!out) {
        return 1;
    }
    // copy color metadata from input
    out->params = img->params;
    out->params.w = ow;
    out->params.h = oh;
    // unpack rgba rows back to rgb24
    for (int y = 0; y < oh; y++) {
        // copy rgb channels skipping alpha
        unsigned char *src_row = priv->host_tmp + (size_t)y * ow * 4u;
        unsigned char *dst = out->planes[0] + (size_t)y * out->stride[0];
        for (int x = 0; x < ow; x++) {
            dst[x * 3u + 0] = src_row[x * 4u + 0];
            dst[x * 3u + 1] = src_row[x * 4u + 1];
            dst[x * 3u + 2] = src_row[x * 4u + 2];
        }
    }
    // replace frame payload with upscaled image
    talloc_free(frame->data);
    frame->data = out;
    return 0;
}

// process cuda frame fully on device without cpu roundtrip
static void vsr_cuda_frame(struct mp_filter *f, struct priv *priv, struct mp_frame frame) {
    struct mp_image *img = frame.data;
    int w = img->w;
    int h = img->h;
    // ensure backend ready for this size
    if (!vsr_backend_ensure(f, priv, w, h)) {
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }
    // ensure npp converter ready
    if (!vsr_npp_open(f, priv)) {
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }
    // map frame into av frame for device pointers
    AVFrame *av = mp_image_to_av_frame(img);
    // handle mapping failure
    if (!av || !av->data[0] || !av->data[1]) {
        if (av) {
            av_frame_free(&av);
        }
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }
    // grow rgba scratch buffer when too small
    size_t need_rgba = (size_t)w * h * 4u;
    if (need_rgba > priv->rgba_cap) {
        // release old scratch buffer
        if (priv->dev_rgba) {
            priv->cuda.mem_free(priv->dev_rgba);
            priv->dev_rgba = 0;
        }
        // allocate tight rgba buffer
        if (priv->cuda.mem_alloc(&priv->dev_rgba, need_rgba)) {
            av_frame_free(&av);
            mp_pin_in_write(f->ppins[1], frame);
            return;
        }
        priv->rgba_cap = need_rgba;
    }
    // convert nv12 planes to packed rgba on device
    const unsigned char *src[2] = {av->data[0], av->data[1]};
    struct vsr_nppi_size roi = {w, h};
    int npp_status = priv->npp.nv12_to_rgba(src, av->linesize[0],
        (unsigned char *)(size_t)priv->dev_rgba, w * 4, roi);
    // release av frame mapping
    av_frame_free(&av);
    // handle conversion failure
    if (npp_status) {
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }
    // point session input at converted buffer with tight pitch
    // run shared upscale and wrap tail
    if (vsr_run_and_wrap(f, priv, &frame, img, priv->dev_rgba)) {
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }
    // forward upscaled frame downstream
    mp_pin_in_write(f->ppins[1], frame);
}

// ensure backend session ready, init once and reconfigure on size change
static int vsr_backend_ensure(struct mp_filter *f, struct priv *priv, int w, int h) {
    // load cuda driver on first frame
    if (!priv->cuda_ready) {
        // open driver symbols
        if (!vsr_cuda_open(&priv->cuda)) {
            MP_ERR(f, "vsr: libcuda.so.1 missing\n");
            return 0;
        }
        // init driver context on device zero
        int dev = 0;
        if (priv->cuda.init(0) || priv->cuda.dev_get(&dev, 0)
            || priv->cuda.ctx_create(&priv->cuda_ctx, 0, dev)) {
            MP_ERR(f, "vsr: cuda init failed\n");
            return 0;
        }
        priv->cuda_ready = 1;
    }
    // open neural runtime on first frame
    if (!priv->api_ready) {
        // load shared runtime
        if (!vsr_api_open(&priv->api)) {
            MP_ERR(f, "vsr: libvsr_rt.so missing, set VSR_RT_LIB\n");
            return 0;
        }
        // init session at requested quality
        if (priv->api.init(&priv->session, priv->quality)) {
            MP_ERR(f, "vsr: neural init failed: %s\n", priv->api.strerror(1));
            return 0;
        }
        priv->api_ready = 1;
    }
    // derive output dimensions from scale
    int ow = w * priv->scale;
    int oh = h * priv->scale;
    // reconfigure session when size changed
    if (!priv->loaded || w != priv->cur_w || h != priv->cur_h) {
        // load models for new output size
        if (priv->api.load(priv->session, (unsigned)ow, (unsigned)oh)) {
            MP_ERR(f, "vsr: neural load failed\n");
            return 0;
        }
        // resize device buffers for new frame size
        size_t need_in = (size_t)w * h * 4u;
        size_t need_out = (size_t)ow * oh * 4u;
        size_t need = need_in > need_out ? need_in : need_out;
        // grow buffers when too small
        if (need > priv->dev_cap) {
            // release old buffers
            if (priv->dev_in) {
                priv->cuda.mem_free(priv->dev_in);
            }
            if (priv->dev_out) {
                priv->cuda.mem_free(priv->dev_out);
            }
            free(priv->host_tmp);
            // allocate fresh buffers
            priv->host_tmp = malloc(need);
            if (!priv->host_tmp
                || priv->cuda.mem_alloc(&priv->dev_in, need_in)
                || priv->cuda.mem_alloc(&priv->dev_out, need_out)) {
                MP_ERR(f, "vsr: buffer alloc failed\n");
                return 0;
            }
            priv->dev_cap = need;
        }
        // store active geometry
        priv->cur_w = w;
        priv->cur_h = h;
        priv->out_w = ow;
        priv->out_h = oh;
        priv->loaded = 1;
    }
    return 1;
}

// process frames: normalize input, upscale output
static void vf_vsr_process(struct mp_filter *f) {
    struct priv *priv = f->priv;
    // pull upstream frames into converter
    if (mp_pin_can_transfer_data(priv->conv->f->pins[0], f->ppins[0])) {
        struct mp_frame frame = mp_pin_out_read(f->ppins[0]);
        // route cuda frames to zero-copy device path
        if (frame.type == MP_FRAME_VIDEO && frame.data
            && ((struct mp_image *)frame.data)->imgfmt == IMGFMT_CUDA) {
            vsr_cuda_frame(f, priv, frame);
        } else if (frame.type == MP_FRAME_VIDEO) {
            struct mp_image *img = frame.data;
            if (img && IMGFMT_IS_HWACCEL(img->imgfmt)) {
                // copy hw surface into sw image
                struct mp_image *sw = mp_image_hw_download(img, NULL);
                // forward original when download fails
                if (!sw) {
                    mp_pin_in_write(priv->conv->f->pins[0], frame);
                } else {
                    talloc_free(frame.data);
                    frame.data = sw;
                    mp_pin_in_write(priv->conv->f->pins[0], frame);
                }
            } else {
                mp_pin_in_write(priv->conv->f->pins[0], frame);
            }
        } else {
            mp_pin_in_write(priv->conv->f->pins[0], frame);
        }
    }
    // handle converted frames from converter
    if (!mp_pin_can_transfer_data(f->ppins[1], priv->conv->f->pins[1])) {
        return;
    }
    // read converted frame
    struct mp_frame frame = mp_pin_out_read(priv->conv->f->pins[1]);
    // forward non-video frames untouched
    if (frame.type != MP_FRAME_VIDEO) {
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }
    // accept packed rgb24 cpu frames only
    struct mp_image *img = frame.data;
    if (img->imgfmt != IMGFMT_RGB24 || !img->planes[0]) {
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }
    // ensure backend ready for this size
    int w = img->w;
    int h = img->h;
    if (!vsr_backend_ensure(f, priv, w, h)) {
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }
    // pack rows into contiguous rgba host buffer
    for (int y = 0; y < h; y++) {
        // copy rgb row and pad alpha channel
        unsigned char *src = img->planes[0] + (size_t)y * img->stride[0];
        unsigned char *dst = priv->host_tmp + (size_t)y * w * 4u;
        for (int x = 0; x < w; x++) {
            dst[x * 4u + 0] = src[x * 3u + 0];
            dst[x * 4u + 1] = src[x * 3u + 1];
            dst[x * 4u + 2] = src[x * 3u + 2];
            dst[x * 4u + 3] = 255;
        }
    }
    // upload frame to device
    priv->cuda.cpy_htod(priv->dev_in, priv->host_tmp, (size_t)w * h * 4u);
    // run shared upscale and wrap tail
    if (vsr_run_and_wrap(f, priv, &frame, img, priv->dev_in)) {
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }
    // forward upscaled frame downstream
    mp_pin_in_write(f->ppins[1], frame);
}

// release backend resources on filter teardown
static void vf_vsr_destroy(struct mp_filter *f) {
    // fetch private state
    struct priv *priv = f->priv;
    // handle missing state gracefully
    if (!priv) {
        return;
    }
    // destroy neural session
    if (priv->api_ready && priv->session) {
        priv->api.destroy(priv->session);
    }
    // unload runtime library
    if (priv->api.lib) {
        dlclose(priv->api.lib);
    }
    // release device buffers
    if (priv->cuda_ready) {
        if (priv->dev_in) {
            priv->cuda.mem_free(priv->dev_in);
        }
        if (priv->dev_out) {
            priv->cuda.mem_free(priv->dev_out);
        }
        if (priv->dev_rgba) {
            priv->cuda.mem_free(priv->dev_rgba);
        }
        if (priv->cuda_ctx) {
            priv->cuda.ctx_destroy(priv->cuda_ctx);
        }
        if (priv->cuda.lib) {
            dlclose(priv->cuda.lib);
        }
    }
    // unload npp converter library
    if (priv->npp.lib) {
        dlclose(priv->npp.lib);
    }
    // release host scratch buffer
    free(priv->host_tmp);
}

// static filter descriptor with process callback
static const struct mp_filter_info vf_vsr_filter = {
    .name = "vsr",
    .process = vf_vsr_process,
    .priv_size = sizeof(struct priv),
    .destroy = vf_vsr_destroy,
};

// create filter instance with converter and options
static struct mp_filter *vf_vsr_create(struct mp_filter *parent, void *options) {
    // allocate filter node
    struct mp_filter *f = mp_filter_create(parent, &vf_vsr_filter);
    // handle allocation failure
    if (!f) {
        talloc_free(options);
        return NULL;
    }
    // bind options block
    struct priv *priv = f->priv;
    memset(priv, 0, sizeof(*priv));
    struct vf_vsr_opts *opts = talloc_steal(priv, options);
    priv->quality = opts->quality;
    priv->scale = opts->scale;
    // add filter pins
    mp_filter_add_pin(f, MP_PIN_IN, "in");
    mp_filter_add_pin(f, MP_PIN_OUT, "out");
    // create converter normalizing to rgb24 cpu frames
    priv->conv = mp_autoconvert_create(f);
    // handle converter failure
    if (!priv->conv) {
        talloc_free(f);
        return NULL;
    }
    // request packed rgb input for cuda upload
    mp_autoconvert_add_imgfmt(priv->conv, IMGFMT_RGB24, 0);
    return f;
}

// option table for vf vsr string
#define OPT_BASE_STRUCT struct vf_vsr_opts
static const m_option_t vf_opts_fields[] = {
    {"quality", OPT_INT(quality), M_RANGE(0, 4)},
    {"scale", OPT_INT(scale), M_RANGE(2, 4)},
    {0}
};

// filter registration entry for mpv
const struct mp_user_filter_entry vf_vsr = {
    .desc = {
        .description = "neural video super resolution via libvsr_rt",
        .name = "vsr",
        .priv_size = sizeof(OPT_BASE_STRUCT),
        .priv_defaults = &(const OPT_BASE_STRUCT){
            .quality = 3,
            .scale = 2,
        },
        .options = vf_opts_fields,
    },
    .create = vf_vsr_create,
};
