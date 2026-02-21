# Master of Puppets

Backend-agnostic viewport rendering engine in C11.

Master of Puppets renders 3D geometry through interchangeable backends — CPU software rasterization, OpenGL, or Vulkan — behind a single public API with opaque handles.  Designed as reusable infrastructure for DCC tools and 3D applications.

## Usage

```c
#include <mop/mop.h>

/* Create a viewport with the CPU backend */
MopViewport *vp = mop_viewport_create(&(MopViewportDesc){
    .width = 800, .height = 600, .backend = MOP_BACKEND_CPU
});

/* Camera */
mop_viewport_set_camera(vp,
    (MopVec3){ 0, 3, 8 }, (MopVec3){ 0, 0, 0 }, (MopVec3){ 0, 1, 0 },
    45.0f, 0.1f, 100.0f);

/* Add a mesh */
MopMesh *mesh = mop_viewport_add_mesh(vp, &(MopMeshDesc){
    .vertices = verts, .vertex_count = nv,
    .indices = idxs,   .index_count = ni,
    .object_id = 1,
});
mop_mesh_set_position(mesh, (MopVec3){ 0, 0.5f, 0 });

/* Add lights */
MopLight *sun = mop_viewport_add_light(vp, &(MopLight){
    .type = MOP_LIGHT_DIRECTIONAL,
    .direction = { 0.3f, 1.0f, 0.5f },
    .color = { 1, 1, 0.95f, 1 },
    .intensity = 1.0f, .active = true,
});

/* Feed input events for interaction */
mop_viewport_input(vp, &(MopInputEvent){
    MOP_INPUT_POINTER_DOWN, mouse_x, mouse_y, 0, 0, 0
});

/* Render and read back */
mop_viewport_render(vp);
const uint8_t *pixels = mop_viewport_read_color(vp, &w, &h);

/* Poll output events */
MopEvent ev;
while (mop_viewport_poll_event(vp, &ev)) {
    if (ev.type == MOP_EVENT_SELECTED)
        printf("Selected object %u\n", ev.object_id);
}

/* Pick objects */
MopPickResult pick = mop_viewport_pick(vp, mouse_x, mouse_y);

mop_viewport_destroy(vp);
```

## Build

```bash
nix develop          # Enter development shell
make                 # Build static library (libmop.a)
make test            # Run test suite
```

Optional backends and features:

```bash
make MOP_ENABLE_VULKAN=1   # Include Vulkan backend
make MOP_ENABLE_OPENGL=1   # Include OpenGL backend
make MOP_ENABLE_LUA=1      # Include Lua configuration system
```

Without Nix:

```bash
make CC=clang        # Or CC=gcc
```

## Examples

Examples live in `examples/` with their own Makefile and Nix flake.

```bash
cd examples
nix develop                        # Provides SDL3 + pkg-config
make                               # Build all examples
make run                           # Run interactive showcase (SDL3)
make run-headless                  # Run headless PPM exporter
MOP_BACKEND=vulkan ./build/mop_showcase  # Run with Vulkan
```

## Switching Backends

```c
.backend = MOP_BACKEND_CPU      /* Always available, no GPU needed   */
.backend = MOP_BACKEND_OPENGL   /* OpenGL 3.3 (compile with MOP_ENABLE_OPENGL=1) */
.backend = MOP_BACKEND_VULKAN   /* Vulkan 1.0 headless (compile with MOP_ENABLE_VULKAN=1) */
.backend = MOP_BACKEND_AUTO     /* Platform default                  */
```

## Architecture

```
Application  →  Viewport Public API  →  Viewport Core  →  RHI  →  Backends
                                     →  Interaction     →
```

The application sees only opaque handles and value types.  The RHI is a function-pointer table that backends implement.  Backend selection happens at runtime.

### Core Systems

| System | Description |
|--------|-------------|
| **Viewport** | Scene management, render orchestration, framebuffer readback |
| **Lights** | Multi-light shading — directional, point, spot (max 8) with interactive indicators |
| **Gizmo** | Translate/rotate/scale handles with screen-space scaling |
| **Input** | Event-driven interaction state machine (orbit, pan, select, drag) |
| **Camera** | Orbit camera with pan, zoom, and WASD movement |
| **Overlays** | Wireframe, normals, bounds, selection highlight (built-in + custom) |
| **Picking** | Object ID buffer — click to select meshes and lights |
| **Undo/Redo** | Transform history stack |
| **Particles** | Emitters with smoke, fire, sparks presets |
| **Water** | Procedural sine-wave animated surface |
| **Post-Process** | Gamma, tonemapping, vignette, fog |
| **Spatial** | AABB, frustum culling, raycasting |
| **Loaders** | OBJ and binary .mop format |

## Extending

1. Implement the 14-function `MopRhiBackend` table
2. Register in `src/rhi/rhi.c`
3. Add source to root `Makefile`
4. Document in `docs/reference/backend-<name>.mdx`

See [docs/architecture/extension.mdx](docs/architecture/extension.mdx) for the full guide.

## Documentation

- [docs/index.mdx](docs/index.mdx) — Documentation root and module index
- [docs/architecture.mdx](docs/architecture.mdx) — System architecture and layer model
- [docs/reference.mdx](docs/reference.mdx) — Module-level API reference
- [docs/development.mdx](docs/development.mdx) — Build, platform, and contributor guide

## License

Apache-2.0 — see [LICENSE](LICENSE).
