/*
 * vita_sw3d.c - software 3D rasteriser (see vita_sw3d.h)
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "vita_sw3d.h"

#define SW3D_W_EPS 1e-4f

typedef struct {
    float x, y, z, w;
    float r, g, b, a;
    float u, v;
} clipv;

/* Screen-space vertex; colour is pre-multiplied by 1/w for perspective-
 * correct interpolation. */
typedef struct {
    float x, y, z, iw;
    float pr, pg, pb, pa;
    float pu, pv;
} scrv;

typedef struct {
    uint32_t *pix; /* ARGB, as Java hands it down */
    int w, h;
    int used;
    /* v04.35: Texture2D wrap mode. 1 = clamp to edge, 0 = repeat (default). */
    int clamp_s, clamp_t;
} swtex;

static int g_w = 0, g_h = 0;
static uint32_t *g_color = NULL;
static float *g_depth = NULL;
static float g_mvp[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
static swtex g_tex[SW3D_MAX_TEXTURES];
static int g_tex_bound = 0;
static int g_cull = 0;
/* v04.41: M3G Fog: mode 0/80/81, colour 0..255 per channel, near/far in world
 * units, density per unit. Mixed per pixel by eye-space depth (= 1/iw). */
static int g_fog_mode = 0;
static float g_fog_cr = 0.0f, g_fog_cg = 0.0f, g_fog_cb = 0.0f;
static float g_fog_near = 0.0f, g_fog_far = 1.0f, g_fog_density = 0.0f;
/* v04.02: M3G CompositingMode. g_blend keeps the raw mode (64..68) rather
 * than a bool, because ALPHA / ALPHA_ADD / MODULATE / MODULATE_X2 are four
 * different equations. The old bool meant "alpha over" for all of them. */
static int g_blend = 68;          /* V3D_BLEND_REPLACE */
static int g_depth_test = 1;
static int g_depth_write = 1;
static int g_color_write = 1;
static float g_alpha_threshold = 0.0f;   /* 0..1, 0 = always pass */
static uint32_t g_blend_color = 0;
static int g_sc_x0 = 0, g_sc_y0 = 0, g_sc_x1 = 0, g_sc_y1 = 0; /* exclusive */
static vita_sw3d_stats_t g_st;

#define SW3D_TRACE_MAX 32
static int g_trace_on = 0, g_trace_n = 0;
static vita_sw3d_drec_t g_trace[SW3D_TRACE_MAX];
static int g_wminx, g_wminy, g_wmaxx, g_wmaxy;
static float g_d_amax, g_d_first[6];
static unsigned g_d_small;
static int g_d_have;

void vita_sw3d_trace_arm(int on) {
    g_trace_on = on;
    if (on) {
        g_trace_n = 0;
    }
}

int vita_sw3d_trace_take(vita_sw3d_drec_t *out, int max) {
    int n = g_trace_n < max ? g_trace_n : max;
    memcpy(out, g_trace, (size_t)n * sizeof(*out));
    g_trace_n = 0;
    return n;
}

void vita_sw3d_take_stats(vita_sw3d_stats_t *out) {
    *out = g_st;
    g_st.draws = g_st.tri_in = g_st.tri_clip = g_st.tri_area0 = g_st.tri_cull = 0;
    g_st.tri_bbox = g_st.tri_rast = g_st.px_out = g_st.px_zr = g_st.px_zt = 0;
    g_st.px_wr = g_st.px_bright = g_st.px_tex = g_st.px_notex = 0;
    g_st.in_r = g_st.in_g = g_st.in_b = g_st.out_r = g_st.out_g = g_st.out_b = g_st.clears = 0;
}

int vita_sw3d_init(int w, int h) {
    if (g_color) {
        return 0;
    }
    g_color = (uint32_t *)malloc((size_t)w * h * sizeof(uint32_t));
    g_depth = (float *)malloc((size_t)w * h * sizeof(float));
    if (!g_color || !g_depth) {
        free(g_color);
        free(g_depth);
        g_color = NULL;
        g_depth = NULL;
        return -1;
    }
    g_w = w;
    g_h = h;
    g_sc_x0 = 0;
    g_sc_y0 = 0;
    g_sc_x1 = w;
    g_sc_y1 = h;
    return 0;
}

void vita_sw3d_clear(uint32_t rgba, int clear_depth, int clear_color) {
    size_t n = (size_t)g_w * g_h;
    if (!g_color) {
        return;
    }
    g_st.clears++;
    {
        int x, y;
        for (y = g_sc_y0; y < g_sc_y1; y++) {
            size_t row = (size_t)y * g_w;
            for (x = g_sc_x0; x < g_sc_x1; x++) {
                if (clear_color) {
                    g_color[row + x] = rgba;
                }
                if (clear_depth) {
                    g_depth[row + x] = 1.0f;
                }
            }
        }
    }
    (void)n;
}

void vita_sw3d_set_mvp(const float m[16]) {
    memcpy(g_mvp, m, sizeof(g_mvp));
}

static void xform(const float *p, clipv *o) {
    const float *m = g_mvp;
    o->x = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
    o->y = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
    o->z = m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14];
    o->w = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
}

