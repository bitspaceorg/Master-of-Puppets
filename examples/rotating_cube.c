/*
 * Master of Puppets — Interactive Viewport Example (SDL3)
 *
 * A real-time interactive viewport rendered by the CPU backend and
 * displayed in an SDL3 window.  Supports object selection and
 * Translate / Rotate / Scale gizmos.
 *
 * Controls:
 *   Left-drag     Orbit camera (preserves selection)
 *   Left-click    Select / deselect objects
 *   Right-drag    Pan camera
 *   Scroll        Zoom in / out
 *   T             Translate gizmo mode
 *   G             Rotate gizmo mode
 *   E             Scale gizmo mode
 *   W             Toggle wireframe / solid
 *   Space         Toggle auto-rotation
 *   R             Reset camera + deselect
 *   Arrow keys    Move camera on XZ plane
 *   S             Spawn cube at random position
 *   Escape        Deselect (or quit if nothing selected)
 *   Q             Quit
 *
 * Build:
 *   cd examples && nix develop    # provides SDL3 + pkg-config
 *   cd .. && make interactive     # builds with pkg-config --cflags/--libs sdl3
 *   make run                      # launches the viewport
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <mop/mop.h>
#include <SDL3/SDL.h>

#include <stdio.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------------- */

#define WINDOW_W          960
#define WINDOW_H          720
#define PI                3.14159265358979323846f
#define MAX_SCENE_OBJECTS 128
#define CLICK_THRESHOLD   5.0f

/* -------------------------------------------------------------------------
 * Types
 * ------------------------------------------------------------------------- */

typedef struct {
    bool     active;
    uint32_t object_id;
    MopMesh *mesh;
    MopVec3  position, rotation, scale_val;
    bool     auto_rotates;
    bool     gizmo_enabled;
} SceneObject;

/* -------------------------------------------------------------------------
 * Orbit camera
 * ------------------------------------------------------------------------- */

typedef struct {
    MopVec3 target;
    float   distance;
    float   yaw;
    float   pitch;
} OrbitCamera;

static MopVec3 orbit_eye(const OrbitCamera *cam) {
    float cp = cosf(cam->pitch);
    return (MopVec3){
        cam->target.x + cam->distance * cp * sinf(cam->yaw),
        cam->target.y + cam->distance * sinf(cam->pitch),
        cam->target.z + cam->distance * cp * cosf(cam->yaw)
    };
}

static void orbit_apply(const OrbitCamera *cam, MopViewport *vp) {
    mop_viewport_set_camera(vp, orbit_eye(cam), cam->target,
                            (MopVec3){ 0, 1, 0 },
                            60.0f, 0.1f, 100.0f);
}

/* -------------------------------------------------------------------------
 * Cube geometry — X-macro generated
 * ------------------------------------------------------------------------- */

#define CUBE_FACES(F)                                                        \
    /*    nx  ny  nz    r     g     b       v0            v1            v2            v3         */\
    F( 0,  0,  1,  0.9f, 0.2f, 0.2f,  -1,-1, 1,   1,-1, 1,   1, 1, 1,  -1, 1, 1)  /* front  */\
    F( 0,  0, -1,  0.2f, 0.9f, 0.2f,   1,-1,-1,  -1,-1,-1,  -1, 1,-1,   1, 1,-1)  /* back   */\
    F( 0,  1,  0,  0.2f, 0.2f, 0.9f,  -1, 1, 1,   1, 1, 1,   1, 1,-1,  -1, 1,-1)  /* top    */\
    F( 0, -1,  0,  0.9f, 0.9f, 0.2f,  -1,-1,-1,   1,-1,-1,   1,-1, 1,  -1,-1, 1)  /* bottom */\
    F( 1,  0,  0,  0.2f, 0.9f, 0.9f,   1,-1, 1,   1,-1,-1,   1, 1,-1,   1, 1, 1)  /* right  */\
    F(-1,  0,  0,  0.9f, 0.2f, 0.9f,  -1,-1,-1,  -1,-1, 1,  -1, 1, 1,  -1, 1,-1)  /* left   */

#define FACE_VERTS(nx,ny,nz, cr,cg,cb, x0,y0,z0, x1,y1,z1, x2,y2,z2, x3,y3,z3) \
    {{ .5f*(x0), .5f*(y0), .5f*(z0) }, { nx,ny,nz }, { cr,cg,cb,1 }},            \
    {{ .5f*(x1), .5f*(y1), .5f*(z1) }, { nx,ny,nz }, { cr,cg,cb,1 }},            \
    {{ .5f*(x2), .5f*(y2), .5f*(z2) }, { nx,ny,nz }, { cr,cg,cb,1 }},            \
    {{ .5f*(x3), .5f*(y3), .5f*(z3) }, { nx,ny,nz }, { cr,cg,cb,1 }},

