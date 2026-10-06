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

// filter private state with persistent vram session
struct priv {
    struct mp_autoconvert *conv;
    struct vsr_api api;
    void *session;
    int out_w;
    int out_h;
    int loaded;
    int quality;
    int scale;
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

// process frames: normalize input, upscale output
static void vf_vsr_process(struct mp_filter *f) {
    struct priv *priv = f->priv;
    // pull upstream frames into converter
    if (mp_pin_can_transfer_data(priv->conv->f->pins[0], f->ppins[0])) {
        struct mp_frame frame = mp_pin_out_read(f->ppins[0]);
        mp_pin_in_write(priv->conv->f->pins[0], frame);
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
    // TODO: cuda upload plus inference plus download lands here
    // passthrough until neural backend wires in
    mp_pin_in_write(f->ppins[1], frame);
}

// static filter descriptor with process callback
static const struct mp_filter_info vf_vsr_filter = {
    .name = "vsr",
    .process = vf_vsr_process,
    .priv_size = sizeof(struct priv),
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
