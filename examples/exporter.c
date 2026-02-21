/*
 * Master of Puppets — Example: Scene Exporter (Interactive)
 *
 * Interactive scene viewer with on-demand export.  Orbit the camera
 * to inspect the scene, then press E to dump a full export.
 * E=export to console  S=save .mop scene  W=wireframe  Q/Esc=quit
 *
 * APIs: query.h, camera_query.h, light.h, material.h
 * SPDX-License-Identifier: Apache-2.0
 */

#include <mop/mop.h>
#include "geometry.h"
#include "sdl_harness.h"

#include <stdio.h>
#include <stdbool.h>
#include <math.h>

/* =========================================================================
 * Context
 * ========================================================================= */

typedef struct {
    int export_count;
} ExporterCtx;

/* =========================================================================
 * Print helpers
 * ========================================================================= */

static void print_vec3(const char *label, MopVec3 v)
{
    printf("  %-18s (%.4f, %.4f, %.4f)\n", label, v.x, v.y, v.z);
}

static void print_color(const char *label, MopColor c)
{
    printf("  %-18s (%.3f, %.3f, %.3f, %.3f)\n", label, c.r, c.g, c.b, c.a);
}

static void print_mat4(const char *label, MopMat4 m)
{
    printf("  %s:\n", label);
    for (int row = 0; row < 4; row++) {
        printf("    [%8.4f %8.4f %8.4f %8.4f]\n",
               m.d[0 * 4 + row], m.d[1 * 4 + row],
               m.d[2 * 4 + row], m.d[3 * 4 + row]);
    }
}

static const char *light_type_str(MopLightType t)
{
    switch (t) {
    case MOP_LIGHT_DIRECTIONAL: return "directional";
    case MOP_LIGHT_POINT:       return "point";
    case MOP_LIGHT_SPOT:        return "spot";
    default:                    return "unknown";
    }
}

static const char *blend_mode_str(MopBlendMode m)
{
    switch (m) {
    case MOP_BLEND_OPAQUE:   return "opaque";
    case MOP_BLEND_ALPHA:    return "alpha";
    case MOP_BLEND_ADDITIVE: return "additive";
    case MOP_BLEND_MULTIPLY: return "multiply";
    default:                 return "unknown";
    }
}

/* =========================================================================
 * Export routine — prints full scene to console
 * ========================================================================= */

