/*
 * Master of Puppets — OpenGL 3.3 Reference Backend
 * opengl_backend.c — Full RHI implementation via OpenGL 3.3 core profile
 *
 * This backend is compiled only when MOP_ENABLE_OPENGL=1 is set.
 * It assumes the caller has already created and made current an
 * OpenGL 3.3+ core profile context before calling device_create.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#if defined(MOP_HAS_OPENGL)

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#include "rhi/rhi.h"
#include "shaders.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Helper: compile a shader stage
 * ------------------------------------------------------------------------- */

static GLuint compile_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "[MOP/GL] shader compile error: %s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

/* -------------------------------------------------------------------------
 * Helper: link a shader program from vertex + fragment source
 * ------------------------------------------------------------------------- */

static GLuint create_program(const char *vs_src, const char *fs_src) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    if (!vs) return 0;
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!fs) { glDeleteShader(vs); return 0; }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "[MOP/GL] program link error: %s\n", log);
        glDeleteProgram(prog);
        prog = 0;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

/* -------------------------------------------------------------------------
 * Internal types
 * ------------------------------------------------------------------------- */

struct MopRhiDevice {
    GLuint solid_program;
    GLuint wireframe_program;

    /* Uniform locations for solid program */
    GLint u_mvp;
    GLint u_model;
    GLint u_light_dir;
    GLint u_ambient;
    GLint u_opacity;
    GLint u_blend_mode;
    GLint u_object_id;
    GLint u_has_texture;
    GLint u_texture;

    /* Multi-light uniform locations */
    GLint u_num_lights;
    GLint u_light_position[4];
    GLint u_light_direction[4];
    GLint u_light_color[4];
    GLint u_light_params[4];

    /* Uniform locations for wireframe program */
    GLint uw_mvp;
    GLint uw_model;
    GLint uw_object_id;

    /* Empty VAO for binding */
    GLuint empty_vao;
};

struct MopRhiBuffer {
    GLuint  vbo;
    size_t  size;
    void   *shadow;  /* CPU-side shadow copy for readback */
};

struct MopRhiFramebuffer {
    GLuint fbo;
    GLuint color_tex;
    GLuint depth_rbo;
    GLuint pick_tex;     /* GL_R32UI for object_id picking */
    int    width;
    int    height;
    uint8_t  *readback_color;   /* CPU readback buffer */
    uint32_t *readback_pick;
    float    *readback_depth;
};

struct MopRhiTexture {
    GLuint  tex_id;
    int     width;
    int     height;
};

/* -------------------------------------------------------------------------
 * Device lifecycle
 * ------------------------------------------------------------------------- */

static MopRhiDevice *gl_device_create(void) {
    MopRhiDevice *dev = calloc(1, sizeof(MopRhiDevice));
    if (!dev) return NULL;

    /* Compile shaders */
    dev->solid_program = create_program(MOP_GL_VERTEX_SHADER,
                                         MOP_GL_FRAGMENT_SHADER);
    dev->wireframe_program = create_program(MOP_GL_VERTEX_SHADER,
                                             MOP_GL_WIREFRAME_FRAGMENT_SHADER);

    if (!dev->solid_program || !dev->wireframe_program) {
        if (dev->solid_program) glDeleteProgram(dev->solid_program);
        if (dev->wireframe_program) glDeleteProgram(dev->wireframe_program);
        free(dev);
        return NULL;
    }

    /* Cache uniform locations: solid */
    dev->u_mvp         = glGetUniformLocation(dev->solid_program, "u_mvp");
    dev->u_model       = glGetUniformLocation(dev->solid_program, "u_model");
    dev->u_light_dir   = glGetUniformLocation(dev->solid_program, "u_light_dir");
    dev->u_ambient     = glGetUniformLocation(dev->solid_program, "u_ambient");
    dev->u_opacity     = glGetUniformLocation(dev->solid_program, "u_opacity");
    dev->u_blend_mode  = glGetUniformLocation(dev->solid_program, "u_blend_mode");
    dev->u_object_id   = glGetUniformLocation(dev->solid_program, "u_object_id");
    dev->u_has_texture = glGetUniformLocation(dev->solid_program, "u_has_texture");
    dev->u_texture     = glGetUniformLocation(dev->solid_program, "u_texture");

    /* Cache uniform locations: multi-light */
    dev->u_num_lights = glGetUniformLocation(dev->solid_program, "u_num_lights");
    for (int i = 0; i < 4; i++) {
        char name[64];
        snprintf(name, sizeof(name), "u_light_position[%d]", i);
        dev->u_light_position[i] = glGetUniformLocation(dev->solid_program, name);
        snprintf(name, sizeof(name), "u_light_direction[%d]", i);
        dev->u_light_direction[i] = glGetUniformLocation(dev->solid_program, name);
        snprintf(name, sizeof(name), "u_light_color[%d]", i);
        dev->u_light_color[i] = glGetUniformLocation(dev->solid_program, name);
        snprintf(name, sizeof(name), "u_light_params[%d]", i);
        dev->u_light_params[i] = glGetUniformLocation(dev->solid_program, name);
    }

    /* Cache uniform locations: wireframe */
    dev->uw_mvp       = glGetUniformLocation(dev->wireframe_program, "u_mvp");
    dev->uw_model     = glGetUniformLocation(dev->wireframe_program, "u_model");
    dev->uw_object_id = glGetUniformLocation(dev->wireframe_program, "u_object_id");

    /* Create empty VAO (required by core profile) */
    glGenVertexArrays(1, &dev->empty_vao);

    return dev;
}

