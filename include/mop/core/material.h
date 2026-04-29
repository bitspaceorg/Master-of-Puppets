/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * material.h — PBR-inspired material descriptor
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_CORE_MATERIAL_H
#define MOP_CORE_MATERIAL_H

#include <mop/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration — defined in scene.h */
typedef struct MopTexture MopTexture;
typedef struct MopMesh MopMesh;

typedef struct MopMaterial {
  MopColor base_color;    /* sRGB base color (linear in shader) */
  float metallic;         /* 0.0 = dielectric, 1.0 = metal */
  float roughness;        /* 0.0 = mirror, 1.0 = diffuse */
  MopVec3 emissive;       /* HDR emission color (linear, additive) */
  MopTexture *albedo_map; /* sRGB base color texture (optional) */
  MopTexture *normal_map; /* tangent-space normal map (optional) */
  MopTexture *metallic_roughness_map; /* glTF: G=roughness, B=metallic */
  MopTexture *ao_map;                 /* R channel = ambient occlusion */
} MopMaterial;

void mop_mesh_set_material(MopMesh *mesh, const MopMaterial *material);
MopMaterial mop_material_default(void);

#ifdef __cplusplus
}
#endif

#endif /* MOP_CORE_MATERIAL_H */
