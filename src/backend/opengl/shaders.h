/*
 * Master of Puppets — OpenGL Backend
 * shaders.h — GLSL 330 core shader source strings
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_GL_SHADERS_H
#define MOP_GL_SHADERS_H

#if defined(MOP_HAS_OPENGL)

/* -------------------------------------------------------------------------
 * Vertex shader
 *
 * Transforms vertices by MVP, passes normals/colors/UVs to fragment stage.
 * Normals are transformed by the model matrix (upper 3x3).
 * ------------------------------------------------------------------------- */

static const char *MOP_GL_VERTEX_SHADER =
    "#version 330 core\n"
    "\n"
    "layout(location = 0) in vec3 a_position;\n"
    "layout(location = 1) in vec3 a_normal;\n"
    "layout(location = 2) in vec4 a_color;\n"
    "layout(location = 3) in vec2 a_texcoord;\n"
    "\n"
    "uniform mat4 u_mvp;\n"
    "uniform mat4 u_model;\n"
    "\n"
    "out vec3 v_normal;\n"
    "out vec4 v_color;\n"
    "out vec2 v_texcoord;\n"
    "\n"
    "void main() {\n"
    "    gl_Position = u_mvp * vec4(a_position, 1.0);\n"
    "    v_normal    = mat3(u_model) * a_normal;\n"
    "    v_color     = a_color;\n"
    "    v_texcoord  = a_texcoord;\n"
    "}\n";

/* -------------------------------------------------------------------------
 * Fragment shader
 *
 * Lambert diffuse lighting with optional texture sampling.
 * Supports blend modes via uniform (0=opaque, 1=alpha, 2=additive, 3=multiply).
 * Outputs to two render targets: color (location 0) and picking ID (location 1).
 * ------------------------------------------------------------------------- */

static const char *MOP_GL_FRAGMENT_SHADER =
    "#version 330 core\n"
    "\n"
    "in vec3 v_normal;\n"
    "in vec4 v_color;\n"
    "in vec2 v_texcoord;\n"
    "\n"
    "uniform vec3  u_light_dir;\n"
    "uniform float u_ambient;\n"
    "uniform float u_opacity;\n"
    "uniform int   u_blend_mode;\n"
    "uniform uint  u_object_id;\n"
    "uniform bool  u_has_texture;\n"
    "uniform sampler2D u_texture;\n"
    "\n"
    "/* Multi-light support (matches Vulkan fragment shader logic) */\n"
    "uniform int u_num_lights;\n"
    "uniform vec4 u_light_position[4];\n"   /* xyz + type */
    "uniform vec4 u_light_direction[4];\n"  /* xyz + pad */
    "uniform vec4 u_light_color[4];\n"      /* rgb + intensity */
    "uniform vec4 u_light_params[4];\n"     /* range, inner_cos, outer_cos, active */
    "\n"
    "layout(location = 0) out vec4 frag_color;\n"
    "layout(location = 1) out uint frag_object_id;\n"
    "\n"
    "void main() {\n"
    "    vec3 n = normalize(v_normal);\n"
    "    float lighting = u_ambient;\n"
    "\n"
    "    if (u_num_lights > 0) {\n"
    "        for (int i = 0; i < u_num_lights; i++) {\n"
    "            if (u_light_params[i].w < 0.5) continue;\n"
    "\n"
    "            int light_type = int(u_light_position[i].w + 0.5);\n"
    "            float ndotl = 0.0;\n"
    "            float attenuation = 1.0;\n"
    "            float spot_factor = 1.0;\n"
    "            float intensity = u_light_color[i].w;\n"
    "\n"
    "            if (light_type == 0) {\n"
    "                vec3 dir = normalize(u_light_direction[i].xyz);\n"
    "                ndotl = max(dot(n, dir), 0.0);\n"
    "            } else if (light_type == 1) {\n"
    "                vec3 to_light = u_light_position[i].xyz;\n"
    "                float dist = length(to_light);\n"
    "                vec3 dir = to_light / max(dist, 0.000001);\n"
    "                ndotl = max(dot(n, dir), 0.0);\n"
    "                float range = u_light_params[i].x;\n"
    "                if (range > 0.0) {\n"
    "                    float r = dist / range;\n"
    "                    attenuation = max(1.0 - r, 0.0);\n"
    "                    attenuation *= attenuation;\n"
    "                } else {\n"
    "                    attenuation = 1.0 / (1.0 + dist * dist);\n"
    "                }\n"
    "            } else {\n"
    "                vec3 to_light = u_light_position[i].xyz;\n"
    "                float dist = length(to_light);\n"
    "                vec3 dir = to_light / max(dist, 0.000001);\n"
    "                ndotl = max(dot(n, dir), 0.0);\n"
    "                vec3 spot_dir = normalize(u_light_direction[i].xyz);\n"
    "                float cos_angle = -dot(dir, spot_dir);\n"
    "                float outer_cos = u_light_params[i].z;\n"
    "                float inner_cos = u_light_params[i].y;\n"
    "                if (cos_angle < outer_cos) {\n"
    "                    spot_factor = 0.0;\n"
    "                } else if (cos_angle < inner_cos) {\n"
    "                    float range_val = inner_cos - outer_cos;\n"
    "                    if (range_val > 0.000001) {\n"
    "                        spot_factor = (cos_angle - outer_cos) / range_val;\n"
    "                    }\n"
    "                }\n"
    "                float range = u_light_params[i].x;\n"
    "                if (range > 0.0) {\n"
    "                    float r = dist / range;\n"
    "                    attenuation = max(1.0 - r, 0.0);\n"
    "                    attenuation *= attenuation;\n"
    "                } else {\n"
    "                    attenuation = 1.0 / (1.0 + dist * dist);\n"
    "                }\n"
    "            }\n"
    "            lighting += ndotl * intensity * attenuation * spot_factor;\n"
    "        }\n"
    "        lighting = clamp(lighting, 0.0, 1.0);\n"
    "    } else {\n"
    "        vec3 l = normalize(u_light_dir);\n"
    "        float ndotl = max(dot(n, l), 0.0);\n"
    "        lighting = clamp(u_ambient + (1.0 - u_ambient) * ndotl, 0.0, 1.0);\n"
    "    }\n"
    "\n"
    "    vec4 base = v_color;\n"
    "    if (u_has_texture) {\n"
    "        base *= texture(u_texture, v_texcoord);\n"
    "    }\n"
    "\n"
    "    vec3 lit = base.rgb * lighting;\n"
    "    float alpha = base.a * u_opacity;\n"
    "\n"
    "    frag_color = vec4(lit, alpha);\n"
    "    frag_object_id = u_object_id;\n"
    "}\n";

/* -------------------------------------------------------------------------
 * Wireframe vertex shader (same as above)
 * Wireframe fragment shader (flat color, no lighting)
 * ------------------------------------------------------------------------- */

static const char *MOP_GL_WIREFRAME_FRAGMENT_SHADER =
    "#version 330 core\n"
    "\n"
    "in vec3 v_normal;\n"
    "in vec4 v_color;\n"
    "in vec2 v_texcoord;\n"
    "\n"
    "uniform uint u_object_id;\n"
    "\n"
    "layout(location = 0) out vec4 frag_color;\n"
    "layout(location = 1) out uint frag_object_id;\n"
    "\n"
    "void main() {\n"
    "    frag_color = v_color;\n"
    "    frag_object_id = u_object_id;\n"
    "}\n";

#endif /* MOP_HAS_OPENGL */
#endif /* MOP_GL_SHADERS_H */