/* Clip a convex polygon against w >= eps so projection is always safe; the
 * real near/far planes are enforced per pixel on z. Output holds up to n+1
 * vertices. */
static int clip_w(const clipv *in, int n, clipv *out) {
    int i, m = 0;
    for (i = 0; i < n; i++) {
        const clipv *a = &in[i];
        const clipv *b = &in[(i + 1) % n];
        float da = a->w - SW3D_W_EPS;
        float db = b->w - SW3D_W_EPS;
        if (da >= 0.0f) {
            out[m++] = *a;
        }
        if ((da >= 0.0f) != (db >= 0.0f)) {
            float t = da / (da - db);
            clipv *o = &out[m++];
            o->x = a->x + t * (b->x - a->x);
            o->y = a->y + t * (b->y - a->y);
            o->z = a->z + t * (b->z - a->z);
            o->w = a->w + t * (b->w - a->w);
            o->r = a->r + t * (b->r - a->r);
            o->g = a->g + t * (b->g - a->g);
            o->b = a->b + t * (b->b - a->b);
            o->a = a->a + t * (b->a - a->a);
            o->u = a->u + t * (b->u - a->u);
            o->v = a->v + t * (b->v - a->v);
        }
    }
    return m;
}

static void project(const clipv *c, scrv *s) {
    float iw = 1.0f / c->w;
    s->x = (c->x * iw * 0.5f + 0.5f) * (float)g_w;
    s->y = (0.5f - c->y * iw * 0.5f) * (float)g_h; /* top-down storage */
    s->z = c->z * iw;
    s->iw = iw;
    s->pr = c->r * iw;
    s->pg = c->g * iw;
    s->pb = c->b * iw;
    s->pa = c->a * iw;
    s->pu = c->u * iw;
    s->pv = c->v * iw;
}

static uint32_t texel(const swtex *t, float u, float v) {
    int x, y;
    /* v04.35: honour Texture2D.setWrapping(). REPEAT (M3G's default) takes the
     * fractional part; CLAMP keeps the coordinate so a UV that runs past 1.0
     * samples the edge texel instead of the opposite side of the atlas. */
    if (!t->clamp_s) u -= floorf(u);
    if (!t->clamp_t) v -= floorf(v);
    /* M3G puts the texture origin at the TOP-LEFT, so v=0 is row 0 (the
     * first row of the Image2D data) - unlike desktop GL. */
    x = (int)(u * (float)t->w);
    y = (int)(v * (float)t->h);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= t->w) x = t->w - 1;
    if (y >= t->h) y = t->h - 1;
    return t->pix[(size_t)y * t->w + x];
}

static uint32_t clamp8(float v) {
    int i = (int)(v + 0.5f);
    return (uint32_t)(i < 0 ? 0 : (i > 255 ? 255 : i));
}

