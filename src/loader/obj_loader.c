/*
 * Master of Puppets — OBJ File Loader
 * obj_loader.c — Wavefront .obj parser
 *
 * Supports:
 *   v  — vertex positions
 *   vt — texture coordinates
 *   vn — vertex normals
 *   f  — faces (triangles and quads, triangulated automatically)
 *
 * Does not support: materials, groups, smooth shading directives.
 * All vertices are assigned a default light-gray color.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <mop/loader.h>
#include <mop/log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* -------------------------------------------------------------------------
 * Dynamic array helpers
 * ------------------------------------------------------------------------- */

#define DA_INIT_CAP 1024

typedef struct { float *d; uint32_t n, cap; } FloatArray;
typedef struct { uint32_t *d; uint32_t n, cap; } UintArray;

static void fa_push(FloatArray *a, float v) {
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : DA_INIT_CAP;
        a->d = realloc(a->d, a->cap * sizeof(float));
    }
    a->d[a->n++] = v;
}

static void ua_push(UintArray *a, uint32_t v) {
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : DA_INIT_CAP;
        a->d = realloc(a->d, a->cap * sizeof(uint32_t));
    }
    a->d[a->n++] = v;
}

/* -------------------------------------------------------------------------
 * Face index parsing
 *
 * OBJ face indices can be:  v, v/vt, v/vt/vn, v//vn
 * We only care about v and vn.  Returns 0-based indices;
 * negative OBJ indices are relative to current array end.
 * ------------------------------------------------------------------------- */

typedef struct { int vi, ti, ni; } FaceIdx;

static FaceIdx parse_face_index(const char *s, int nv, int nt, int nn) {
    FaceIdx f = { 0, -1, -1 };
    char *end;

    f.vi = (int)strtol(s, &end, 10);
    /* Convert to 0-based */
    f.vi = (f.vi > 0) ? f.vi - 1 : nv + f.vi;

    if (*end == '/') {
        end++;  /* skip first slash */
        if (*end == '/') {
            /* v//vn */
            end++;
            int ni = (int)strtol(end, &end, 10);
            f.ni = (ni > 0) ? ni - 1 : nn + ni;
        } else {
            /* v/vt or v/vt/vn */
            int ti = (int)strtol(end, &end, 10);
            f.ti = (ti > 0) ? ti - 1 : nt + ti;
            if (*end == '/') {
                end++;
                int ni = (int)strtol(end, &end, 10);
                f.ni = (ni > 0) ? ni - 1 : nn + ni;
            }
        }
    }
    return f;
}

/* -------------------------------------------------------------------------
 * Compute face normal from 3 positions
 * ------------------------------------------------------------------------- */