static void gl_device_destroy(MopRhiDevice *device) {
    if (!device) return;
    if (device->solid_program) glDeleteProgram(device->solid_program);
    if (device->wireframe_program) glDeleteProgram(device->wireframe_program);
    if (device->empty_vao) glDeleteVertexArrays(1, &device->empty_vao);
    free(device);
}

/* -------------------------------------------------------------------------
 * Buffer management
 * ------------------------------------------------------------------------- */

static MopRhiBuffer *gl_buffer_create(MopRhiDevice *device,
                                       const MopRhiBufferDesc *desc) {
    (void)device;

    MopRhiBuffer *buf = malloc(sizeof(MopRhiBuffer));
    if (!buf) return NULL;

    buf->size = desc->size;

    /* Shadow copy for CPU readback */
    buf->shadow = malloc(desc->size);
    if (!buf->shadow) {
        free(buf);
        return NULL;
    }
    memcpy(buf->shadow, desc->data, desc->size);

    glGenBuffers(1, &buf->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, buf->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)desc->size, desc->data,
                 GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    return buf;
}

static void gl_buffer_destroy(MopRhiDevice *device, MopRhiBuffer *buffer) {
    (void)device;
    if (!buffer) return;
    glDeleteBuffers(1, &buffer->vbo);
    free(buffer->shadow);
    free(buffer);
}

/* -------------------------------------------------------------------------
 * Framebuffer management
 * ------------------------------------------------------------------------- */

