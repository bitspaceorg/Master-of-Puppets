/*
 * Master of Puppets — Interactive Viewport Example (SDL3)
 *
 * A real-time interactive viewport rendered by the CPU backend and
 * displayed in an SDL3 window.  The app only maps SDL events to MOP
 * input enums and reacts to MOP output events.  All interaction logic
 * (selection, gizmo, camera, click-vs-drag) is owned by MOP.
 *
 * Keybindings are loaded from mop.lua if available; otherwise hardcoded
 * defaults are used.  Edit mop.lua to remap any key.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <mop/mop.h>
#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* -------------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------------- */

#define WINDOW_W          960
#define WINDOW_H          720
#define MAX_SCENE_OBJECTS 128

/* -------------------------------------------------------------------------
 * Types — app-level registry (business logic only)
 * ------------------------------------------------------------------------- */

typedef struct {
    bool     active;
    uint32_t object_id;
    MopMesh *mesh;
    bool     auto_rotates;
} SceneObject;

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
    obj->auto_rotates = auto_rot;
    mop_mesh_set_position(obj->mesh, pos);
    return obj;
}

/* -------------------------------------------------------------------------
 * SDL key name → config key name (lowercase)
 * ------------------------------------------------------------------------- */

static const char *sdl_key_to_config_name(SDL_Keycode key, char *buf, int len) {
    const char *name = SDL_GetKeyName(key);
    if (!name || !name[0]) return NULL;

    int i = 0;
    while (name[i] && i < len - 1) {
        buf[i] = (char)tolower((unsigned char)name[i]);
        i++;
    }
    buf[i] = '\0';
    return buf;
}

/* -------------------------------------------------------------------------
 * OBJ import file dialog callback (SDL3 async)
 * ------------------------------------------------------------------------- */