static void raster(const scrv *a, const scrv *b, const scrv *c) {
    float area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    float minxf, maxxf, minyf, maxyf, inv;
    int minx, maxx, miny, maxy, x, y;

    if (area == 0.0f) {
        g_st.tri_area0++;
        return;
    }
    if (g_cull && area > 0.0f) {
        g_st.tri_cull++;
        return;
    }
    minxf = fminf(a->x, fminf(b->x, c->x));
    maxxf = fmaxf(a->x, fmaxf(b->x, c->x));
    minyf = fminf(a->y, fminf(b->y, c->y));
    maxyf = fmaxf(a->y, fmaxf(b->y, c->y));
    /* Clamp as floats first: a vertex far outside the screen must not
     * overflow the int conversion. */
    minx = (int)fmaxf(floorf(minxf), (float)g_sc_x0);
    miny = (int)fmaxf(floorf(minyf), (float)g_sc_y0);
    maxx = (int)fminf(ceilf(maxxf), (float)(g_sc_x1 - 1));
    maxy = (int)fminf(ceilf(maxyf), (float)(g_sc_y1 - 1));
    if (minx > maxx || miny > maxy) {
        g_st.tri_bbox++;
        return;
    }
    g_st.tri_rast++;
    {
        float aa = area < 0.0f ? -area : area;
        if (aa > g_d_amax) g_d_amax = aa;
        if (aa < 1.0f) g_d_small++;
        if (!g_d_have) {
            g_d_have = 1;
            g_d_first[0] = a->x; g_d_first[1] = a->y;
            g_d_first[2] = b->x; g_d_first[3] = b->y;
            g_d_first[4] = c->x; g_d_first[5] = c->y;
        }
    }
    inv = 1.0f / area;

    for (y = miny; y <= maxy; y++) {
        float py = (float)y + 0.5f;
        for (x = minx; x <= maxx; x++) {
            float px = (float)x + 0.5f;
            float w0 = ((b->x - px) * (c->y - py) - (b->y - py) * (c->x - px)) * inv;
            float w1 = ((c->x - px) * (a->y - py) - (c->y - py) * (a->x - px)) * inv;
            float w2 = 1.0f - w0 - w1;
            float z, iw, cr, cg, cb, ca;
            size_t idx;

            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) {
                g_st.px_out++;
                continue;
            }
            z = w0 * a->z + w1 * b->z + w2 * c->z;
            if (z < -1.0f || z > 1.0f) {
                g_st.px_zr++;
                continue;
            }
            idx = (size_t)y * g_w + x;
            if (g_depth_test && z >= g_depth[idx]) {
                g_st.px_zt++;
                continue;
            }
            iw = w0 * a->iw + w1 * b->iw + w2 * c->iw;
            iw = 1.0f / iw;
            cr = (w0 * a->pr + w1 * b->pr + w2 * c->pr) * iw;
            cg = (w0 * a->pg + w1 * b->pg + w2 * c->pg) * iw;
            cb = (w0 * a->pb + w1 * b->pb + w2 * c->pb) * iw;
            ca = (w0 * a->pa + w1 * b->pa + w2 * c->pa) * iw;
            if (g_tex_bound > 0 && g_tex_bound <= SW3D_MAX_TEXTURES &&
                g_tex[g_tex_bound - 1].used) {
                const swtex *t = &g_tex[g_tex_bound - 1];
                uint32_t tx = texel(t, (w0 * a->pu + w1 * b->pu + w2 * c->pu) * iw,
                                       (w0 * a->pv + w1 * b->pv + w2 * c->pv) * iw);
                cr = cr * (float)((tx >> 16) & 0xFF) / 255.0f;
                cg = cg * (float)((tx >> 8) & 0xFF) / 255.0f;
                cb = cb * (float)(tx & 0xFF) / 255.0f;
                ca = ca * (float)((tx >> 24) & 0xFF) / 255.0f;
                g_st.px_tex++;
            } else {
                g_st.px_notex++;
            }
            /* v04.41: M3G Fog. `iw` is interpolated 1/w, so 1/iw is the
             * eye-space depth: LINEAR mixes towards the fog colour between
             * near and far, EXPONENTIAL uses exp(-density*dist) - the same
             * curves GL_FOG uses on the hardware path. Applied to the final
             * shaded+textured colour, before the alpha test. */
            if (g_fog_mode != 0 && iw > 0.0f) {
                float dist = 1.0f / iw;
                float f;
                if (g_fog_mode == 81) {
                    float span = g_fog_far - g_fog_near;
                    f = (span != 0.0f) ? ((g_fog_far - dist) / span) : 1.0f;
                } else {
                    f = expf(-g_fog_density * dist);
                }
                if (f < 0.0f) {
                    f = 0.0f;
                } else if (f > 1.0f) {
                    f = 1.0f;
                }
                cr = f * cr + (1.0f - f) * g_fog_cr;
                cg = f * cg + (1.0f - f) * g_fog_cg;
                cb = f * cb + (1.0f - f) * g_fog_cb;
            }
            /* v04.02: JSR-184 alpha test. Evaluated on the FINAL alpha
             * (vertex * texture), which is what CompositingMode
             * .setAlphaThreshold is defined against. It must run before the
             * depth write: a discarded pixel must leave no depth behind, or
             * the foliage texture would occlude what sits behind its
             * transparent texels. */
            if (g_alpha_threshold > 0.0f && ca < g_alpha_threshold * 255.0f) {
                g_st.px_out++;
                continue;
            }
            if (g_depth_write) {
                g_depth[idx] = z;
            }
            if (g_color_write && g_blend != 68) {
                uint32_t d = g_color[idx];
                float sr = cr, sg = cg, sb = cb, sa = ca / 255.0f;
                float dr = (float)(d & 0xFF);
                float dg = (float)((d >> 8) & 0xFF);
                float db = (float)((d >> 16) & 0xFF);
                float da = (float)((d >> 24) & 0xFF);
                /* Modulate blends both operands with CompositingMode's
                 * blendColor in M3G 1.1; with the default 0x000000 that
                 * would make every MODULATE draw black, so blendColor is
                 * used only when the appearance actually set one. */
                float mr = 1.0f, mg = 1.0f, mb = 1.0f;
                if (g_blend_color != 0) {
                    mr = (float)((g_blend_color >> 16) & 0xFF) / 255.0f;
                    mg = (float)((g_blend_color >> 8) & 0xFF) / 255.0f;
                    mb = (float)(g_blend_color & 0xFF) / 255.0f;
                }
                switch (g_blend) {
                case 64: /* ALPHA: src*a + dst*(1-a) */
                    cr = sr * sa + dr * (1.0f - sa);
                    cg = sg * sa + dg * (1.0f - sa);
                    cb = sb * sa + db * (1.0f - sa);
                    ca = ca + da * (1.0f - sa);
                    break;
                case 65: /* ALPHA_ADD: src*a + dst */
                    cr = sr * sa + dr;
                    cg = sg * sa + dg;
                    cb = sb * sa + db;
                    ca = ca + da;
                    break;
                case 66: /* MODULATE: src*dst */
                    cr = sr * mr * dr * mr / 255.0f;
                    cg = sg * mg * dg * mg / 255.0f;
                    cb = sb * mb * db * mb / 255.0f;
                    ca = ca * da / 255.0f;
                    break;
                case 67: /* MODULATE_X2: 2*src*dst */
                    cr = 2.0f * sr * mr * dr * mr / 255.0f;
                    cg = 2.0f * sg * mg * dg * mg / 255.0f;
                    cb = 2.0f * sb * mb * db * mb / 255.0f;
                    ca = 2.0f * ca * da / 255.0f;
                    break;
                default:
                    break;
                }
            }
            {
                uint32_t or8 = clamp8(cr), og8 = clamp8(cg), ob8 = clamp8(cb);
                g_color[idx] = (clamp8(ca) << 24) | (ob8 << 16) | (og8 << 8) | or8;
                g_st.px_wr++;
                if (x < g_wminx) g_wminx = x;
                if (x > g_wmaxx) g_wmaxx = x;
                if (y < g_wminy) g_wminy = y;
                if (y > g_wmaxy) g_wmaxy = y;
                if (or8 > g_st.out_r) g_st.out_r = or8;
                if (og8 > g_st.out_g) g_st.out_g = og8;
                if (ob8 > g_st.out_b) g_st.out_b = ob8;
                if (or8 > 32 || og8 > 32 || ob8 > 32) g_st.px_bright++;
            }
        }
    }
}