static void gl_fb_create_attachments(struct MopRhiFramebuffer *fb,
                                      int width, int height) {
    fb->width  = width;
    fb->height = height;

    /* Color attachment (SRGB8_ALPHA8 — linear→sRGB on write) */
    glGenTextures(1, &fb->color_tex);
    glBindTexture(GL_TEXTURE_2D, fb->color_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    /* Object ID attachment (R32UI) */
    glGenTextures(1, &fb->pick_tex);
    glBindTexture(GL_TEXTURE_2D, fb->pick_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, width, height, 0,
                 GL_RED_INTEGER, GL_UNSIGNED_INT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    /* Depth attachment (renderbuffer) */
    glGenRenderbuffers(1, &fb->depth_rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, fb->depth_rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);

    /* FBO */
    glGenFramebuffers(1, &fb->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fb->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, fb->color_tex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,
                           GL_TEXTURE_2D, fb->pick_tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, fb->depth_rbo);

    GLenum bufs[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
    glDrawBuffers(2, bufs);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    /* CPU readback buffers */
    size_t npixels = (size_t)width * (size_t)height;
    fb->readback_color = malloc(npixels * 4);
    fb->readback_pick  = malloc(npixels * sizeof(uint32_t));
    fb->readback_depth = malloc(npixels * sizeof(float));
}

static void gl_fb_destroy_attachments(struct MopRhiFramebuffer *fb) {
    if (fb->fbo) glDeleteFramebuffers(1, &fb->fbo);
    if (fb->color_tex) glDeleteTextures(1, &fb->color_tex);
    if (fb->pick_tex) glDeleteTextures(1, &fb->pick_tex);
    if (fb->depth_rbo) glDeleteRenderbuffers(1, &fb->depth_rbo);
    free(fb->readback_color);
    free(fb->readback_pick);
    free(fb->readback_depth);
    fb->fbo = 0;
    fb->color_tex = 0;
    fb->pick_tex = 0;
    fb->depth_rbo = 0;
    fb->readback_color = NULL;
    fb->readback_pick = NULL;
    fb->readback_depth = NULL;
}

static MopRhiFramebuffer *gl_framebuffer_create(MopRhiDevice *device,
                                                  const MopRhiFramebufferDesc *desc) {
    (void)device;
    MopRhiFramebuffer *fb = calloc(1, sizeof(MopRhiFramebuffer));
    if (!fb) return NULL;

    gl_fb_create_attachments(fb, desc->width, desc->height);
    return fb;
}

static void gl_framebuffer_destroy(MopRhiDevice *device,
                                    MopRhiFramebuffer *fb) {
    (void)device;
    if (!fb) return;
    gl_fb_destroy_attachments(fb);
    free(fb);
}

static void gl_framebuffer_resize(MopRhiDevice *device,
                                   MopRhiFramebuffer *fb,
                                   int width, int height) {
    (void)device;
    if (!fb) return;
    gl_fb_destroy_attachments(fb);
    gl_fb_create_attachments(fb, width, height);
}

/* -------------------------------------------------------------------------
 * Frame commands
 * ------------------------------------------------------------------------- */

static void gl_frame_begin(MopRhiDevice *device, MopRhiFramebuffer *fb,
                            MopColor clear_color) {
    (void)device;
    glBindFramebuffer(GL_FRAMEBUFFER, fb->fbo);
    glViewport(0, 0, fb->width, fb->height);

    glClearColor(clear_color.r, clear_color.g, clear_color.b, clear_color.a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    /* Clear picking buffer to 0 */
    GLuint zero = 0;
    glClearBufferuiv(GL_COLOR, 1, &zero);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glEnable(GL_FRAMEBUFFER_SRGB);
}

static void gl_frame_end(MopRhiDevice *device, MopRhiFramebuffer *fb) {
    (void)device;
    (void)fb;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

/* -------------------------------------------------------------------------
 * Draw call
 * ------------------------------------------------------------------------- */

static void gl_draw(MopRhiDevice *device, MopRhiFramebuffer *fb,
                     const MopRhiDrawCall *call) {
    (void)fb;

    GLuint prog;
    GLuint vao;

    if (call->wireframe) {
        prog = device->wireframe_program;
        glUseProgram(prog);
        glUniformMatrix4fv(device->uw_mvp, 1, GL_FALSE, call->mvp.d);
        glUniformMatrix4fv(device->uw_model, 1, GL_FALSE, call->model.d);
        glUniform1ui(device->uw_object_id, call->object_id);
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    } else {
        prog = device->solid_program;
        glUseProgram(prog);
        glUniformMatrix4fv(device->u_mvp, 1, GL_FALSE, call->mvp.d);
        glUniformMatrix4fv(device->u_model, 1, GL_FALSE, call->model.d);
        glUniform3f(device->u_light_dir, call->light_dir.x,
                    call->light_dir.y, call->light_dir.z);
        glUniform1f(device->u_ambient, call->ambient);
        glUniform1f(device->u_opacity, call->opacity);
        glUniform1i(device->u_blend_mode, (GLint)call->blend_mode);
        glUniform1ui(device->u_object_id, call->object_id);

        if (call->texture) {
            glUniform1i(device->u_has_texture, 1);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, call->texture->tex_id);
            glUniform1i(device->u_texture, 0);
        } else {
            glUniform1i(device->u_has_texture, 0);
        }

        /* Multi-light uniforms */
        uint32_t nl = call->light_count < 4 ? call->light_count : 4;
        glUniform1i(device->u_num_lights,
                    (call->lights && nl > 0) ? (GLint)nl : 0);
        if (call->lights && nl > 0) {
            for (uint32_t li = 0; li < nl; li++) {
                const MopLight *light = &call->lights[li];
                glUniform4f(device->u_light_position[li],
                    light->position.x, light->position.y, light->position.z,
                    (float)light->type);
                glUniform4f(device->u_light_direction[li],
                    light->direction.x, light->direction.y, light->direction.z,
                    0.0f);
                glUniform4f(device->u_light_color[li],
                    light->color.r, light->color.g, light->color.b,
                    light->intensity);
                glUniform4f(device->u_light_params[li],
                    light->range, light->spot_inner_cos, light->spot_outer_cos,
                    light->active ? 1.0f : 0.0f);
            }
        }

        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }

    /* Blending */
    if (call->blend_mode != MOP_BLEND_OPAQUE && call->opacity < 1.0f) {
        glEnable(GL_BLEND);
        switch (call->blend_mode) {
        case MOP_BLEND_ADDITIVE:
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            break;
        case MOP_BLEND_MULTIPLY:
            glBlendFunc(GL_DST_COLOR, GL_ZERO);
            break;
        default: /* ALPHA */
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            break;
        }
    } else {
        glDisable(GL_BLEND);
    }

    /* Depth test */
    if (call->depth_test) {
        glEnable(GL_DEPTH_TEST);
    } else {
        glDisable(GL_DEPTH_TEST);
    }

    /* Backface culling */
    if (call->backface_cull) {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
    } else {
        glDisable(GL_CULL_FACE);
    }

    /* Set up VAO with vertex buffer */
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    /* Bind vertex buffer and set up attributes matching MopVertex layout:
     * struct MopVertex {
     *     MopVec3 position;  // 3 floats at offset 0
     *     MopVec3 normal;    // 3 floats at offset 12
     *     MopColor color;    // 4 floats at offset 24
     *     float u, v;        // 2 floats at offset 40
     * };  // stride = 48
     */
    glBindBuffer(GL_ARRAY_BUFFER, call->vertex_buffer->vbo);
    GLsizei stride = (GLsizei)sizeof(MopVertex);

    /* position */
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                          (const void *)0);
    /* normal */
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                          (const void *)(3 * sizeof(float)));
    /* color (MopColor is 4 floats: r,g,b,a) */
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride,
                          (const void *)(6 * sizeof(float)));
    /* texcoord */
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride,
                          (const void *)(10 * sizeof(float)));

    /* Bind index buffer and draw */
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, call->index_buffer->vbo);
    glDrawElements(GL_TRIANGLES, (GLsizei)call->index_count,
                   GL_UNSIGNED_INT, NULL);

    /* Cleanup */
    glBindVertexArray(0);
    glDeleteVertexArrays(1, &vao);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glDisable(GL_BLEND);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
}