static const MopVertex CUBE_VERTS[] = { CUBE_FACES(FACE_VERTS) };

static const uint32_t CUBE_INDICES[] = {
     0,  1,  2,   2,  3,  0,
     4,  5,  6,   6,  7,  4,
     8,  9, 10,  10, 11,  8,
    12, 13, 14,  14, 15, 12,
    16, 17, 18,  18, 19, 16,
    20, 21, 22,  22, 23, 20,
};

#define CUBE_VERT_COUNT  (sizeof(CUBE_VERTS)   / sizeof(CUBE_VERTS[0]))
#define CUBE_INDEX_COUNT (sizeof(CUBE_INDICES) / sizeof(CUBE_INDICES[0]))

/* -------------------------------------------------------------------------
 * Ground plane
 * ------------------------------------------------------------------------- */

static const MopVertex GROUND_VERTS[] = {
    {{ -3, 0, -3 }, { 0,1,0 }, { .35f,.35f,.38f,1 }},
    {{  3, 0, -3 }, { 0,1,0 }, { .35f,.35f,.38f,1 }},
    {{  3, 0,  3 }, { 0,1,0 }, { .35f,.35f,.38f,1 }},
    {{ -3, 0,  3 }, { 0,1,0 }, { .35f,.35f,.38f,1 }},
};
static const uint32_t GROUND_IDX[] = { 0,1,2, 2,3,0 };

/* -------------------------------------------------------------------------
 * Transform helpers
 * ------------------------------------------------------------------------- */

static MopMat4 compose_trs(MopVec3 pos, MopVec3 rot, MopVec3 scl) {
    MopMat4 s  = mop_mat4_scale(scl);
    MopMat4 rx = mop_mat4_rotate_x(rot.x);
    MopMat4 ry = mop_mat4_rotate_y(rot.y);
    MopMat4 rz = mop_mat4_rotate_z(rot.z);
    MopMat4 t  = mop_mat4_translate(pos);
    return mop_mat4_multiply(t,
           mop_mat4_multiply(rz,
           mop_mat4_multiply(ry,
           mop_mat4_multiply(rx, s))));
}

/* -------------------------------------------------------------------------
 * Scene management
 * ------------------------------------------------------------------------- */

static SceneObject *scene_add(SceneObject scene[], MopViewport *vp,
                              const MopMeshDesc *desc, MopVec3 pos,
                              bool auto_rot) {
    uint32_t id = desc->object_id;
    if (id == 0 || id > MAX_SCENE_OBJECTS) return NULL;
    SceneObject *obj = &scene[id - 1];
    obj->active       = true;
    obj->object_id    = id;
    obj->mesh         = mop_viewport_add_mesh(vp, desc);
    obj->position     = pos;
    obj->rotation     = (MopVec3){0,0,0};
    obj->scale_val    = (MopVec3){1,1,1};
    obj->auto_rotates = auto_rot;
    obj->gizmo_enabled = true;
    MopMat4 xf = compose_trs(pos, obj->rotation, obj->scale_val);
    mop_mesh_set_transform(obj->mesh, &xf);
    return obj;
}