static void draw_impl(const float *pos3, const float *uv2,
                      const uint8_t *rgba, int nverts) {
    int i;
    unsigned tri0, rast0, wr0, zt0, out0, zr0;
    float pmnx = 1e30f, pmny = 1e30f, pmxx = -1e30f, pmxy = -1e30f;
    if (!g_color) {
        return;
    }
    g_st.draws++;
    tri0 = g_st.tri_in;
    rast0 = g_st.tri_rast;
    wr0 = g_st.px_wr;
    zt0 = g_st.px_zt;
    out0 = g_st.px_out;
    zr0 = g_st.px_zr;
    g_d_amax = 0.0f;
    g_d_small = 0;
    g_d_have = 0;
    g_wminx = g_wminy = 1 << 30;
    g_wmaxx = g_wmaxy = -1;
    for (i = 0; i + 2 < nverts; i += 3) {
        clipv in[3], out[4];
        scrv s[4];
        int k, n;
        g_st.tri_in++;
        for (k = 0; k < 3; k++) {
            const uint8_t *c = rgba + (size_t)(i + k) * 4;
            if (c[0] > g_st.in_r) g_st.in_r = c[0];
            if (c[1] > g_st.in_g) g_st.in_g = c[1];
            if (c[2] > g_st.in_b) g_st.in_b = c[2];
            xform(pos3 + (size_t)(i + k) * 3, &in[k]);
            in[k].r = c[0];
            in[k].g = c[1];
            in[k].b = c[2];
            in[k].a = c[3];
            in[k].u = (uv2 != NULL) ? uv2[(size_t)(i + k) * 2] : 0.0f;
            in[k].v = (uv2 != NULL) ? uv2[(size_t)(i + k) * 2 + 1] : 0.0f;
        }
        n = clip_w(in, 3, out);
        if (n < 3) {
            g_st.tri_clip++;
            continue;
        }
        for (k = 0; k < n; k++) {
            project(&out[k], &s[k]);
            if (s[k].x < pmnx) pmnx = s[k].x;
            if (s[k].x > pmxx) pmxx = s[k].x;
            if (s[k].y < pmny) pmny = s[k].y;
            if (s[k].y > pmxy) pmxy = s[k].y;
        }
        for (k = 1; k + 1 < n; k++) {
            raster(&s[0], &s[k], &s[k + 1]);
        }
    }
    if (g_trace_on && g_trace_n < SW3D_TRACE_MAX) {
        vita_sw3d_drec_t *r = &g_trace[g_trace_n++];
        r->nverts = (unsigned)nverts;
        r->tex = (unsigned)g_tex_bound;
        r->cull = (unsigned)g_cull;
        r->blend = (unsigned)g_blend;
        r->tri_in = g_st.tri_in - tri0;
        r->tri_rast = g_st.tri_rast - rast0;
        r->px_wr = g_st.px_wr - wr0;
        r->px_zt = g_st.px_zt - zt0;
        r->px_out = g_st.px_out - out0;
        r->px_zr = g_st.px_zr - zr0;
        r->tri_small = g_d_small;
        r->amax = g_d_amax;
        memcpy(r->first, g_d_first, sizeof(r->first));
        r->pminx = (int)pmnx;
        r->pminy = (int)pmny;
        r->pmaxx = (int)pmxx;
        r->pmaxy = (int)pmxy;
        r->wminx = g_wminx;
        r->wminy = g_wminy;
        r->wmaxx = g_wmaxx;
        r->wmaxy = g_wmaxy;
    }
}

