/*
 * Master of Puppets — Three-Way Backend Comparison
 *
 * Renders the same cube scene with CPU, OpenGL, and Vulkan backends,
 * then compares all three pairs pixel-by-pixel.
 *
 * On macOS, creates a CGL offscreen context for the OpenGL backend.
 * On Linux, creates an EGL offscreen context.
 *
 * Build:
 *   make compare  (from examples/)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <mop/mop.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* -------------------------------------------------------------------------
 * Platform-specific headless GL context
 * ------------------------------------------------------------------------- */

#if defined(__APPLE__) && defined(MOP_HAS_OPENGL)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>

static CGLContextObj g_cgl_ctx;

static int gl_context_create(void) {
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
        kCGLPFAColorSize,     (CGLPixelFormatAttribute)24,
        kCGLPFADepthSize,     (CGLPixelFormatAttribute)24,
        kCGLPFAAllowOfflineRenderers,
        (CGLPixelFormatAttribute)0
    };

    CGLPixelFormatObj pf;
    GLint npix;
    if (CGLChoosePixelFormat(attrs, &pf, &npix) != kCGLNoError) return 0;

    CGLError err = CGLCreateContext(pf, NULL, &g_cgl_ctx);
    CGLDestroyPixelFormat(pf);
    if (err != kCGLNoError) return 0;

    CGLSetCurrentContext(g_cgl_ctx);
    return 1;
}

static void gl_context_destroy(void) {
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(g_cgl_ctx);
}

#elif defined(MOP_HAS_OPENGL)
/* Linux: EGL headless context */
#include <EGL/egl.h>

static EGLDisplay g_egl_dpy;
static EGLContext g_egl_ctx;

static int gl_context_create(void) {
    g_egl_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!g_egl_dpy) return 0;
    if (!eglInitialize(g_egl_dpy, NULL, NULL)) return 0;

    eglBindAPI(EGL_OPENGL_API);

    EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE,   8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_NONE
    };
    EGLConfig cfg;
    EGLint ncfg;
    if (!eglChooseConfig(g_egl_dpy, cfg_attrs, &cfg, 1, &ncfg) || ncfg < 1)
        return 0;

    EGLint ctx_attrs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE
    };
    g_egl_ctx = eglCreateContext(g_egl_dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (!g_egl_ctx) return 0;

    eglMakeCurrent(g_egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g_egl_ctx);
    return 1;
}

