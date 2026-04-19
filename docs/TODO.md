---
title: "MOP Continuation / Handoff Notes"
description: "Persistent status of outstanding work on the Master of Puppets rendering engine. Update this as items land."
slug: "todo"
author: "rahulmnavneeth"
date: "2026-04-19"
tags: ["todo", "roadmap", "continuation"]
---

Persistent handoff for MOP work in progress. The goal is that anyone (including a fresh Claude session) can pick up exactly where the current state left off.

## Where we are

MOP is a backend-agnostic rasterizer (CPU / OpenGL / Vulkan) targeting DCC-plugin and game-engine embedding. Core architectural work done in the recent push:

- Pointer-stable mesh / instanced-mesh pools with O(1) free-list allocation.
- Recursive scene mutex; hot mutation APIs auto-lock; host-explicit `mop_viewport_scene_lock` for batching.
- Topological single-pass transform propagation (unlimited depth).
- RTT via `mop_viewport_present_to_texture` with SSAA-downsample on blit.
- HDRI environment properly wired; skybox decoupled from editor chrome.
- Vulkan transient staging for oversize (>16 MB) texture upload + readback.
- Showcase example demonstrating threading + RTT + HDRI + orbit camera.
- TSan-clean under concurrent mutation-plus-render stress (see `tests/test_scene_threading.c`).

## What's outstanding

Grouped by effort. Check items off here as they land.

### Small — DONE this round

- [x] **Auto-lock the remaining public APIs (lifecycle / viewport-first).** Wrapped: `add_light`, `remove_light`, `clear_lights`, `add_camera`, `remove_camera`, `set_active_camera`, `add_decal`, `remove_decal`, `clear_decals`, `set_environment`, `_rotation`, `_intensity`, `_background`, `_procedural_sky`, `add_overlay`, `remove_overlay`, `set_overlay_enabled`, `set_post_effects`, `set_fog`, `set_exposure`, `set_ssr`, `set_volumetric`, `set_bloom`, `register_shader`, `unregister_shader`.
- [x] **OpenGL backend copy paths.** Implemented `gl_framebuffer_copy_to_texture` (via `glBlitFramebuffer` into a transient draw FBO attached to the target texture) and `gl_texture_read_rgba8` (via `glGetTexImage`). No longer NULL in the GL backend table.
- [x] **`mop_viewport_render_to` convenience API.** Ships in `include/mop/core/viewport.h` + `src/core/viewport.c`. Fuses render + present in one call.
- [x] **Silenced singular matrix warnings.** Downgraded to `MOP_DEBUG` in `src/math/math.c` — skybox path no longer spams the log.

### Still small (pick up next round)

- [x] **Individual entity setters** — `MopLight` and `MopCameraObject` now carry a `viewport` back-pointer (set at add time). All `mop_light_set_*` and `mop_camera_object_set_*` setters auto-lock.
- [x] **Texture pipeline + undo + selection + input** — `mop_tex_create`, `mop_tex_load_async`, `mop_tex_cache_flush`, `mop_tex_read_rgba8`, `mop_viewport_push_undo*`, `mop_viewport_undo`, `mop_viewport_redo`, `mop_viewport_select_*`, `mop_viewport_deselect_*`, `mop_viewport_clear_selection`, `mop_viewport_toggle_element`, `mop_viewport_input`, `mop_viewport_poll_event`, `mop_mesh_set_edit_mode` all auto-lock.
- [ ] **Interactive mesh edit ops** — the 9 `mop_mesh_edit_*` functions (move/delete/merge/split/dissolve/extrude/inset/flip-normals) are long, complex, and have many early returns. Wrapping them safely requires goto-cleanup refactors. Today they're callable from worker threads only if the host wraps with explicit `scene_lock`. Not hot-path; typically driven by a single UI thread.
- [ ] **Material graph public API** — `mop_mat_graph_*` additions/removals + `mop_mat_graph_compile`. Lock-wrap in the same pattern as other files.

### Medium — DONE this round

- [x] **First-class vertex format presets.** `mop_vertex_format_posonly()`, `_pos_normal()`, `_pos_normal_uv()` joined the existing `_standard()`.

- [x] **Flexible format first-class (additive, no ABI break).** `MopMeshDesc` gained an optional `vertex_format` field. When non-NULL, `mop_viewport_add_mesh` routes through the `_ex` path automatically. Existing code keeps working because the field defaults to NULL via designated initializers. No need to rename `_ex` — the existing function stays as an alias.

- [x] **Host-owned render target.** RHI method `framebuffer_create_from_texture` added. CPU implementation does zero-copy by aliasing the host texture's pixel buffer as `MopSwFramebuffer.color` (new `mop_sw_framebuffer_alloc_wrapping` helper + `color_external` flag). Vulkan and OpenGL return NULL → viewport falls back to internal framebuffer and logs a warning (real wrapping of host `VkImage` / `GLuint` needs render-pass format matching and host-owned layout-transition contract — flagged in docs/TODO.md below). Public API: `MopViewportDesc.render_target`.

