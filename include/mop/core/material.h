/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * material.h — PBR-inspired material descriptor
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_CORE_MATERIAL_H
#define MOP_CORE_MATERIAL_H

#include <mop/types.h>

/* Forward declaration — defined in scene.h */
typedef struct MopTexture MopTexture;
typedef struct MopMesh MopMesh;

typedef struct MopMaterial {
  MopColor base_color;
  float metallic;
  float roughness;
  MopVec3 emissive;
  MopTexture *albedo_map;
  MopTexture *normal_map;
  MopTexture *metallic_roughness_map; /* glTF: G=roughness, B=metallic */
  MopTexture *ao_map;                 /* R channel = ambient occlusion */
} MopMaterial;

void mop_mesh_set_material(MopMesh *mesh, const MopMaterial *material);
MopMaterial mop_material_default(void);

#endif /* MOP_CORE_MATERIAL_H */