static void gl_context_destroy(void) {
    eglMakeCurrent(g_egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(g_egl_dpy, g_egl_ctx);
    eglTerminate(g_egl_dpy);
}

#endif /* platform GL context */

/* -------------------------------------------------------------------------
 * Cube geometry (same as headless.c)
 * ------------------------------------------------------------------------- */

static const MopVertex CUBE_VERTICES[] = {
    /* Front (Z+) — red */
    { .position = { -0.5f, -0.5f,  0.5f }, .normal = {  0, 0,  1 }, .color = { 0.9f, 0.2f, 0.2f, 1 } },
    { .position = {  0.5f, -0.5f,  0.5f }, .normal = {  0, 0,  1 }, .color = { 0.9f, 0.2f, 0.2f, 1 } },
    { .position = {  0.5f,  0.5f,  0.5f }, .normal = {  0, 0,  1 }, .color = { 0.9f, 0.2f, 0.2f, 1 } },
    { .position = { -0.5f,  0.5f,  0.5f }, .normal = {  0, 0,  1 }, .color = { 0.9f, 0.2f, 0.2f, 1 } },
    /* Back (Z-) — green */
    { .position = {  0.5f, -0.5f, -0.5f }, .normal = {  0, 0, -1 }, .color = { 0.2f, 0.9f, 0.2f, 1 } },
    { .position = { -0.5f, -0.5f, -0.5f }, .normal = {  0, 0, -1 }, .color = { 0.2f, 0.9f, 0.2f, 1 } },
    { .position = { -0.5f,  0.5f, -0.5f }, .normal = {  0, 0, -1 }, .color = { 0.2f, 0.9f, 0.2f, 1 } },
    { .position = {  0.5f,  0.5f, -0.5f }, .normal = {  0, 0, -1 }, .color = { 0.2f, 0.9f, 0.2f, 1 } },
    /* Top (Y+) — blue */
    { .position = { -0.5f,  0.5f,  0.5f }, .normal = {  0,  1, 0 }, .color = { 0.2f, 0.2f, 0.9f, 1 } },
    { .position = {  0.5f,  0.5f,  0.5f }, .normal = {  0,  1, 0 }, .color = { 0.2f, 0.2f, 0.9f, 1 } },
    { .position = {  0.5f,  0.5f, -0.5f }, .normal = {  0,  1, 0 }, .color = { 0.2f, 0.2f, 0.9f, 1 } },
    { .position = { -0.5f,  0.5f, -0.5f }, .normal = {  0,  1, 0 }, .color = { 0.2f, 0.2f, 0.9f, 1 } },
    /* Bottom (Y-) — yellow */
    { .position = { -0.5f, -0.5f, -0.5f }, .normal = {  0, -1, 0 }, .color = { 0.9f, 0.9f, 0.2f, 1 } },
    { .position = {  0.5f, -0.5f, -0.5f }, .normal = {  0, -1, 0 }, .color = { 0.9f, 0.9f, 0.2f, 1 } },
    { .position = {  0.5f, -0.5f,  0.5f }, .normal = {  0, -1, 0 }, .color = { 0.9f, 0.9f, 0.2f, 1 } },
    { .position = { -0.5f, -0.5f,  0.5f }, .normal = {  0, -1, 0 }, .color = { 0.9f, 0.9f, 0.2f, 1 } },
    /* Right (X+) — cyan */
    { .position = {  0.5f, -0.5f,  0.5f }, .normal = {  1, 0, 0 }, .color = { 0.2f, 0.9f, 0.9f, 1 } },
    { .position = {  0.5f, -0.5f, -0.5f }, .normal = {  1, 0, 0 }, .color = { 0.2f, 0.9f, 0.9f, 1 } },
    { .position = {  0.5f,  0.5f, -0.5f }, .normal = {  1, 0, 0 }, .color = { 0.2f, 0.9f, 0.9f, 1 } },
    { .position = {  0.5f,  0.5f,  0.5f }, .normal = {  1, 0, 0 }, .color = { 0.2f, 0.9f, 0.9f, 1 } },
    /* Left (X-) — magenta */
    { .position = { -0.5f, -0.5f, -0.5f }, .normal = { -1, 0, 0 }, .color = { 0.9f, 0.2f, 0.9f, 1 } },
    { .position = { -0.5f, -0.5f,  0.5f }, .normal = { -1, 0, 0 }, .color = { 0.9f, 0.2f, 0.9f, 1 } },
    { .position = { -0.5f,  0.5f,  0.5f }, .normal = { -1, 0, 0 }, .color = { 0.9f, 0.2f, 0.9f, 1 } },
    { .position = { -0.5f,  0.5f, -0.5f }, .normal = { -1, 0, 0 }, .color = { 0.9f, 0.2f, 0.9f, 1 } },
};

static const uint32_t CUBE_INDICES[] = {
     0,  1,  2,   2,  3,  0,
     4,  5,  6,   6,  7,  4,
     8,  9, 10,  10, 11,  8,
    12, 13, 14,  14, 15, 12,
    16, 17, 18,  18, 19, 16,
    20, 21, 22,  22, 23, 20,
};

#define CUBE_VERTEX_COUNT (sizeof(CUBE_VERTICES) / sizeof(CUBE_VERTICES[0]))
#define CUBE_INDEX_COUNT  (sizeof(CUBE_INDICES)  / sizeof(CUBE_INDICES[0]))

/* -------------------------------------------------------------------------
 * Render scene on a given backend, return pixel buffer (caller frees)
 * ------------------------------------------------------------------------- */

static uint8_t *render_scene(MopBackendType backend, int w, int h,
                             int *out_w, int *out_h) {
    MopViewportDesc desc = { .width = w, .height = h, .backend = backend };
    MopViewport *vp = mop_viewport_create(&desc);
    if (!vp) return NULL;

    mop_viewport_set_camera(vp,
        (MopVec3){ 2.0f, 2.0f, 3.0f },
        (MopVec3){ 0.0f, 0.0f, 0.0f },
        (MopVec3){ 0.0f, 1.0f, 0.0f },
        60.0f, 0.1f, 50.0f);

    mop_viewport_set_clear_color(vp,
        (MopColor){ 0.15f, 0.15f, 0.2f, 1.0f });

    MopMeshDesc mesh = {
        .vertices     = CUBE_VERTICES,
        .vertex_count = CUBE_VERTEX_COUNT,
        .indices      = CUBE_INDICES,
        .index_count  = CUBE_INDEX_COUNT,
        .object_id    = 1
    };
    MopMesh *cube = mop_viewport_add_mesh(vp, &mesh);
    if (!cube) { mop_viewport_destroy(vp); return NULL; }

    /* Render a rotated cube (same angle every time for determinism) */
    float angle = 45.0f * (3.14159265f / 180.0f);
    MopMat4 rot = mop_mat4_rotate_y(angle);
    mop_mesh_set_transform(cube, &rot);

    /* Add multi-light scene: warm key + cool fill + green point */
    MopLight key = {
        .type      = MOP_LIGHT_DIRECTIONAL,
        .direction = { 0.5f, 1.0f, 0.3f },
        .color     = { 1.0f, 0.9f, 0.7f, 1.0f },
        .intensity = 0.8f,
        .active    = true,
    };
    MopLight fill = {
        .type      = MOP_LIGHT_DIRECTIONAL,
        .direction = { -0.5f, 0.3f, -0.5f },
        .color     = { 0.4f, 0.5f, 0.9f, 1.0f },
        .intensity = 0.4f,
        .active    = true,
    };
    MopLight point = {
        .type      = MOP_LIGHT_POINT,
        .position  = { 1.5f, 0.5f, 1.5f },
        .color     = { 0.2f, 1.0f, 0.3f, 1.0f },
        .intensity = 0.6f,
        .range     = 5.0f,
        .active    = true,
    };
    mop_viewport_add_light(vp, &key);
    mop_viewport_add_light(vp, &fill);
    mop_viewport_add_light(vp, &point);
    mop_viewport_set_ambient(vp, 0.1f);

    mop_viewport_render(vp);

    int fw, fh;
    const uint8_t *pixels = mop_viewport_read_color(vp, &fw, &fh);
    if (!pixels) { mop_viewport_destroy(vp); return NULL; }

    /* Copy — the pointer is invalidated on destroy */
    size_t sz = (size_t)fw * (size_t)fh * 4;
    uint8_t *copy = malloc(sz);
    if (copy) memcpy(copy, pixels, sz);

    *out_w = fw;
    *out_h = fh;

    mop_viewport_remove_mesh(vp, cube);
    mop_viewport_destroy(vp);
    return copy;
}

/* -------------------------------------------------------------------------
 * Write PPM for visual inspection
 * ------------------------------------------------------------------------- */

static void write_ppm(const char *path, const uint8_t *px, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++)
        fwrite(&px[i * 4], 1, 3, f);
    fclose(f);
}