static void obj_dialog_callback(void *userdata, const char *const *filelist,
                                 int filter) {
    (void)filter;
    char **out = (char **)userdata;
    if (filelist && filelist[0]) {
        *out = strdup(filelist[0]);
    }
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

    /* ---- Load optional Lua config ---- */
    MopConfig *cfg = mop_config_load("mop.lua");
    if (cfg) {
        mop_config_apply(cfg, vp);
        printf("Loaded config from mop.lua\n");
    }

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

    /* ---- SDL texture for CPU framebuffer blit ---- */
    int win_w = WINDOW_W, win_h = WINDOW_H;
    SDL_Texture *tex = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ABGR8888,
        SDL_TEXTUREACCESS_STREAMING, win_w, win_h
    );

    /* ---- OBJ import state (for async SDL file dialog) ---- */
    char *pending_obj_path = NULL;

    bool running = true;
    srand((unsigned)SDL_GetPerformanceCounter());

    Uint64 last = SDL_GetPerformanceCounter();
    Uint64 freq = SDL_GetPerformanceFrequency();

    printf("Master of Puppets — Interactive Viewport (SDL3 + CPU rasterizer)\n");
    printf("  Left-drag: orbit  |  Right-drag: pan  |  Scroll: zoom\n");
    printf("  Click: select  |  T: translate  |  G: rotate  |  E: scale\n");
    printf("  W: wireframe  |  Space: pause  |  R: reset  |  Esc: deselect/quit\n");
    printf("  Arrow keys: move camera  |  S: spawn cube  |  I: import .obj\n");

    /* ---- Event loop ---- */
    while (running) {
        Uint64 now = SDL_GetPerformanceCounter();
        float dt = (float)(now - last) / (float)freq;
        last = now;

        /* ---- SDL -> MOP input mapping ---- */
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {

            case SDL_EVENT_QUIT:
                running = false;
                break;

            /* ----------------------------------------------------------
             * Keyboard — config-driven dispatch
             *
             * 1. Convert SDL key name to lowercase config key name
             * 2. Look up action in config keymap
             * 3. If it's a known MOP action → mop_viewport_input
             * 4. If it's an app action → handle locally
             * 5. Fallback: hardcoded defaults (no config loaded)
             * ---------------------------------------------------------- */
            case SDL_EVENT_KEY_DOWN: {
                char key_buf[32];
                const char *key_name = sdl_key_to_config_name(
                    ev.key.key, key_buf, sizeof(key_buf));
                const char *action = key_name
                    ? mop_config_get_action(cfg, key_name) : NULL;

                if (action) {
                    /* Try as MOP input first */
                    int input = mop_config_resolve_input(action);
                    if (input >= 0) {
                        /* "deselect" has special app logic: quit if nothing selected */
                        if (input == MOP_INPUT_DESELECT
                            && !mop_viewport_get_selected(vp)) {
                            running = false;
                        } else {
                            mop_viewport_input(vp,
                                &(MopInputEvent){.type = input});
                        }
                    }
                    /* App-specific actions */
                    else if (strcmp(action, "quit") == 0) {
                        running = false;
                    }
                    else if (strcmp(action, "toggle_auto_rotate") == 0) {
                        scene[cube_id-1].auto_rotates =
                            !scene[cube_id-1].auto_rotates;
                    }
                    else if (strcmp(action, "spawn_cube") == 0) {
                        if (next_id <= MAX_SCENE_OBJECTS) {
                            float rx = ((float)rand()/(float)RAND_MAX)*6.0f-3.0f;
                            float rz = ((float)rand()/(float)RAND_MAX)*6.0f-3.0f;
                            uint32_t sid = next_id++;
                            scene_add(scene, vp, &(MopMeshDesc){
                                .vertices=CUBE_VERTS, .vertex_count=CUBE_VERT_COUNT,
                                .indices=CUBE_INDICES, .index_count=CUBE_INDEX_COUNT,
                                .object_id=sid
                            }, (MopVec3){rx, 0.5f, rz}, false);
                            printf("Spawned cube #%u at (%.1f, %.1f)\n",
                                   sid, rx, rz);
                        }
                    }
                    else if (strcmp(action, "import_obj") == 0) {
                        static const SDL_DialogFileFilter obj_filter[] = {
                            { "Wavefront OBJ", "obj" },
                        };
                        SDL_ShowOpenFileDialog(obj_dialog_callback,
                            &pending_obj_path, window,
                            obj_filter, 1, NULL, false);
                    }
                } else {
                    /* Fallback: hardcoded defaults when no config */
                    switch (ev.key.key) {
                    case SDLK_Q: running = false; break;
                    case SDLK_ESCAPE:
                        if (mop_viewport_get_selected(vp))
                            mop_viewport_input(vp, &(MopInputEvent){
                                .type = MOP_INPUT_DESELECT});
                        else running = false;
                        break;
                    case SDLK_T:
                        mop_viewport_input(vp, &(MopInputEvent){
                            .type = MOP_INPUT_MODE_TRANSLATE}); break;
                    case SDLK_G:
                        mop_viewport_input(vp, &(MopInputEvent){
                            .type = MOP_INPUT_MODE_ROTATE}); break;
                    case SDLK_E:
                        mop_viewport_input(vp, &(MopInputEvent){
                            .type = MOP_INPUT_MODE_SCALE}); break;
                    case SDLK_W:
                        mop_viewport_input(vp, &(MopInputEvent){
                            .type = MOP_INPUT_TOGGLE_WIREFRAME}); break;
                    case SDLK_R:
                        mop_viewport_input(vp, &(MopInputEvent){
                            .type = MOP_INPUT_RESET_VIEW}); break;
                    case SDLK_SPACE:
                        scene[cube_id-1].auto_rotates =
                            !scene[cube_id-1].auto_rotates;
                        break;
                    case SDLK_S:
                        if (next_id <= MAX_SCENE_OBJECTS) {
                            float rx = ((float)rand()/(float)RAND_MAX)*6.0f-3.0f;
                            float rz = ((float)rand()/(float)RAND_MAX)*6.0f-3.0f;
                            uint32_t sid = next_id++;
                            scene_add(scene, vp, &(MopMeshDesc){
                                .vertices=CUBE_VERTS, .vertex_count=CUBE_VERT_COUNT,
                                .indices=CUBE_INDICES, .index_count=CUBE_INDEX_COUNT,
                                .object_id=sid
                            }, (MopVec3){rx, 0.5f, rz}, false);
                            printf("Spawned cube #%u at (%.1f, %.1f)\n",
                                   sid, rx, rz);
                        }
                        break;
                    case SDLK_I: {
                        static const SDL_DialogFileFilter obj_filter[] = {
                            { "Wavefront OBJ", "obj" },
                        };
                        SDL_ShowOpenFileDialog(obj_dialog_callback,
                            &pending_obj_path, window,
                            obj_filter, 1, NULL, false);
                        break;
                    }
                    default: break;
                    }
                }
                break;
            }

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (ev.button.button == SDL_BUTTON_LEFT)
                    mop_viewport_input(vp, &(MopInputEvent){
                        MOP_INPUT_POINTER_DOWN, ev.button.x, ev.button.y,
                        0, 0, 0});
                else if (ev.button.button == SDL_BUTTON_RIGHT)
                    mop_viewport_input(vp, &(MopInputEvent){
                        MOP_INPUT_SECONDARY_DOWN, ev.button.x, ev.button.y,
                        0, 0, 0});
                break;

            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (ev.button.button == SDL_BUTTON_LEFT)
                    mop_viewport_input(vp, &(MopInputEvent){
                        MOP_INPUT_POINTER_UP, ev.button.x, ev.button.y,
                        0, 0, 0});
                else if (ev.button.button == SDL_BUTTON_RIGHT)
                    mop_viewport_input(vp, &(MopInputEvent){
                        .type = MOP_INPUT_SECONDARY_UP});
                break;

            case SDL_EVENT_MOUSE_MOTION:
                mop_viewport_input(vp, &(MopInputEvent){
                    MOP_INPUT_POINTER_MOVE,
                    ev.motion.x, ev.motion.y,
                    ev.motion.xrel, ev.motion.yrel, 0});
                break;

            case SDL_EVENT_MOUSE_WHEEL:
                mop_viewport_input(vp, &(MopInputEvent){
                    .type = MOP_INPUT_SCROLL,
                    .scroll = ev.wheel.y});
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

        /* ---- Continuous camera movement (arrow keys) ---- */
        {
            const bool *keys = SDL_GetKeyboardState(NULL);
            float speed = 3.0f * dt;
            float fwd = 0, rgt = 0;
            if (keys[SDL_SCANCODE_UP])    fwd += speed;
            if (keys[SDL_SCANCODE_DOWN])  fwd -= speed;
            if (keys[SDL_SCANCODE_RIGHT]) rgt += speed;
            if (keys[SDL_SCANCODE_LEFT])  rgt -= speed;
            if (fwd != 0 || rgt != 0)
                mop_viewport_input(vp, &(MopInputEvent){
                    .type = MOP_INPUT_CAMERA_MOVE,
                    .dx = rgt, .dy = fwd});
        }

        /* ---- Process pending OBJ import ---- */
        if (pending_obj_path) {
            MopObjMesh obj;
            if (mop_obj_load(pending_obj_path, &obj)
                && next_id <= MAX_SCENE_OBJECTS) {
                uint32_t sid = next_id++;
                /* Place model so its bottom sits on the ground plane.
                 * The loader centers at origin, so bbox_min.y is negative.
                 * Shift up by -bbox_min.y. */
                float ground_y = -obj.bbox_min.y;
                SceneObject *o = scene_add(scene, vp, &(MopMeshDesc){
                    .vertices = obj.vertices,
                    .vertex_count = obj.vertex_count,
                    .indices  = obj.indices,
                    .index_count = obj.index_count,
                    .object_id = sid
                }, (MopVec3){0, ground_y, 0}, false);
                if (o) printf("Imported OBJ #%u (%u verts, %u tris) from %s\n",
                    sid, obj.vertex_count, obj.index_count / 3,
                    pending_obj_path);
                mop_obj_free(&obj);
            } else {
                printf("Failed to load OBJ: %s\n", pending_obj_path);
            }
            free(pending_obj_path);
            pending_obj_path = NULL;
        }

        /* ---- Poll MOP events — app reacts ---- */
        MopEvent mev;
        while (mop_viewport_poll_event(vp, &mev)) {
            if (mev.type == MOP_EVENT_SELECTED)
                printf("Selected object %u\n", mev.object_id);
            if (mev.type == MOP_EVENT_DESELECTED)
                printf("Deselected\n");
        }

        /* ---- App-specific: auto-rotate unselected cubes ---- */
        uint32_t sel = mop_viewport_get_selected(vp);
        for (int i = 0; i < MAX_SCENE_OBJECTS; i++) {
            SceneObject *o = &scene[i];
            if (!o->active || !o->auto_rotates) continue;
            if (o->object_id == sel) continue;
            MopVec3 r = mop_mesh_get_rotation(o->mesh);
            r.y += dt * 0.8f;
            mop_mesh_set_rotation(o->mesh, r);
        }

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
    free(pending_obj_path);
    mop_config_free(cfg);
    SDL_DestroyTexture(tex);
    mop_viewport_destroy(vp);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    printf("Clean shutdown.\n");
    return 0;
}
