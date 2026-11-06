/*
 * vita_3d.c - 3D backend selection (see vita_3d.h)
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>

#include "vita_3d.h"
#include "vita_gl3d.h"
#include "vita_sw3d.h"
#include "vita_crumb.h"

#define V3D_DEFAULTS_CFG "ux0:/data/J2ME00001/defaults.cfg"

static int g_backend = V3D_BACKEND_NONE;

int v3d_configured_mode(void) {
    char buf[512];
    SceUID fd = sceIoOpen(V3D_DEFAULTS_CFG, SCE_O_RDONLY, 0);
    const char *p;
    int n;

    if (fd < 0) {
        return V3D_MODE_AUTO;
    }
    n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0) {
        return V3D_MODE_AUTO;
    }
    buf[n] = '\0';
    for (p = buf; (p = strstr(p, "3d=")) != NULL; p += 3) {
        if (p == buf || p[-1] == '\n') {
            if (strncmp(p + 3, "hw", 2) == 0) {
                return V3D_MODE_HW;
            }
            if (strncmp(p + 3, "sw", 2) == 0) {
                return V3D_MODE_SW;
            }
            return V3D_MODE_AUTO;
        }
    }
    return V3D_MODE_AUTO;
}

/* Value after "key=" when the key starts a line, else NULL. */
static const char *cfg_value(const char *buf, const char *key) {
    size_t kl = strlen(key);
    const char *p;
    for (p = buf; (p = strstr(p, key)) != NULL; p += kl) {
        if (p == buf || p[-1] == '\n') {
            return p + kl;
        }
    }
    return NULL;
}

int v3d_configured_cam(void) {
    char buf[512];
    SceUID fd = sceIoOpen(V3D_DEFAULTS_CFG, SCE_O_RDONLY, 0);
    const char *v;
    int n, cam = 0;

    if (fd < 0) {
        return 0;
    }
    n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0) {
        return 0;
    }
    buf[n] = '\0';
    v = cfg_value(buf, "m3gcam=");
    if (v != NULL) {
        if (strncmp(v, "direct", 6) == 0) {
            cam = 1;
        } else if (strncmp(v, "invt", 4) == 0) {
            cam = 2;
        }
    }
    /* v03.84: m3gnative=0 turns the native skinning kernel off (Java path), for A/B. Bit 16. */
    v = cfg_value(buf, "m3gnative=");
    if (v != NULL && v[0] == '0') {
        cam |= 1 << 16;
    }
    /* v04.20: m3gshade=0 keeps only the lighting loop in Java (bit 17), for A/B. */
    v = cfg_value(buf, "m3gshade=");
    if (v != NULL && v[0] == '0') {
        cam |= 1 << 17;
    }
    /* v04.28: m3gtex=0 uploads textures exactly as stored (no alpha bleeding), for A/B. Bit 18. */
    v = cfg_value(buf, "m3gtex=");
    if (v != NULL && v[0] == '0') {
        cam |= 1 << 18;
    }
    return cam;
}

const char *v3d_backend_name(int backend) {
    return backend == V3D_BACKEND_HW ? "hardware"
         : backend == V3D_BACKEND_SW ? "software" : "none";
}

int v3d_backend(void) {
    return g_backend;
}

int v3d_init(int mode, int w, int h) {
    int rc;

    if (g_backend != V3D_BACKEND_NONE) {
        return g_backend;
    }
    if (mode == V3D_MODE_AUTO || mode == V3D_MODE_HW) {
        rc = vita_gl3d_init(w, h);
        if (rc == GL3D_OK) {
            g_backend = V3D_BACKEND_HW;
            crumb_printf("3d: backend=hardware (mode=%d)", mode);
            return g_backend;
        }
        crumb_printf("3d: hardware init failed rc=%d%s", rc,
                     mode == V3D_MODE_AUTO ? ", falling back to software" : "");
        if (mode == V3D_MODE_HW) {
            return rc;
        }
    }
    rc = vita_sw3d_init(w, h);
    if (rc != 0) {
        crumb_printf("3d: software init failed rc=%d", rc);
        return rc;
    }
    g_backend = V3D_BACKEND_SW;
    crumb_printf("3d: backend=software (mode=%d)", mode);
    return g_backend;
}

void v3d_clear(uint32_t rgba, int clear_depth, int clear_color) {
    if (g_backend == V3D_BACKEND_HW) {
        vita_gl3d_clear(rgba, clear_depth, clear_color);
    } else if (g_backend == V3D_BACKEND_SW) {
        vita_sw3d_clear(rgba, clear_depth, clear_color);
    }
}

void v3d_set_mvp(const float m[16]) {
    if (g_backend == V3D_BACKEND_HW) {
        vita_gl3d_set_mvp(m);
    } else if (g_backend == V3D_BACKEND_SW) {
        vita_sw3d_set_mvp(m);
    }
}