/* -------------------------------------------------------------------------
 * Compare two buffers and print stats
 * ------------------------------------------------------------------------- */

static void compare(const char *name_a, const uint8_t *a,
                    const char *name_b, const uint8_t *b,
                    int w, int h) {
    int total  = w * h;
    int exact  = 0;
    int close1 = 0;  /* diff <= 1 per channel */
    int close5 = 0;  /* diff <= 5 */
    int bad    = 0;   /* diff > 5 */
    int max_diff = 0;

    for (int i = 0; i < total; i++) {
        int off = i * 4;
        int dr = abs((int)a[off+0] - (int)b[off+0]);
        int dg = abs((int)a[off+1] - (int)b[off+1]);
        int db = abs((int)a[off+2] - (int)b[off+2]);
        int da = abs((int)a[off+3] - (int)b[off+3]);
        int mx = dr; if (dg > mx) mx = dg; if (db > mx) mx = db; if (da > mx) mx = da;

        if (mx == 0)     exact++;
        else if (mx <= 1) close1++;
        else if (mx <= 5) close5++;
        else              bad++;

        if (mx > max_diff) max_diff = mx;
    }

    printf("  %s vs %s:\n", name_a, name_b);
    printf("    exact:   %6d (%5.1f%%)\n", exact,  100.0 * exact  / total);
    printf("    ±1:      %6d (%5.1f%%)\n", close1, 100.0 * close1 / total);
    printf("    ±2..5:   %6d (%5.1f%%)\n", close5, 100.0 * close5 / total);
    printf("    >5:      %6d (%5.1f%%)\n", bad,    100.0 * bad    / total);
    printf("    max diff: %d\n\n", max_diff);
}

