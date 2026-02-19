# Master of Puppets

Backend-agnostic viewport rendering engine in C11.

Master of Puppets renders 3D geometry through interchangeable backends — CPU software rasterization, OpenGL, or Vulkan — behind a single public API with opaque handles.  Designed as reusable infrastructure for DCC tools and 3D applications.

## Usage

```c
#include <mop/mop.h>

MopViewport *vp = mop_viewport_create(&(MopViewportDesc){
    .width = 800, .height = 600, .backend = MOP_BACKEND_CPU
});

MopMesh *mesh = mop_viewport_add_mesh(vp, &mesh_desc);
mop_mesh_set_transform(mesh, &rotation);
mop_viewport_render(vp);

const uint8_t *pixels = mop_viewport_read_color(vp, &w, &h);
MopPickResult pick = mop_viewport_pick(vp, mouse_x, mouse_y);

mop_viewport_destroy(vp);
```

## Build

```bash
nix develop          # Enter development shell
make                 # Build static library (libmop.a)
```

Without Nix:

```bash
make CC=clang        # Or CC=gcc
```

## Examples

Examples live in `examples/` with their own Makefile and Nix flake.

```bash
cd examples
nix develop          # Provides SDL3 + pkg-config
make                 # Build all examples
make run             # Run interactive rotating cube (SDL3)
make run-headless    # Run headless PPM exporter
```

## Switching Backends

```c
.backend = MOP_BACKEND_CPU      /* Always available, no GPU needed */
.backend = MOP_BACKEND_OPENGL   /* OpenGL 3.3 (contract only)     */
.backend = MOP_BACKEND_VULKAN   /* Vulkan 1.0 (contract only)     */
.backend = MOP_BACKEND_AUTO     /* Platform default                */
```

## Architecture

```
Application  →  Viewport Public API  →  Viewport Core  →  RHI  →  Backends
```

The application sees only opaque handles and value types.  The RHI is a function-pointer table that backends implement.  Backend selection happens at runtime.

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