void v3d_draw(const float *pos3, const uint8_t *rgba, int nverts) {
    if (g_backend == V3D_BACKEND_HW) {
        vita_gl3d_draw(pos3, rgba, nverts);
    } else if (g_backend == V3D_BACKEND_SW) {
        vita_sw3d_draw(pos3, rgba, nverts);
    }
}

int v3d_texture_create(const uint32_t *argb, int w, int h) {
    if (g_backend == V3D_BACKEND_HW) {
        return vita_gl3d_texture_create(argb, w, h);
    }
    if (g_backend == V3D_BACKEND_SW) {
        return vita_sw3d_texture_create(argb, w, h);
    }
    return 0;
}

void v3d_texture_bind(int handle) {
    if (g_backend == V3D_BACKEND_HW) {
        vita_gl3d_texture_bind(handle);
    } else if (g_backend == V3D_BACKEND_SW) {
        vita_sw3d_texture_bind(handle);
    }
}

void v3d_texture_filter(int nearest) {
    if (g_backend == V3D_BACKEND_HW) {
        vita_gl3d_texture_filter(nearest);
    }
}

/* v04.35: wrap mode of the bound texture (see vita_3d.h). */
void v3d_texture_wrap(int clamp_s, int clamp_t) {
    if (g_backend == V3D_BACKEND_HW) {
        vita_gl3d_texture_wrap(clamp_s, clamp_t);
    } else if (g_backend == V3D_BACKEND_SW) {
        vita_sw3d_texture_wrap(clamp_s, clamp_t);
    }
}

/* v04.41: M3G Fog (see vita_3d.h). */
void v3d_fog(int mode, int color, int near256, int far256, int density1000) {
    if (g_backend == V3D_BACKEND_HW) {
        vita_gl3d_fog(mode, color, near256, far256, density1000);
    } else if (g_backend == V3D_BACKEND_SW) {
        vita_sw3d_fog(mode, color, near256, far256, density1000);
    }
}

void v3d_texture_free(int handle) {
    if (g_backend == V3D_BACKEND_HW) {
        vita_gl3d_texture_free(handle);
    } else if (g_backend == V3D_BACKEND_SW) {
        vita_sw3d_texture_free(handle);
    }
}

void v3d_set_state(int cull, int blend, int depth_test, int depth_write,
                   int color_write, float alpha_threshold,
                   uint32_t blend_color) {
    if (g_backend == V3D_BACKEND_HW) {
        vita_gl3d_set_state(cull, blend, depth_test, depth_write, color_write,
                            alpha_threshold, blend_color);
    } else if (g_backend == V3D_BACKEND_SW) {
        vita_sw3d_set_state(cull, blend, depth_test, depth_write, color_write,
                            alpha_threshold, blend_color);
    }
}

void v3d_set_scissor(int x, int y, int w, int h) {
    if (g_backend == V3D_BACKEND_HW) {
        vita_gl3d_set_scissor(x, y, w, h);
    } else if (g_backend == V3D_BACKEND_SW) {
        vita_sw3d_set_scissor(x, y, w, h);
    }
}

void v3d_draw_uv(const float *pos3, const float *uv2,
                 const uint8_t *rgba, int nverts) {
    if (g_backend == V3D_BACKEND_HW) {
        vita_gl3d_draw_uv(pos3, uv2, rgba, nverts);
    } else if (g_backend == V3D_BACKEND_SW) {
        vita_sw3d_draw_uv(pos3, uv2, rgba, nverts);
    }
}

/* Independent of m3g_native.c's probe (which stops after 8 reports, all of them in the first seconds): sample the
 * rasteriser counters on frames 1-3 and then every 60th, so gameplay frames are covered too. */
static void sw3d_report(unsigned long long frame) {
    vita_sw3d_stats_t s;
    vita_sw3d_take_stats(&s);
    crumb_printf("sw3d#%llu dr=%u tri=%u clip=%u a0=%u cull=%u bb=%u rast=%u", frame,
                 s.draws, s.tri_in, s.tri_clip, s.tri_area0, s.tri_cull, s.tri_bbox, s.tri_rast);
    crumb_printf("sw3d#%llu px wr=%u out=%u zr=%u zt=%u bright=%u tex=%u notex=%u", frame,
                 s.px_wr, s.px_out, s.px_zr, s.px_zt, s.px_bright, s.px_tex, s.px_notex);
    crumb_printf("sw3d#%llu in=%u,%u,%u out=%u,%u,%u tex ok=%u fail=%u dark=%u clr=%u", frame,
                 s.in_r, s.in_g, s.in_b, s.out_r, s.out_g, s.out_b,
                 s.tex_ok, s.tex_fail, s.tex_dark, s.clears);
}