/* -------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */

int main(void) {
    int w = 800, h = 600;

    printf("=== Three-Way Backend Comparison (%dx%d) ===\n\n", w, h);

    /* --- CPU --- */
    int cw, ch;
    uint8_t *cpu_px = render_scene(MOP_BACKEND_CPU, w, h, &cw, &ch);
    if (!cpu_px) { fprintf(stderr, "CPU render failed\n"); return 1; }
    printf("[CPU]    rendered %dx%d\n", cw, ch);
    write_ppm("build/compare_cpu.ppm", cpu_px, cw, ch);

    /* --- OpenGL --- */
    uint8_t *gl_px = NULL;
    int gw = 0, gh = 0;
    (void)gw; (void)gh;
#if defined(MOP_HAS_OPENGL)
    if (gl_context_create()) {
        gl_px = render_scene(MOP_BACKEND_OPENGL, w, h, &gw, &gh);
        if (gl_px) {
            printf("[OpenGL] rendered %dx%d\n", gw, gh);
            write_ppm("build/compare_gl.ppm", gl_px, gw, gh);
        } else {
            printf("[OpenGL] render FAILED\n");
        }
        gl_context_destroy();
    } else {
        printf("[OpenGL] context creation FAILED — skipping\n");
    }
#else
    printf("[OpenGL] not compiled — skipping\n");
#endif

    /* --- Vulkan --- */
    uint8_t *vk_px = NULL;
    int vw __attribute__((unused)) = 0, vh __attribute__((unused)) = 0;
#if defined(MOP_HAS_VULKAN)
    vk_px = render_scene(MOP_BACKEND_VULKAN, w, h, &vw, &vh);
    if (vk_px) {
        printf("[Vulkan] rendered %dx%d\n", vw, vh);
        write_ppm("build/compare_vk.ppm", vk_px, vw, vh);
    } else {
        printf("[Vulkan] render FAILED\n");
    }
#else
    printf("[Vulkan] not compiled — skipping\n");
#endif

    /* --- Compare all pairs --- */
    printf("\n=== Pixel Comparison ===\n\n");

    if (cpu_px && gl_px)
        compare("CPU", cpu_px, "OpenGL", gl_px, w, h);

    if (cpu_px && vk_px)
        compare("CPU", cpu_px, "Vulkan", vk_px, w, h);

    if (gl_px && vk_px)
        compare("OpenGL", gl_px, "Vulkan", vk_px, w, h);

    /* --- Verdict --- */
    int backends = 1 + (gl_px ? 1 : 0) + (vk_px ? 1 : 0);
    printf("Compared %d backend%s.\n", backends, backends > 1 ? "s" : "");

    free(cpu_px);
    free(gl_px);
    free(vk_px);
    return 0;
}