void vita_sw3d_draw(const float *pos3, const uint8_t *rgba, int nverts) {
    draw_impl(pos3, NULL, rgba, nverts);
}

void vita_sw3d_draw_uv(const float *pos3, const float *uv2,
                       const uint8_t *rgba, int nverts) {
    /* v04.10: see vita_gl3d_draw_uv - "texture bound, no texcoords" is a
     * reachable combination (M3G allows it) and the GL path used to read
     * address 0 for it. Here the equivalent failure is silent: draw_impl
     * substitutes u=v=0 for every vertex, so the whole mesh samples texel
     * (0,0) and comes out as one flat colour. Drop the texture for this call
     * instead, so the vertex/material colour shows through - same policy as
     * the GL backend. */
    int saved = g_tex_bound;
    if (uv2 == NULL) {
        g_tex_bound = 0;
    }
    draw_impl(pos3, uv2, rgba, nverts);
    g_tex_bound = saved;
}

int vita_sw3d_texture_create(const uint32_t *argb, int w, int h) {
    int i;
    swtex *t;
    if (!argb || w <= 0 || h <= 0) {
        g_st.tex_fail++;
        return 0;
    }
    for (i = 0; i < SW3D_MAX_TEXTURES; i++) {
        if (!g_tex[i].used) {
            break;
        }
    }
    if (i == SW3D_MAX_TEXTURES) {
        g_st.tex_fail++;
        return 0;
    }
    t = &g_tex[i];
    t->pix = (uint32_t *)malloc((size_t)w * h * sizeof(uint32_t));
    if (!t->pix) {
        g_st.tex_fail++;
        return 0;
    }
    memcpy(t->pix, argb, (size_t)w * h * sizeof(uint32_t));
    {
        size_t k, n = (size_t)w * h, bright = 0, step = n > 4096 ? n / 4096 : 1;
        for (k = 0; k < n; k += step) {
            uint32_t p = argb[k];
            if (((p >> 16) & 0xFF) > 32 || ((p >> 8) & 0xFF) > 32 || (p & 0xFF) > 32) {
                bright++;
            }
        }
        if (bright == 0) {
            g_st.tex_dark++;
        }
        g_st.tex_ok++;
    }
    t->w = w;
    t->h = h;
    t->used = 1;
    t->clamp_s = 0;
    t->clamp_t = 0;
    return i + 1;
}