/* Dump the per-draw records of the frame armed on the previous present. */
static void sw3d_trace_dump(unsigned long long frame) {
    vita_sw3d_drec_t r[32];
    int i, n = vita_sw3d_trace_take(r, 32);
    for (i = 0; i < n; i++) {
        crumb_printf("sw3d#%llu d%d nv=%u tex=%u c=%u b=%u tri=%u rast=%u wr=%u zt=%u proj[%d,%d..%d,%d] wrote[%d,%d..%d,%d]",
                     frame, i, r[i].nverts, r[i].tex, r[i].cull, r[i].blend, r[i].tri_in, r[i].tri_rast,
                     r[i].px_wr, r[i].px_zt, r[i].pminx, r[i].pminy, r[i].pmaxx, r[i].pmaxy,
                     r[i].wminx, r[i].wminy, r[i].wmaxx, r[i].wmaxy);
        /* x10 integers: crumb_printf's buffer is small and %f may not be linked in */
        crumb_printf("sw3d#%llu d%d out=%u zr=%u small=%u amax10=%d first10=(%d,%d)(%d,%d)(%d,%d)",
                     frame, i, r[i].px_out, r[i].px_zr, r[i].tri_small, (int)(r[i].amax * 10.0f),
                     (int)(r[i].first[0] * 10.0f), (int)(r[i].first[1] * 10.0f),
                     (int)(r[i].first[2] * 10.0f), (int)(r[i].first[3] * 10.0f),
                     (int)(r[i].first[4] * 10.0f), (int)(r[i].first[5] * 10.0f));
    }
}

unsigned v3d_present(uint32_t *dst, int dst_stride, int x, int y) {
    static unsigned long long frames = 0;
    if (g_backend == V3D_BACKEND_HW) {
        return vita_gl3d_present(dst, dst_stride, x, y);
    }
    if (g_backend == V3D_BACKEND_SW) {
        unsigned r = vita_sw3d_present(dst, dst_stride, x, y);
        frames++;
        if (frames <= 3 || (frames % 60) == 0) {
            sw3d_report(frames);
        }
        if (frames > 3 && (frames % 60) == 0) {
            sw3d_trace_dump(frames);
        }
        vita_sw3d_trace_arm(frames >= 59 && (frames % 60) == 59 && frames < 400);
        return r;
    }
    return 0;
}

int v3d_selftest(int frames) {
    static const float verts[9] = {
        0.0f, 0.8f, 0.0f, -0.8f, -0.8f, 0.0f, 0.8f, -0.8f, 0.0f
    };
    static const uint8_t cols[12] = {
        255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255
    };
    const uint32_t bg = 0xFF4D1A1Au; /* A8B8G8R8: R=26 G=26 B=77 */
    const int W = 240, H = 320;
    int mode = v3d_configured_mode();
    int rc, i, backend;
    unsigned long long t_draw = 0, t_pres = 0;
    unsigned covered = 0;
    uint32_t *scratch;

    rc = v3d_init(mode, W, H);
    if (rc < 0) {
        crumb_printf("3d: selftest init failed mode=%d rc=%d", mode, rc);
        return rc;
    }
    backend = rc;
    scratch = (uint32_t *)malloc((size_t)W * H * sizeof(uint32_t));
    if (!scratch) {
        return -1;
    }

    for (i = 0; i < frames; i++) {
        float a = (float)i * 0.05f;
        float c = cosf(a), s = sinf(a);
        const float m[16] = { c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        unsigned long long t0 = sceKernelGetProcessTimeWide();
        unsigned long long t1;

        v3d_clear(bg, 1, 1);
        v3d_set_mvp(m);
        v3d_draw(verts, cols, 3);
        t1 = sceKernelGetProcessTimeWide();
        v3d_present(scratch, W, 0, 0);
        t_draw += t1 - t0;
        t_pres += sceKernelGetProcessTimeWide() - t1;
    }

    for (i = 0; i < W * H; i++) {
        if (scratch[i] != bg) {
            covered++;
        }
    }
    /* 32% of clip space when fully on screen, slightly less once a corner
     * rotates past the edge; 0% means nothing was drawn. */
    crumb_printf("3d: selftest backend=%s frames=%d draw=%llu us/f "
                 "present=%llu us/f centre=0x%08x bg=0x%08x covered=%u%%",
                 v3d_backend_name(backend), frames,
                 t_draw / (unsigned long long)frames,
                 t_pres / (unsigned long long)frames,
                 scratch[(H / 2) * W + W / 2], bg,
                 covered * 100u / (unsigned)(W * H));
    free(scratch);
    return backend;
}

void v3d_trace_present(unsigned long long n, int distinct, int total,
                       unsigned first, unsigned centre, int alpha) {
    crumb_printf("3d: present #%llu distinct=%d/%d first=0x%08x"
                 " centre=0x%08x alpha!=0=%d backend=%s",
                 n, distinct, total, first, centre, alpha,
                 v3d_backend_name(g_backend));
}