/* -------------------------------------------------------------------------
 * Instanced draw call
 * ------------------------------------------------------------------------- */

static void gl_draw_instanced(MopRhiDevice *device, MopRhiFramebuffer *fb,
                                const MopRhiDrawCall *call,
                                const MopMat4 *instance_transforms,
                                uint32_t instance_count) {
    if (!call || !instance_transforms || instance_count == 0) return;

    for (uint32_t inst = 0; inst < instance_count; inst++) {
        MopRhiDrawCall inst_call = *call;
        inst_call.model = instance_transforms[inst];

        MopMat4 view_model = mop_mat4_multiply(call->view,
                                                instance_transforms[inst]);
        inst_call.mvp = mop_mat4_multiply(call->projection, view_model);

        gl_draw(device, fb, &inst_call);
    }
}

/* -------------------------------------------------------------------------
 * Dynamic buffer update
 * ------------------------------------------------------------------------- */

static void gl_buffer_update(MopRhiDevice *device, MopRhiBuffer *buffer,
                               const void *data, size_t offset, size_t size) {
    (void)device;
    glBindBuffer(GL_ARRAY_BUFFER, buffer->vbo);
    glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)offset, (GLsizeiptr)size, data);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    /* Update shadow copy */
    memcpy((uint8_t *)buffer->shadow + offset, data, size);
}

/* -------------------------------------------------------------------------
 * Picking readback
 * ------------------------------------------------------------------------- */

