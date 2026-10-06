#ifndef VSR_RT_H
#define VSR_RT_H

#include <stddef.h>

// opaque neural upscaler session, model lives in vram after load
typedef struct vsr_rt_session vsr_rt_session_t;

// quality levels mirror nvidia vfx sdk video super resolution modes
typedef enum {
    VSR_RT_BICUBIC = 0,
    VSR_RT_LOW = 1,
    VSR_RT_MEDIUM = 2,
    VSR_RT_HIGH = 3,
    VSR_RT_ULTRA = 4,
    VSR_RT_DENOISE_HIGH = 10
} vsr_rt_quality_t;

// error codes for neural session lifecycle
typedef enum {
    VSR_RT_OK = 0,
    VSR_RT_ERR_ARGS = -1,
    VSR_RT_ERR_SDK_NOT_FOUND = -2,
    VSR_RT_ERR_SYMBOL = -3,
    VSR_RT_ERR_EFFECT = -4,
    VSR_RT_ERR_PARAM = -5,
    VSR_RT_ERR_GEOMETRY = -6,
    VSR_RT_ERR_LOAD = -7,
    VSR_RT_ERR_RUN = -8,
    VSR_RT_ERR_CUDA = -9
} vsr_rt_status_t;

// resolve sdk library path for diagnostics
const char *vsr_rt_sdk_path(void);

// translate status code to readable string
const char *vsr_rt_strerror(vsr_rt_status_t status);

// create session, load sdk and create effect (no vram models yet)
vsr_rt_status_t vsr_rt_init(vsr_rt_session_t **out, int quality);

// configure output size and load models into vram (slow once)
vsr_rt_status_t vsr_rt_load(vsr_rt_session_t *session, unsigned out_w, unsigned out_h);

// upscale one rgba f32 frame, device pointers stay on gpu, no copies inside
vsr_rt_status_t vsr_rt_upscale(vsr_rt_session_t *session,
    unsigned long long dev_in, unsigned in_w, unsigned in_h,
    unsigned long long dev_out);

// upscale with explicit row pitches, zero means tightly packed
vsr_rt_status_t vsr_rt_upscale_pitched(vsr_rt_session_t *session,
    unsigned long long dev_in, unsigned in_w, unsigned in_h, unsigned in_pitch,
    unsigned long long dev_out, unsigned out_pitch);

// release session and sdk handles
void vsr_rt_destroy(vsr_rt_session_t *session);

// report negotiated input geometry for caller buffer layout
void vsr_rt_in_geometry(const vsr_rt_session_t *session, int *format, int *type, unsigned *layout);

// report negotiated output geometry for caller buffer layout
void vsr_rt_out_geometry(const vsr_rt_session_t *session, int *format, int *type, unsigned *layout);

#endif // VSR_RT_H