/* -------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    /* ---- SDL3 init ---- */
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow(
        "Master of Puppets", WINDOW_W, WINDOW_H,
        SDL_WINDOW_RESIZABLE
    );
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);

    /* ---- Viewport ---- */
    MopViewport *vp = mop_viewport_create(&(MopViewportDesc){
        .width   = WINDOW_W,
        .height  = WINDOW_H,
        .backend = MOP_BACKEND_CPU
    });
    if (!vp) {
        fprintf(stderr, "Failed to create MOP viewport\n");
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    mop_viewport_set_clear_color(vp, (MopColor){ .12f,.12f,.16f,1 });

    /* ---- Camera ---- */
    OrbitCamera cam = {
        .target = { 0, .4f, 0 }, .distance = 4.5f,
        .yaw = .6f, .pitch = .4f
    };
    orbit_apply(&cam, vp);

    /* ---- Scene registry ---- */
    SceneObject scene[MAX_SCENE_OBJECTS];
    memset(scene, 0, sizeof(scene));
    uint32_t next_id = 1;

    uint32_t cube_id = next_id++;
    scene_add(scene, vp, &(MopMeshDesc){
        .vertices = CUBE_VERTS, .vertex_count = CUBE_VERT_COUNT,
        .indices  = CUBE_INDICES, .index_count = CUBE_INDEX_COUNT,
        .object_id = cube_id
    }, (MopVec3){0, .5f, 0}, true);

    uint32_t ground_id = next_id++;
    (void)ground_id;
    scene_add(scene, vp, &(MopMeshDesc){
        .vertices = GROUND_VERTS, .vertex_count = 4,
        .indices  = GROUND_IDX, .index_count = 6,
        .object_id = ground_id
    }, (MopVec3){0, 0, 0}, false);

    /* ---- SDL texture for CPU framebuffer blit ---- */
    int win_w = WINDOW_W, win_h = WINDOW_H;
    SDL_Texture *tex = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ABGR8888,
        SDL_TEXTUREACCESS_STREAMING, win_w, win_h
    );

    /* ---- Selection & gizmo state ---- */
    uint32_t     selected_id    = 0;
    MopGizmo    *gizmo          = mop_gizmo_create(vp);
    bool         dragging_gizmo = false;
    MopGizmoAxis drag_axis      = MOP_GIZMO_AXIS_NONE;

    /* ---- Mouse / interaction state ---- */
    bool  running       = true;
    bool  left_btn_down = false;    /* left button currently held */
    bool  click_resolved = false;   /* drag threshold exceeded */
    float click_start_x = 0, click_start_y = 0;
    bool  dragging_l    = false;    /* orbiting */
    bool  dragging_r    = false;
    bool  wireframe     = false;
    srand((unsigned)SDL_GetPerformanceCounter());

    Uint64 last = SDL_GetPerformanceCounter();
    Uint64 freq = SDL_GetPerformanceFrequency();

    printf("Master of Puppets — Interactive Viewport (SDL3 + CPU rasterizer)\n");
    printf("  Left-drag: orbit  |  Right-drag: pan  |  Scroll: zoom\n");
    printf("  Click: select  |  T: translate  |  G: rotate  |  E: scale\n");
    printf("  W: wireframe  |  Space: pause  |  R: reset  |  Esc: deselect/quit\n");
    printf("  Arrow keys: move camera  |  S: spawn cube\n");

    /* ---- Event loop ---- */
    while (running) {
        Uint64 now = SDL_GetPerformanceCounter();
        float dt = (float)(now - last) / (float)freq;
        last = now;

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {

            case SDL_EVENT_QUIT:
                running = false;
                break;

            case SDL_EVENT_KEY_DOWN:
                switch (ev.key.key) {
                case SDLK_Q:
                    running = false; break;
                case SDLK_ESCAPE:
                    if (selected_id) {
                        mop_gizmo_hide(gizmo);
                        selected_id = 0;
                        dragging_gizmo = false;
                        printf("Deselected\n");
                    } else {
                        running = false;
                    }
                    break;
                case SDLK_W:
                    wireframe = !wireframe;
                    mop_viewport_set_render_mode(vp,
                        wireframe ? MOP_RENDER_WIREFRAME : MOP_RENDER_SOLID);
                    break;
                case SDLK_SPACE:
                    scene[cube_id-1].auto_rotates = !scene[cube_id-1].auto_rotates;
                    break;
                case SDLK_R:
                    cam = (OrbitCamera){
                        .target = { 0,.4f,0 }, .distance = 4.5f,
                        .yaw = .6f, .pitch = .4f
                    };
                    mop_gizmo_hide(gizmo);
                    selected_id = 0;
                    dragging_gizmo = false;
                    scene[cube_id-1].rotation = (MopVec3){0,0,0};
                    scene[cube_id-1].auto_rotates = true;
                    break;
                case SDLK_T:
                    if (selected_id
                        && mop_gizmo_get_mode(gizmo) != MOP_GIZMO_TRANSLATE
                        && scene[selected_id-1].gizmo_enabled) {
                        mop_gizmo_set_mode(gizmo, MOP_GIZMO_TRANSLATE);
                        printf("Gizmo: Translate\n");
                    }
                    break;
                case SDLK_G:
                    if (selected_id
                        && mop_gizmo_get_mode(gizmo) != MOP_GIZMO_ROTATE
                        && scene[selected_id-1].gizmo_enabled) {
                        mop_gizmo_set_mode(gizmo, MOP_GIZMO_ROTATE);
                        printf("Gizmo: Rotate\n");
                    }
                    break;
                case SDLK_E:
                    if (selected_id
                        && mop_gizmo_get_mode(gizmo) != MOP_GIZMO_SCALE
                        && scene[selected_id-1].gizmo_enabled) {
                        mop_gizmo_set_mode(gizmo, MOP_GIZMO_SCALE);
                        printf("Gizmo: Scale\n");
                    }
                    break;
                case SDLK_S:
                    if (next_id <= MAX_SCENE_OBJECTS) {
                        float rx = ((float)rand()/(float)RAND_MAX)*6.0f - 3.0f;
                        float rz = ((float)rand()/(float)RAND_MAX)*6.0f - 3.0f;
                        uint32_t sid = next_id++;
                        scene_add(scene, vp, &(MopMeshDesc){
                            .vertices = CUBE_VERTS, .vertex_count = CUBE_VERT_COUNT,
                            .indices  = CUBE_INDICES, .index_count = CUBE_INDEX_COUNT,
                            .object_id = sid
                        }, (MopVec3){rx, 0.5f, rz}, false);
                        printf("Spawned cube #%u at (%.1f, %.1f)\n", sid, rx, rz);
                    }
                    break;
                default: break;
                }
                break;

            /* ----------------------------------------------------------
             * Mouse interaction: click vs drag distinction
             *
             * Button-down: only start gizmo drags immediately.
             * Motion:      if mouse moves past threshold, start orbit.
             * Button-up:   if threshold was NOT exceeded, treat as click
             *              (select / deselect).
             * ---------------------------------------------------------- */

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    left_btn_down  = true;
                    click_resolved = false;
                    click_start_x  = ev.button.x;
                    click_start_y  = ev.button.y;

                    /* Gizmo handle hit → start drag immediately */
                    MopPickResult p = mop_viewport_pick(vp,
                        (int)ev.button.x, (int)ev.button.y);
                    MopGizmoAxis axis = mop_gizmo_test_pick(gizmo, p);
                    if (axis != MOP_GIZMO_AXIS_NONE) {
                        dragging_gizmo = true;
                        click_resolved = true;
                        drag_axis = axis;
                    }
                }
                if (ev.button.button == SDL_BUTTON_RIGHT)
                    dragging_r = true;
                break;

            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    if (left_btn_down && !click_resolved) {
                        /* Mouse barely moved — this is a click */
                        MopPickResult p = mop_viewport_pick(vp,
                            (int)ev.button.x, (int)ev.button.y);
                        MopGizmoAxis axis = mop_gizmo_test_pick(gizmo, p);
                        if (axis != MOP_GIZMO_AXIS_NONE) {
                            /* Clicked gizmo (no drag) — ignore */
                        } else if (p.hit && p.object_id >= 1
                                   && p.object_id <= MAX_SCENE_OBJECTS
                                   && scene[p.object_id-1].active) {
                            /* Select scene object */
                            if (selected_id != p.object_id) {
                                selected_id = p.object_id;
                                if (scene[selected_id-1].gizmo_enabled) {
                                    mop_gizmo_show(gizmo,
                                        scene[selected_id-1].position);
                                    mop_gizmo_set_rotation(gizmo,
                                        scene[selected_id-1].rotation);
                                } else {
                                    mop_gizmo_hide(gizmo);
                                }
                                printf("Selected object %u\n", selected_id);
                            }
                        } else {
                            /* Clicked empty — deselect */
                            if (selected_id) {
                                mop_gizmo_hide(gizmo);
                                selected_id = 0;
                                printf("Deselected\n");
                            }
                        }
                    }
                    left_btn_down  = false;
                    dragging_l     = false;
                    dragging_gizmo = false;
                    click_resolved = false;
                }
                if (ev.button.button == SDL_BUTTON_RIGHT)
                    dragging_r = false;
                break;

            case SDL_EVENT_MOUSE_MOTION:
                /* Promote pending left-click to orbit drag once threshold
                 * is exceeded — selection is preserved. */
                if (left_btn_down && !click_resolved && !dragging_gizmo) {
                    float dx = ev.motion.x - click_start_x;
                    float dy = ev.motion.y - click_start_y;
                    if (dx*dx + dy*dy > CLICK_THRESHOLD*CLICK_THRESHOLD) {
                        click_resolved = true;
                        dragging_l = true;
                    }
                }

                /* Gizmo drag */
                if (dragging_gizmo && selected_id) {
                    SceneObject *sel = &scene[selected_id-1];
                    MopGizmoDelta d = mop_gizmo_drag(gizmo, drag_axis,
                        ev.motion.xrel, ev.motion.yrel);

                    sel->position = mop_vec3_add(sel->position, d.translate);
                    sel->rotation = mop_vec3_add(sel->rotation, d.rotate);
                    sel->scale_val = mop_vec3_add(sel->scale_val, d.scale);

                    /* Clamp scale to minimum */
                    if (sel->scale_val.x < 0.05f) sel->scale_val.x = 0.05f;
                    if (sel->scale_val.y < 0.05f) sel->scale_val.y = 0.05f;
                    if (sel->scale_val.z < 0.05f) sel->scale_val.z = 0.05f;

                    mop_gizmo_set_position(gizmo, sel->position);
                    mop_gizmo_set_rotation(gizmo, sel->rotation);
                } else if (dragging_l) {
                    cam.yaw   -= ev.motion.xrel * 0.005f;
                    cam.pitch += ev.motion.yrel * 0.005f;
                    if (cam.pitch >  1.5f) cam.pitch =  1.5f;
                    if (cam.pitch < -1.5f) cam.pitch = -1.5f;
                }
                if (dragging_r) {
                    MopVec3 right = { cosf(cam.yaw), 0, -sinf(cam.yaw) };
                    float s = cam.distance * 0.003f;
                    cam.target.x -= right.x * ev.motion.xrel * s;
                    cam.target.z -= right.z * ev.motion.xrel * s;
                    cam.target.y += ev.motion.yrel * s;
                }
                break;

            case SDL_EVENT_MOUSE_WHEEL:
                cam.distance -= ev.wheel.y * 0.3f;
                if (cam.distance < 0.5f)  cam.distance = 0.5f;
                if (cam.distance > 30.0f) cam.distance = 30.0f;
                break;

            case SDL_EVENT_WINDOW_RESIZED: {
                int w = ev.window.data1, h = ev.window.data2;
                if (w > 0 && h > 0) {
                    win_w = w; win_h = h;
                    mop_viewport_resize(vp, w, h);
                    SDL_DestroyTexture(tex);
                    tex = SDL_CreateTexture(renderer,
                        SDL_PIXELFORMAT_ABGR8888,
                        SDL_TEXTUREACCESS_STREAMING, w, h);
                }
                break;
            }

            default: break;
            }
        }

        /* ---- Arrow key camera movement ---- */
        {
            const bool *keys = SDL_GetKeyboardState(NULL);
            float speed = 3.0f * dt;
            float fwd_x = -sinf(cam.yaw), fwd_z = -cosf(cam.yaw);
            float rgt_x =  cosf(cam.yaw), rgt_z = -sinf(cam.yaw);
            if (keys[SDL_SCANCODE_UP])    { cam.target.x += fwd_x*speed; cam.target.z += fwd_z*speed; }
            if (keys[SDL_SCANCODE_DOWN])  { cam.target.x -= fwd_x*speed; cam.target.z -= fwd_z*speed; }
            if (keys[SDL_SCANCODE_RIGHT]) { cam.target.x += rgt_x*speed; cam.target.z += rgt_z*speed; }
            if (keys[SDL_SCANCODE_LEFT])  { cam.target.x -= rgt_x*speed; cam.target.z -= rgt_z*speed; }
        }

        /* ---- Update scene transforms ---- */
        for (int i = 0; i < MAX_SCENE_OBJECTS; i++) {
            SceneObject *obj = &scene[i];
            if (!obj->active) continue;
            if (obj->auto_rotates && obj->object_id != selected_id)
                obj->rotation.y += dt * 0.8f;
            MopMat4 xf = compose_trs(obj->position, obj->rotation,
                                     obj->scale_val);
            mop_mesh_set_transform(obj->mesh, &xf);
        }
        if (selected_id && scene[selected_id-1].gizmo_enabled) {
            mop_gizmo_set_position(gizmo, scene[selected_id-1].position);
            mop_gizmo_set_rotation(gizmo, scene[selected_id-1].rotation);
        }

        orbit_apply(&cam, vp);

        /* ---- Render ---- */
        mop_viewport_render(vp);

        /* ---- Blit CPU framebuffer -> SDL texture -> window ---- */
        int fb_w, fb_h;
        const uint8_t *px = mop_viewport_read_color(vp, &fb_w, &fb_h);
        if (px && tex) {
            SDL_UpdateTexture(tex, NULL, px, fb_w * 4);
            SDL_RenderClear(renderer);
            SDL_RenderTexture(renderer, tex, NULL, NULL);
            SDL_RenderPresent(renderer);
        }
    }

    /* ---- Cleanup ---- */
    printf("Shutting down...\n");
    mop_gizmo_destroy(gizmo);
    SDL_DestroyTexture(tex);
    mop_viewport_destroy(vp);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    printf("Clean shutdown.\n");
    return 0;
}