- [x] **MDI activation (scaffolding + toggle).** RHI method `set_gpu_driven_rendering(dev, bool)` + public `mop_viewport_set_gpu_driven_rendering`. Vulkan implementation flips `indirect_draw_requested` which gates the existing warm-up counter. The actual per-pipeline-bucket `vkCmdDrawIndexedIndirectCount` dispatch is still pending (docs below). Toggling today is safe but a no-op on the draw path.

- [x] **GPU skinning scaffolding.** New `src/backend/vulkan/shaders/mop_skin.comp` (GLSL 450 compute: linear-blend skinning over format-agnostic raw vertex buffers + bone matrix SSBO). `compile.sh` picks it up and defines `MOP_VK_HAS_SKIN_SHADER` when glslc succeeds. `vk_device_create` builds the pipeline + descriptor layout behind that guard; sets `dev->skin_enabled`. The dispatch helper and integration with `mop_skin_apply` (gate CPU path on backend) are the remaining wire-in steps.

### Still medium (pick up next round)

- [ ] **Vulkan host-owned render target.** Wrap an externally-provided `VkImage` as the framebuffer color attachment. Render pass + pipeline-cache need to match the host image's format; host owns layout transitions before/after the MOP frame. Tricky lifetime contract — spec it carefully before coding.
- [ ] **OpenGL host-owned render target.** Wrap host `GLuint` texture into an FBO color attachment.
- [ ] **Activate MDI end-to-end.** In `vk_draw()`, when `dev->indirect_draw_requested && dev->indirect_draw_enabled`, skip the immediate `vkCmdDrawIndexed` and only populate `input_draw_cmds`. After each pass (opaque / transparent), emit `vkCmdDrawIndexedIndirectCount` against `output_draw_cmds` + `draw_count_buf`. Requires pipeline bucketing — group by (blend_mode, wireframe, backface_cull) and emit one dispatch per bucket. Uber-shader with bindless textures makes this tractable; existing `mop_solid_bindless.*` shaders are a starting point.
- [ ] **Dispatch helper for GPU skinning.** Add `mop_vk_dispatch_skin(dev, cmd_buf, bind_pose_buf, output_buf, bones_buf, push_consts)` in `vulkan_pipeline.c`. Wire into `mop_skin_apply`: when `vp->backend_type == MOP_BACKEND_VULKAN && vp->rhi->dispatch_skin`, use it; else fall back to CPU.

### Large / architectural (1–2 weeks each)

- [ ] **Copy-on-write scene snapshot.** Removes the single-mutex bottleneck. Render thread reads an immutable snapshot; mutation threads write to a shadow; swap at frame boundary. Major refactor: every mutation must clone-on-write the affected entity (mesh, light, etc.), and the render path must deref handles through the snapshot index.

- [ ] **Fully GPU-driven rendering pipeline.** Replace per-mesh CPU dispatch with per-bucket indirect dispatches across shadow / opaque / transparent. Requires multi-draw-indirect (above), plus a unified uber-material shader, plus per-frame pipeline state caching.

- [ ] **Asset streaming / virtual texturing.** `MopTexStreamState` enum exists but machinery doesn't. Progressive mip loading, eviction policy, async I/O pipeline, sparse textures.

- [ ] **Fine-grained locking.** Current one-mutex design serializes all mutations. Options: per-mesh locks, seqlock for readers, epoch-based reclamation (EBR).

### Out of scope (separate engineering)

- Ray tracing (`VK_KHR_acceleration_structure`).
- Mesh shaders as first-class (infrastructure exists on native Vulkan, not wired into main path).
- Compute shaders beyond skinning (plugin accepts compute SPIR-V but doesn't dispatch).
- Frame-budget telemetry / OOM recovery / shipping-title infrastructure.

## Known bugs / polish

- [x] `docs/reference/viewport.mdx` refreshed with threading + RTT + HDRI + GPU-driven + vertex format preset sections.
- [x] `examples/frames/` added to `.gitignore`.
- [x] Undo/redo survives concurrent add/remove — `test_scene_threading.c` added `undo_redo_after_mesh_removal` case. The `find_mesh_index` path uses mesh `slot_index` + pool validation, so stale entries are safely skipped.
- [ ] Other reference `.mdx` files (material.mdx, light.mdx, camera.mdx) haven't been cross-referenced against recent API changes. Low priority — TODO.md and viewport.mdx are the canonical handoff.

## Testing notes

- `make test` runs all 22+ test files, including `test_scene_threading` (TSan-clean).
- `make SANITIZE=tsan test` re-runs with ThreadSanitizer instrumentation on libmop.
- `make SANITIZE=asan test` runs with AddressSanitizer.
- Showcase: `./build/examples/showcase --vulkan --4k` renders 60 4K frames to `/tmp/mop-showcase-out/`.

## Conventions reminder (from CLAUDE.md)

- C11, Make + Nix only (no CMake).
- Column-major matrices: `M(mat, row, col) = mat.d[col * 4 + row]`.
- CCW triangle winding viewed from outside (verify with `cross(e1, e2) · normal > 0`).
- `.mdx` docs follow the musikell convention with YAML frontmatter.
- Prefer editing existing files to creating new ones.
- No comments that narrate "what" — only non-obvious "why".