void vita_sw3d_texture_bind(int handle) {
    g_tex_bound = (handle > 0 && handle <= SW3D_MAX_TEXTURES &&
                   g_tex[handle - 1].used) ? handle : 0;
}

/* v04.35: wrap mode of the bound texture (see vita_3d.h). */
void vita_sw3d_texture_wrap(int clamp_s, int clamp_t) {
    swtex *t;

    if (g_tex_bound <= 0 || g_tex_bound > SW3D_MAX_TEXTURES) {
        return;
    }
    t = &g_tex[g_tex_bound - 1];
    t->clamp_s = clamp_s ? 1 : 0;
    t->clamp_t = clamp_t ? 1 : 0;
}

/* v04.41: M3G Fog (see vita_3d.h). Same two curves as the GL path. */
void vita_sw3d_fog(int mode, int color, int near256, int far256,
                   int density1000) {
    g_fog_mode = mode;
    g_fog_cr = (float) ((color >> 16) & 0xFF);
    g_fog_cg = (float) ((color >> 8) & 0xFF);
    g_fog_cb = (float) (color & 0xFF);
    g_fog_near = (float) near256 / 256.0f;
    g_fog_far = (float) far256 / 256.0f;
    g_fog_density = (float) density1000 / 1000.0f;
}

void vita_sw3d_texture_free(int handle) {
    if (handle <= 0 || handle > SW3D_MAX_TEXTURES) {
        return;
    }
    free(g_tex[handle - 1].pix);
    memset(&g_tex[handle - 1], 0, sizeof(swtex));
    if (g_tex_bound == handle) {
        g_tex_bound = 0;
    }
}

void vita_sw3d_set_scissor(int x, int y, int w, int h) {
    g_sc_x0 = (x < 0) ? 0 : x;
    g_sc_y0 = (y < 0) ? 0 : y;
    g_sc_x1 = (x + w > g_w) ? g_w : x + w;
    g_sc_y1 = (y + h > g_h) ? g_h : y + h;
    if (w <= 0 || h <= 0) {
        /* empty scissor: nothing may be drawn */
        g_sc_x1 = g_sc_x0;
        g_sc_y1 = g_sc_y0;
    }
}

void vita_sw3d_set_state(int cull, int blend, int depth_test, int depth_write,
                         int color_write, float alpha_threshold,
                         uint32_t blend_color) {
    g_cull = cull ? 1 : 0;
    /* Unknown mode falls back to REPLACE, matching the GL path. */
    g_blend = (blend >= 64 && blend <= 68) ? blend : 68;
    g_depth_test = depth_test ? 1 : 0;
    g_depth_write = depth_write ? 1 : 0;
    g_color_write = color_write ? 1 : 0;
    if (alpha_threshold < 0.0f) {
        alpha_threshold = 0.0f;
    } else if (alpha_threshold > 1.0f) {
        alpha_threshold = 1.0f;
    }
    g_alpha_threshold = alpha_threshold;
    g_blend_color = blend_color & 0x00FFFFFFu;
}

unsigned vita_sw3d_present(uint32_t *dst, int dst_stride, int x, int y) {
    int row;
    if (!g_color || !dst) {
        return 0;
    }
    for (row = 0; row < g_h; row++) {
        memcpy(dst + (size_t)(y + row) * dst_stride + x,
               g_color + (size_t)row * g_w, (size_t)g_w * 4);
    }
    return 0;
}