static void do_export(MopViewport *vp, int export_num)
{
    printf("\n");
    printf("======================================================\n");
    printf("  EXPORT #%d\n", export_num);
    printf("======================================================\n\n");

    /* ---- Camera state ---- */
    printf("--- Camera State ---\n");
    MopCameraState cam = mop_viewport_get_camera_state(vp);
    print_vec3("eye:",    cam.eye);
    print_vec3("target:", cam.target);
    printf("  %-18s %.4f rad (%.1f deg)\n", "fov:",
           cam.fov_radians, cam.fov_radians * 180.0f / 3.14159265f);
    printf("  %-18s %.4f\n", "near:", cam.near_plane);
    printf("  %-18s %.4f\n", "far:",  cam.far_plane);
    printf("  %-18s %.4f\n", "aspect:", cam.aspect_ratio);
    print_mat4("view_matrix",       cam.view_matrix);
    print_mat4("projection_matrix", cam.projection_matrix);
    printf("\n");

    /* ---- Lights ---- */
    uint32_t num_lights = mop_viewport_light_count(vp);
    printf("--- Lights (%u) ---\n", num_lights);
    for (uint32_t i = 0; i < num_lights; i++) {
        const MopLight *l = mop_viewport_light_at(vp, i);
        if (!l) continue;
        printf("  light[%u] type=%-13s active=%s\n",
               i, light_type_str(l->type), l->active ? "yes" : "no");
        print_vec3("  position:",  l->position);
        print_vec3("  direction:", l->direction);
        print_color("  color:",    l->color);
        printf("    intensity=%.2f  range=%.2f\n", l->intensity, l->range);
    }
    printf("\n");

    /* ---- Per-mesh data ---- */
    uint32_t num_meshes = mop_viewport_mesh_count(vp);
    printf("--- Meshes (%u) ---\n", num_meshes);

    for (uint32_t mi = 0; mi < num_meshes; mi++) {
        MopMesh *mesh = mop_viewport_mesh_at(vp, mi);
        if (!mesh) continue;

        uint32_t oid   = mop_mesh_get_object_id(mesh);
        uint32_t nv    = mop_mesh_get_vertex_count(mesh);
        uint32_t ntri  = mop_mesh_get_triangle_count(mesh);
        float    alpha = mop_mesh_get_opacity(mesh);

        printf("\n  mesh[%u]  object_id=%u  verts=%u  tris=%u\n",
               mi, oid, nv, ntri);
        printf("  blend=%-8s  opacity=%.2f\n",
               blend_mode_str(mop_mesh_get_blend_mode(mesh)), alpha);

        /* World transform */
        MopMat4 world = mop_mesh_get_world_transform(mesh);
        print_mat4("world_transform", world);

        /* Material */
        if (mop_mesh_has_material(mesh)) {
            MopMaterial mat = mop_mesh_get_material(mesh);
            printf("  material:\n");
            print_color("  base_color:", mat.base_color);
            printf("    metallic=%.2f  roughness=%.2f\n",
                   mat.metallic, mat.roughness);
            print_vec3("  emissive:", mat.emissive);
        } else {
            printf("  material: (default)\n");
        }

        /* OBJ-style vertex dump (world-space) */
        const MopVertex  *verts   = mop_mesh_get_vertices(mesh, vp);
        const uint32_t   *indices = mop_mesh_get_indices(mesh, vp);

        if (verts) {
            printf("  # OBJ vertices (world-space)\n");
            for (uint32_t vi = 0; vi < nv; vi++) {
                MopVec3 lp = verts[vi].position;
                MopVec4 wp = mop_mat4_mul_vec4(world,
                    (MopVec4){ lp.x, lp.y, lp.z, 1.0f });
                printf("  v %.6f %.6f %.6f\n", wp.x, wp.y, wp.z);
            }
        }

        if (indices) {
            printf("  # OBJ faces (1-indexed)\n");
            for (uint32_t ti = 0; ti < ntri; ti++) {
                uint32_t i0 = indices[ti * 3 + 0] + 1;
                uint32_t i1 = indices[ti * 3 + 1] + 1;
                uint32_t i2 = indices[ti * 3 + 2] + 1;
                printf("  f %u %u %u\n", i0, i1, i2);
            }
        }
    }

    printf("\n=== Export #%d complete ===\n\n", export_num);
}

/* =========================================================================
 * Callbacks
 * ========================================================================= */

static void exporter_setup(MopViewport *vp, void *ctx)
{
    (void)ctx;

    /* Camera */
    mop_viewport_set_camera(vp,
        (MopVec3){ 3.0f, 3.0f, 5.0f },
        (MopVec3){ 0.0f, 0.0f, 0.0f },
        (MopVec3){ 0.0f, 1.0f, 0.0f },
        60.0f, 0.1f, 100.0f);

    /* Cube A — origin, red material, id=1 */
    MopMesh *cube_a = mop_viewport_add_mesh(vp, &(MopMeshDesc){
        .vertices     = CUBE_VERTICES,
        .vertex_count = CUBE_VERTEX_COUNT,
        .indices      = CUBE_INDICES,
        .index_count  = CUBE_INDEX_COUNT,
        .object_id    = 1,
    });
    mop_mesh_set_position(cube_a, (MopVec3){ 0.0f, 0.0f, 0.0f });
    mop_mesh_set_material(cube_a, &(MopMaterial){
        .base_color = { 0.9f, 0.15f, 0.15f, 1.0f },
        .metallic   = 0.0f,
        .roughness  = 0.6f,
    });

    /* Cube B — (2,0,0), blue material, half-scale, id=2 */
    MopMesh *cube_b = mop_viewport_add_mesh(vp, &(MopMeshDesc){
        .vertices     = CUBE_VERTICES,
        .vertex_count = CUBE_VERTEX_COUNT,
        .indices      = CUBE_INDICES,
        .index_count  = CUBE_INDEX_COUNT,
        .object_id    = 2,
    });
    mop_mesh_set_position(cube_b, (MopVec3){ 2.0f, 0.0f, 0.0f });
    mop_mesh_set_scale(cube_b, (MopVec3){ 0.5f, 0.5f, 0.5f });
    mop_mesh_set_material(cube_b, &(MopMaterial){
        .base_color = { 0.15f, 0.25f, 0.9f, 1.0f },
        .metallic   = 0.3f,
        .roughness  = 0.4f,
    });

    /* Floor plane — y=-1, id=3 */
    MopMesh *floor_mesh = mop_viewport_add_mesh(vp, &(MopMeshDesc){
        .vertices     = PLANE_VERTICES,
        .vertex_count = PLANE_VERTEX_COUNT,
        .indices      = PLANE_INDICES,
        .index_count  = PLANE_INDEX_COUNT,
        .object_id    = 3,
    });
    mop_mesh_set_position(floor_mesh, (MopVec3){ 0.0f, -1.0f, 0.0f });

    /* Directional light */
    mop_viewport_add_light(vp, &(MopLight){
        .type      = MOP_LIGHT_DIRECTIONAL,
        .direction = { -0.5f, -1.0f, -0.3f },
        .color     = { 1.0f, 0.95f, 0.85f, 1.0f },
        .intensity = 1.2f,
        .active    = true,
    });

    /* Point light */
    mop_viewport_add_light(vp, &(MopLight){
        .type      = MOP_LIGHT_POINT,
        .position  = { 1.0f, 2.0f, 1.5f },
        .color     = { 0.4f, 0.7f, 1.0f, 1.0f },
        .intensity = 3.0f,
        .range     = 10.0f,
        .active    = true,
    });

    /* Materials applied to cubes above */
    mop_viewport_set_ambient(vp, 0.2f);

    printf("[exporter] Scene ready: 3 meshes, 2 lights\n");
    printf("[exporter] E=export to console  S=save export.mop  W=wireframe  Q/Esc=quit\n");
}