static MopVec3 compute_normal(MopVec3 a, MopVec3 b, MopVec3 c) {
    MopVec3 e1 = { b.x - a.x, b.y - a.y, b.z - a.z };
    MopVec3 e2 = { c.x - a.x, c.y - a.y, c.z - a.z };
    MopVec3 n = {
        e1.y * e2.z - e1.z * e2.y,
        e1.z * e2.x - e1.x * e2.z,
        e1.x * e2.y - e1.y * e2.x
    };
    float len = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
    if (len > 1e-8f) { n.x /= len; n.y /= len; n.z /= len; }
    return n;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

bool mop_obj_load(const char *path, MopObjMesh *out) {
    memset(out, 0, sizeof(*out));

    FILE *fp = fopen(path, "r");
    if (!fp) {
        MOP_ERROR("failed to open OBJ file: %s", path);
        return false;
    }

    bool result = false;

    /* Raw position, texcoord, and normal arrays */
    FloatArray positions = {0};
    FloatArray texcoords = {0};    /* 2 floats per vt entry */
    FloatArray normals   = {0};

    /* Output vertex and index arrays */
    MopVertex *verts = NULL;
    uint32_t vert_count = 0, vert_cap = 0;
    UintArray indices = {0};
    MopVec3 *tangents = NULL;

    /* Default vertex color: light gray */
    MopColor default_color = { 0.7f, 0.7f, 0.7f, 1.0f };

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        /* Skip comments and empty lines */
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;

        if (line[0] == 'v' && line[1] == ' ') {
            /* Vertex position */
            float x, y, z;
            if (sscanf(line + 2, "%f %f %f", &x, &y, &z) == 3) {
                fa_push(&positions, x);
                fa_push(&positions, y);
                fa_push(&positions, z);
            }
        } else if (line[0] == 'v' && line[1] == 'n' && line[2] == ' ') {
            /* Vertex normal */
            float x, y, z;
            if (sscanf(line + 3, "%f %f %f", &x, &y, &z) == 3) {
                fa_push(&normals, x);
                fa_push(&normals, y);
                fa_push(&normals, z);
            }
        } else if (line[0] == 'v' && line[1] == 't' && line[2] == ' ') {
            /* Texture coordinate */
            float tu, tv;
            if (sscanf(line + 3, "%f %f", &tu, &tv) >= 2) {
                fa_push(&texcoords, tu);
                fa_push(&texcoords, tv);
            }
        } else if (line[0] == 'f' && line[1] == ' ') {
            /* Face — parse up to 4 indices (triangle or quad) */
            int nv = (int)(positions.n / 3);
            int nt = (int)(texcoords.n / 2);
            int nn = (int)(normals.n / 3);

            FaceIdx face[4];
            int face_count = 0;
            char *tok = strtok(line + 2, " \t\n\r");
            while (tok && face_count < 4) {
                face[face_count++] = parse_face_index(tok, nv, nt, nn);
                tok = strtok(NULL, " \t\n\r");
            }

            if (face_count < 3) continue;

            /* Emit vertices for this face */
            uint32_t base = vert_count;
            for (int i = 0; i < face_count; i++) {
                if (vert_count == vert_cap) {
                    vert_cap = vert_cap ? vert_cap * 2 : DA_INIT_CAP;
                    verts = realloc(verts, vert_cap * sizeof(MopVertex));
                }

                FaceIdx *fi = &face[i];
                MopVec3 pos = {0, 0, 0};
                if (fi->vi >= 0 && fi->vi < nv) {
                    pos.x = positions.d[fi->vi * 3 + 0];
                    pos.y = positions.d[fi->vi * 3 + 1];
                    pos.z = positions.d[fi->vi * 3 + 2];
                }

                MopVec3 norm = {0, 0, 0};
                if (fi->ni >= 0 && fi->ni < nn) {
                    norm.x = normals.d[fi->ni * 3 + 0];
                    norm.y = normals.d[fi->ni * 3 + 1];
                    norm.z = normals.d[fi->ni * 3 + 2];
                }

                float tex_u = 0.0f, tex_v = 0.0f;
                if (fi->ti >= 0 && fi->ti < nt) {
                    tex_u = texcoords.d[fi->ti * 2 + 0];
                    tex_v = texcoords.d[fi->ti * 2 + 1];
                }

                verts[vert_count++] = (MopVertex){
                    pos, norm, default_color, tex_u, tex_v
                };
            }

            /* If no normals were provided, compute face normal */
            if (face[0].ni < 0) {
                MopVec3 fn = compute_normal(
                    verts[base].position,
                    verts[base + 1].position,
                    verts[base + 2].position
                );
                for (int i = 0; i < face_count; i++)
                    verts[base + i].normal = fn;
            }

            /* Triangulate: first triangle */
            ua_push(&indices, base);
            ua_push(&indices, base + 1);
            ua_push(&indices, base + 2);

            /* Second triangle if quad */
            if (face_count == 4) {
                ua_push(&indices, base);
                ua_push(&indices, base + 2);
                ua_push(&indices, base + 3);
            }
        }
        /* Ignore: vt, g, s, mtllib, usemtl, o, etc. */
    }

    fclose(fp);
    fp = NULL;

    if (vert_count == 0 || indices.n == 0) {
        goto cleanup;
    }

    /* Compute AABB */
    MopVec3 bmin = verts[0].position, bmax = bmin;
    for (uint32_t i = 1; i < vert_count; i++) {
        MopVec3 p = verts[i].position;
        if (p.x < bmin.x) bmin.x = p.x;
        if (p.x > bmax.x) bmax.x = p.x;
        if (p.y < bmin.y) bmin.y = p.y;
        if (p.y > bmax.y) bmax.y = p.y;
        if (p.z < bmin.z) bmin.z = p.z;
        if (p.z > bmax.z) bmax.z = p.z;
    }

    /* Center at origin, scale to fit ~2 unit cube */
    MopVec3 center = {
        (bmin.x + bmax.x) * 0.5f,
        (bmin.y + bmax.y) * 0.5f,
        (bmin.z + bmax.z) * 0.5f
    };
    float extent = bmax.x - bmin.x;
    if (bmax.y - bmin.y > extent) extent = bmax.y - bmin.y;
    if (bmax.z - bmin.z > extent) extent = bmax.z - bmin.z;
    float norm_scale = (extent > 1e-6f) ? 2.0f / extent : 1.0f;

    for (uint32_t i = 0; i < vert_count; i++) {
        verts[i].position.x = (verts[i].position.x - center.x) * norm_scale;
        verts[i].position.y = (verts[i].position.y - center.y) * norm_scale;
        verts[i].position.z = (verts[i].position.z - center.z) * norm_scale;
    }

    /* Recompute AABB after normalization */
    bmin = verts[0].position; bmax = bmin;
    for (uint32_t i = 1; i < vert_count; i++) {
        MopVec3 p = verts[i].position;
        if (p.x < bmin.x) bmin.x = p.x;
        if (p.x > bmax.x) bmax.x = p.x;
        if (p.y < bmin.y) bmin.y = p.y;
        if (p.y > bmax.y) bmax.y = p.y;
        if (p.z < bmin.z) bmin.z = p.z;
        if (p.z > bmax.z) bmax.z = p.z;
    }

    /* Compute per-vertex tangents from UV derivatives (for normal mapping).
     * For each triangle, compute the tangent vector from the UV edge
     * derivatives, then accumulate to vertices and normalize. */
    tangents = calloc(vert_count, sizeof(MopVec3));
    if (tangents) {
        for (uint32_t i = 0; i + 2 < indices.n; i += 3) {
            uint32_t idx0 = indices.d[i + 0];
            uint32_t idx1 = indices.d[i + 1];
            uint32_t idx2 = indices.d[i + 2];

            MopVec3 p0 = verts[idx0].position;
            MopVec3 p1 = verts[idx1].position;
            MopVec3 p2 = verts[idx2].position;

            float u0 = verts[idx0].u, v0_t = verts[idx0].v;
            float u1 = verts[idx1].u, v1_t = verts[idx1].v;
            float u2 = verts[idx2].u, v2_t = verts[idx2].v;

            MopVec3 edge1 = { p1.x - p0.x, p1.y - p0.y, p1.z - p0.z };
            MopVec3 edge2 = { p2.x - p0.x, p2.y - p0.y, p2.z - p0.z };

            float du1 = u1 - u0, dv1 = v1_t - v0_t;
            float du2 = u2 - u0, dv2 = v2_t - v0_t;

            float det = du1 * dv2 - du2 * dv1;
            if (fabsf(det) < 1e-8f) continue;
            float inv_det = 1.0f / det;

            MopVec3 tan = {
                (edge1.x * dv2 - edge2.x * dv1) * inv_det,
                (edge1.y * dv2 - edge2.y * dv1) * inv_det,
                (edge1.z * dv2 - edge2.z * dv1) * inv_det
            };

            /* Accumulate tangent to all 3 vertices */
            tangents[idx0].x += tan.x; tangents[idx0].y += tan.y; tangents[idx0].z += tan.z;
            tangents[idx1].x += tan.x; tangents[idx1].y += tan.y; tangents[idx1].z += tan.z;
            tangents[idx2].x += tan.x; tangents[idx2].y += tan.y; tangents[idx2].z += tan.z;
        }

        /* Normalize accumulated tangents */
        for (uint32_t i = 0; i < vert_count; i++) {
            float len = sqrtf(tangents[i].x * tangents[i].x +
                              tangents[i].y * tangents[i].y +
                              tangents[i].z * tangents[i].z);
            if (len > 1e-8f) {
                tangents[i].x /= len;
                tangents[i].y /= len;
                tangents[i].z /= len;
            }
        }
    }

    out->vertices     = verts;
    out->vertex_count = vert_count;
    out->indices      = indices.d;
    out->index_count  = indices.n;
    out->bbox_min     = bmin;
    out->bbox_max     = bmax;
    out->tangents     = tangents;
    result = true;

    /* On success, ownership transfers to out — don't free verts/indices/tangents */
    verts    = NULL;
    indices.d = NULL;
    tangents = NULL;

cleanup:
    if (fp) fclose(fp);
    free(positions.d);
    free(texcoords.d);
    free(normals.d);
    free(verts);
    free(indices.d);
    free(tangents);
    return result;
}

void mop_obj_free(MopObjMesh *mesh) {
    if (!mesh) return;
    free(mesh->vertices);
    free(mesh->indices);
    free(mesh->tangents);
    memset(mesh, 0, sizeof(*mesh));
}