static uint32_t gl_pick_read_id(MopRhiDevice *device, MopRhiFramebuffer *fb,
                                 int x, int y) {
    (void)device;
    if (x < 0 || x >= fb->width || y < 0 || y >= fb->height) return 0;

    /* OpenGL has bottom-left origin; MOP uses top-left */
    int gl_y = fb->height - 1 - y;

    uint32_t id = 0;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb->fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT1);
    glReadPixels(x, gl_y, 1, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, &id);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    return id;
}

static float gl_pick_read_depth(MopRhiDevice *device, MopRhiFramebuffer *fb,
                                 int x, int y) {
    (void)device;
    if (x < 0 || x >= fb->width || y < 0 || y >= fb->height) return 1.0f;

    int gl_y = fb->height - 1 - y;
    float depth = 1.0f;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb->fbo);
    glReadPixels(x, gl_y, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    return depth;
}

/* -------------------------------------------------------------------------
 * Color buffer readback
 * ------------------------------------------------------------------------- */

static const uint8_t *gl_framebuffer_read_color(MopRhiDevice *device,
                                                  MopRhiFramebuffer *fb,
                                                  int *out_width,
                                                  int *out_height) {
    (void)device;
    if (out_width)  *out_width  = fb->width;
    if (out_height) *out_height = fb->height;

    if (!fb->readback_color) return NULL;

    /* Read from FBO */
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb->fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);

    /* Read row by row, flipping vertically (GL is bottom-up, MOP is top-down) */
    int w = fb->width;
    int h = fb->height;
    uint8_t *row = malloc((size_t)w * 4);
    if (row) {
        for (int y = 0; y < h; y++) {
            int gl_y = h - 1 - y;
            glReadPixels(0, gl_y, w, 1, GL_RGBA, GL_UNSIGNED_BYTE, row);
            memcpy(fb->readback_color + (size_t)y * (size_t)w * 4,
                   row, (size_t)w * 4);
        }
        free(row);
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    return fb->readback_color;
}

/* -------------------------------------------------------------------------
 * Texture management
 * ------------------------------------------------------------------------- */

static MopRhiTexture *gl_texture_create(MopRhiDevice *device, int width,
                                          int height,
                                          const uint8_t *rgba_data) {
    (void)device;
    MopRhiTexture *tex = malloc(sizeof(MopRhiTexture));
    if (!tex) return NULL;

    tex->width  = width;
    tex->height = height;

    glGenTextures(1, &tex->tex_id);
    glBindTexture(GL_TEXTURE_2D, tex->tex_id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba_data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);

    return tex;
}

static void gl_texture_destroy(MopRhiDevice *device, MopRhiTexture *texture) {
    (void)device;
    if (!texture) return;
    glDeleteTextures(1, &texture->tex_id);
    free(texture);
}

/* -------------------------------------------------------------------------
 * Buffer read (overlay safety — returns CPU shadow copy)
 * ------------------------------------------------------------------------- */

static const void *gl_buffer_read(MopRhiBuffer *buffer) {
    return buffer ? buffer->shadow : NULL;
}

/* -------------------------------------------------------------------------
 * Backend function table
 * ------------------------------------------------------------------------- */

static const MopRhiBackend GL_BACKEND = {
    .name                 = "opengl",
    .device_create        = gl_device_create,
    .device_destroy       = gl_device_destroy,
    .buffer_create        = gl_buffer_create,
    .buffer_destroy       = gl_buffer_destroy,
    .framebuffer_create   = gl_framebuffer_create,
    .framebuffer_destroy  = gl_framebuffer_destroy,
    .framebuffer_resize   = gl_framebuffer_resize,
    .frame_begin          = gl_frame_begin,
    .frame_end            = gl_frame_end,
    .draw                 = gl_draw,
    .pick_read_id         = gl_pick_read_id,
    .pick_read_depth      = gl_pick_read_depth,
    .framebuffer_read_color = gl_framebuffer_read_color,
    .texture_create         = gl_texture_create,
    .texture_destroy        = gl_texture_destroy,
    .draw_instanced         = gl_draw_instanced,
    .buffer_update          = gl_buffer_update,
    .buffer_read            = gl_buffer_read,
};

const MopRhiBackend *mop_rhi_backend_opengl(void) {
    return &GL_BACKEND;
}

#endif /* MOP_HAS_OPENGL */