static void exporter_update(MopViewport *vp, float dt, void *ctx)
{
    /* Static scene — nothing to update each frame. */
    (void)vp;
    (void)dt;
    (void)ctx;
}

/* =========================================================================
 * .mop scene format writer
 *
 * Human-readable text format capturing the full scene state:
 *   camera, ambient, lights, meshes (transform, material, geometry).
 * Matrices are written column-major (MOP's native layout).
 * ========================================================================= */

static void write_mat4(FILE *f, const char *indent, MopMat4 m)
{
    fprintf(f, "%stransform [\n", indent);
    for (int col = 0; col < 4; col++) {
        fprintf(f, "%s    %.6f %.6f %.6f %.6f\n", indent,
                m.d[col * 4 + 0], m.d[col * 4 + 1],
                m.d[col * 4 + 2], m.d[col * 4 + 3]);
    }
    fprintf(f, "%s]\n", indent);
}

static void save_mop(MopViewport *vp, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        printf("[exporter] ERROR: cannot open %s for writing\n", path);
        return;
    }

    fprintf(f, "# MOP Scene Format v1\n");
    fprintf(f, "# Exported by Master of Puppets\n");
    fprintf(f, "mop_version 1\n\n");

    /* ---- Camera ---- */
    MopCameraState cam = mop_viewport_get_camera_state(vp);
    fprintf(f, "camera {\n");
    fprintf(f, "    eye %.6f %.6f %.6f\n", cam.eye.x, cam.eye.y, cam.eye.z);
    fprintf(f, "    target %.6f %.6f %.6f\n", cam.target.x, cam.target.y, cam.target.z);
    fprintf(f, "    up %.6f %.6f %.6f\n", cam.up.x, cam.up.y, cam.up.z);
    fprintf(f, "    fov %.6f\n", cam.fov_radians);
    fprintf(f, "    near %.6f\n", cam.near_plane);
    fprintf(f, "    far %.6f\n", cam.far_plane);
    fprintf(f, "    aspect %.6f\n", cam.aspect_ratio);
    fprintf(f, "}\n\n");

    /* ---- Lights ---- */
    uint32_t num_lights = mop_viewport_light_count(vp);
    for (uint32_t i = 0; i < num_lights; i++) {
        const MopLight *l = mop_viewport_light_at(vp, i);
        if (!l) continue;
        fprintf(f, "light %s {\n", light_type_str(l->type));
        fprintf(f, "    position %.6f %.6f %.6f\n",
                l->position.x, l->position.y, l->position.z);
        fprintf(f, "    direction %.6f %.6f %.6f\n",
                l->direction.x, l->direction.y, l->direction.z);
        fprintf(f, "    color %.6f %.6f %.6f %.6f\n",
                l->color.r, l->color.g, l->color.b, l->color.a);
        fprintf(f, "    intensity %.6f\n", l->intensity);
        fprintf(f, "    range %.6f\n", l->range);
        fprintf(f, "    active %s\n", l->active ? "true" : "false");
        fprintf(f, "}\n\n");
    }

    /* ---- Meshes ---- */
    uint32_t num_meshes  = mop_viewport_mesh_count(vp);
    uint32_t total_verts = 0;

    for (uint32_t mi = 0; mi < num_meshes; mi++) {
        MopMesh *mesh = mop_viewport_mesh_at(vp, mi);
        if (!mesh) continue;

        uint32_t oid  = mop_mesh_get_object_id(mesh);
        uint32_t nv   = mop_mesh_get_vertex_count(mesh);
        uint32_t ntri = mop_mesh_get_triangle_count(mesh);

        fprintf(f, "mesh {\n");
        fprintf(f, "    object_id %u\n", oid);
        fprintf(f, "    blend %s\n", blend_mode_str(mop_mesh_get_blend_mode(mesh)));
        fprintf(f, "    opacity %.6f\n", mop_mesh_get_opacity(mesh));

        /* World transform (column-major) */
        MopMat4 world = mop_mesh_get_world_transform(mesh);
        write_mat4(f, "    ", world);

        /* Material */
        if (mop_mesh_has_material(mesh)) {
            MopMaterial mat = mop_mesh_get_material(mesh);
            fprintf(f, "    material {\n");
            fprintf(f, "        base_color %.6f %.6f %.6f %.6f\n",
                    mat.base_color.r, mat.base_color.g,
                    mat.base_color.b, mat.base_color.a);
            fprintf(f, "        metallic %.6f\n", mat.metallic);
            fprintf(f, "        roughness %.6f\n", mat.roughness);
            fprintf(f, "        emissive %.6f %.6f %.6f\n",
                    mat.emissive.x, mat.emissive.y, mat.emissive.z);
            fprintf(f, "    }\n");
        }

        /* Vertices: position.xyz normal.xyz color.rgba uv.st */
        const MopVertex  *verts   = mop_mesh_get_vertices(mesh, vp);
        const uint32_t   *indices = mop_mesh_get_indices(mesh, vp);

        if (verts) {
            fprintf(f, "    vertices %u {\n", nv);
            for (uint32_t vi = 0; vi < nv; vi++) {
                const MopVertex *v = &verts[vi];
                fprintf(f, "        %.6f %.6f %.6f  %.6f %.6f %.6f  "
                        "%.4f %.4f %.4f %.4f  %.6f %.6f\n",
                        v->position.x, v->position.y, v->position.z,
                        v->normal.x,   v->normal.y,   v->normal.z,
                        v->color.r,    v->color.g,    v->color.b, v->color.a,
                        v->u,          v->v);
            }
            fprintf(f, "    }\n");
        }

        if (indices) {
            fprintf(f, "    triangles %u {\n", ntri);
            for (uint32_t ti = 0; ti < ntri; ti++) {
                fprintf(f, "        %u %u %u\n",
                        indices[ti * 3 + 0],
                        indices[ti * 3 + 1],
                        indices[ti * 3 + 2]);
            }
            fprintf(f, "    }\n");
        }

        fprintf(f, "}\n\n");
        total_verts += nv;
    }

    fclose(f);
    printf("[exporter] Saved %s  (%u meshes, %u lights, %u total vertices)\n",
           path, num_meshes, num_lights, total_verts);
}

static bool exporter_on_key(MopViewport *vp, SDL_Keycode key, void *ctx)
{
    ExporterCtx *ec = (ExporterCtx *)ctx;

    if (key == SDLK_E) {
        ec->export_count++;
        do_export(vp, ec->export_count);
        return true;
    }

    if (key == SDLK_S) {
        save_mop(vp, "export.mop");
        return true;
    }

    return false;
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    ExporterCtx ctx = { .export_count = 0 };

    MopSDLApp app = {
        .title   = "MOP — Scene Exporter",
        .width   = 800,
        .height  = 600,
        .setup   = exporter_setup,
        .update  = exporter_update,
        .on_key  = exporter_on_key,
        .ctx     = &ctx,
    };

    return mop_sdl_run(&app);
}
