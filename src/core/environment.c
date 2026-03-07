/*
 * Master of Puppets — Environment System
 * environment.c — HDR environment map loading, IBL precomputation,
 *                 and procedural sky generation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/viewport_internal.h"
#include "rhi/rhi.h"

#include <math.h>
#include <mop/util/log.h>
#include <stdlib.h>
#include <string.h>

/* stb_image for .hdr loading (stbi_loadf) */
#include "stb_image.h"

/* tinyexr for .exr loading (LoadEXR) */
#include "tinyexr.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* -------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

/* Bilinear sample from RGBA float image */
static void sample_hdr_bilinear(const float *data, int w, int h, float u,
                                float v, float out[4]) {
  /* Wrap u to [0,1) */
  u = u - floorf(u);
  v = v - floorf(v);

  float fx = u * (float)(w - 1);
  float fy = v * (float)(h - 1);
  int x0 = (int)fx;
  int y0 = (int)fy;
  int x1 = (x0 + 1) % w;
  int y1 = y0 + 1;
  if (y1 >= h)
    y1 = h - 1;

  float sx = fx - (float)x0;
  float sy = fy - (float)y0;

  const float *p00 = &data[((size_t)y0 * w + x0) * 4];
  const float *p10 = &data[((size_t)y0 * w + x1) * 4];
  const float *p01 = &data[((size_t)y1 * w + x0) * 4];
  const float *p11 = &data[((size_t)y1 * w + x1) * 4];

  for (int c = 0; c < 4; c++) {
    float top = p00[c] * (1.0f - sx) + p10[c] * sx;
    float bot = p01[c] * (1.0f - sx) + p11[c] * sx;
    out[c] = top * (1.0f - sy) + bot * sy;
  }
}

/* Convert direction to equirectangular UV */
static void dir_to_equirect(float dx, float dy, float dz, float rotation,
                            float *u, float *v) {
  float phi = atan2f(dz, dx) + rotation;
  float theta = asinf(dy < -1.0f ? -1.0f : (dy > 1.0f ? 1.0f : dy));
  *u = phi / (2.0f * (float)M_PI) + 0.5f;
  *v = theta / (float)M_PI + 0.5f;
}

/* -------------------------------------------------------------------------
 * Cleanup helper
 * ------------------------------------------------------------------------- */

static void env_cleanup(MopViewport *vp) {
  if (vp->env_texture) {
    vp->rhi->texture_destroy(vp->device, vp->env_texture);
    vp->env_texture = NULL;
  }
  if (vp->env_irradiance) {
    vp->rhi->texture_destroy(vp->device, vp->env_irradiance);
    vp->env_irradiance = NULL;
  }
  if (vp->env_prefiltered) {
    vp->rhi->texture_destroy(vp->device, vp->env_prefiltered);
    vp->env_prefiltered = NULL;
  }
  if (vp->env_brdf_lut) {
    vp->rhi->texture_destroy(vp->device, vp->env_brdf_lut);
    vp->env_brdf_lut = NULL;
  }
  free(vp->env_hdr_data);
  vp->env_hdr_data = NULL;
  free(vp->env_irradiance_data);
  vp->env_irradiance_data = NULL;
  free(vp->env_prefiltered_data);
  vp->env_prefiltered_data = NULL;
  free(vp->env_brdf_lut_data);
  vp->env_brdf_lut_data = NULL;
  vp->env_width = 0;
  vp->env_height = 0;
}

/* -------------------------------------------------------------------------
 * IBL: Precompute diffuse irradiance map
 *
 * Convolves the environment map with a cosine-weighted hemisphere integral
 * for each direction, producing a low-res irradiance map (64x32).
 * ------------------------------------------------------------------------- */

#define IRR_W 64
#define IRR_H 32
#define IRR_SAMPLES 128

static void precompute_irradiance(MopViewport *vp) {
  if (!vp->env_hdr_data)
    return;

  size_t size = (size_t)IRR_W * IRR_H * 4;
  float *irr = calloc(size, sizeof(float));
  if (!irr)
    return;

  for (int y = 0; y < IRR_H; y++) {
    for (int x = 0; x < IRR_W; x++) {
      /* Direction for this texel */
      float u = ((float)x + 0.5f) / (float)IRR_W;
      float v = ((float)y + 0.5f) / (float)IRR_H;
      float phi = (u - 0.5f) * 2.0f * (float)M_PI;
      float theta = (v - 0.5f) * (float)M_PI;
      float ct = cosf(theta);
      float nx = ct * cosf(phi);
      float ny = sinf(theta);
      float nz = ct * sinf(phi);
      float len = sqrtf(nx * nx + ny * ny + nz * nz);
      nx /= len;
      ny /= len;
      nz /= len;

      /* Build tangent frame */
      float upx = 0.0f, upy = 1.0f, upz = 0.0f;
      if (fabsf(ny) > 0.99f) {
        upx = 0.0f;
        upy = 0.0f;
        upz = 1.0f;
      }
      /* tangent = normalize(cross(up, n)) */
      float tx = upy * nz - upz * ny;
      float ty = upz * nx - upx * nz;
      float tz = upx * ny - upy * nx;
      float tlen = sqrtf(tx * tx + ty * ty + tz * tz);
      if (tlen > 1e-6f) {
        tx /= tlen;
        ty /= tlen;
        tz /= tlen;
      }
      /* bitangent = cross(n, tangent) */
      float bx = ny * tz - nz * ty;
      float by = nz * tx - nx * tz;
      float bz = nx * ty - ny * tx;

      float accum[3] = {0, 0, 0};
      float total_weight = 0.0f;

      /* Uniform hemisphere sampling with cosine weighting */
      for (int s = 0; s < IRR_SAMPLES; s++) {
        /* Low-discrepancy sequence (Hammersley) */
        float xi1 = (float)s / (float)IRR_SAMPLES;
        uint32_t bits = (uint32_t)s;
        bits = (bits << 16u) | (bits >> 16u);
        bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
        bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
        bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
        bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
        float xi2 = (float)bits * 2.3283064365386963e-10f;

        /* Cosine-weighted hemisphere: theta = acos(sqrt(xi1)), phi = 2*pi*xi2
         */
        float cos_theta_s = sqrtf(xi1);
        float sin_theta_s = sqrtf(1.0f - xi1);
        float phi_s = 2.0f * (float)M_PI * xi2;

        /* Local hemisphere direction */
        float lx = sin_theta_s * cosf(phi_s);
        float ly = cos_theta_s;
        float lz = sin_theta_s * sinf(phi_s);

        /* To world space via TBN */
        float wx = tx * lx + nx * ly + bx * lz;
        float wy = ty * lx + ny * ly + by * lz;
        float wz = tz * lx + nz * ly + bz * lz;

        float su, sv;
        dir_to_equirect(wx, wy, wz, 0.0f, &su, &sv);
        float sample[4];
        sample_hdr_bilinear(vp->env_hdr_data, vp->env_width, vp->env_height, su,
                            sv, sample);

        /* Cosine-weighted: PDF = cos(theta)/pi, so weight = 1.0 for
         * importance-sampled cosine distribution */
        accum[0] += sample[0];
        accum[1] += sample[1];
        accum[2] += sample[2];
        total_weight += 1.0f;
      }

      size_t idx = ((size_t)y * IRR_W + x) * 4;
      if (total_weight > 0.0f) {
        irr[idx + 0] = accum[0] / total_weight;
        irr[idx + 1] = accum[1] / total_weight;
        irr[idx + 2] = accum[2] / total_weight;
      }
      irr[idx + 3] = 1.0f;
    }
  }

  vp->env_irradiance_data = irr;
  vp->env_irradiance_w = IRR_W;
  vp->env_irradiance_h = IRR_H;

  if (vp->rhi->texture_create_hdr) {
    vp->env_irradiance =
        vp->rhi->texture_create_hdr(vp->device, IRR_W, IRR_H, irr);
  }
}

/* -------------------------------------------------------------------------
 * IBL: Precompute prefiltered specular map (GGX importance sampling)
 *
 * Generates mip levels where each level represents increasing roughness.
 * Level 0 = roughness 0 (mirror), level N = roughness 1.
 * All levels stored in a single flat buffer for CPU sampling.
 * ------------------------------------------------------------------------- */

#define PREFILT_BASE_W 128
#define PREFILT_BASE_H 64
#define PREFILT_LEVELS 5
#define PREFILT_SAMPLES 64

static void precompute_prefiltered(MopViewport *vp) {
  if (!vp->env_hdr_data)
    return;

  /* Allocate storage for all mip levels concatenated */
  size_t total_pixels = 0;
  for (int l = 0; l < PREFILT_LEVELS; l++) {
    int w = PREFILT_BASE_W >> l;
    int h = PREFILT_BASE_H >> l;
    if (w < 1)
      w = 1;
    if (h < 1)
      h = 1;
    total_pixels += (size_t)w * h;
  }

  float *buf = calloc(total_pixels * 4, sizeof(float));
  if (!buf)
    return;

  size_t offset = 0;
  for (int l = 0; l < PREFILT_LEVELS; l++) {
    int w = PREFILT_BASE_W >> l;
    int h = PREFILT_BASE_H >> l;
    if (w < 1)
      w = 1;
    if (h < 1)
      h = 1;
    float roughness = (float)l / (float)(PREFILT_LEVELS - 1);
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    if (alpha2 < 1e-7f)
      alpha2 = 1e-7f;

    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        float u = ((float)x + 0.5f) / (float)w;
        float v = ((float)y + 0.5f) / (float)h;
        float phi = (u - 0.5f) * 2.0f * (float)M_PI;
        float theta = (v - 0.5f) * (float)M_PI;
        float ct = cosf(theta);
        float nx = ct * cosf(phi);
        float ny = sinf(theta);
        float nz = ct * sinf(phi);
        float nlen = sqrtf(nx * nx + ny * ny + nz * nz);
        nx /= nlen;
        ny /= nlen;
        nz /= nlen;

        /* View = Normal (split-sum approximation) */
        float vx = nx, vy = ny, vz = nz;

        /* Build tangent frame */
        float upx = 0.0f, upy = 1.0f, upz = 0.0f;
        if (fabsf(ny) > 0.99f) {
          upx = 0.0f;
          upy = 0.0f;
          upz = 1.0f;
        }
        float tax = upy * nz - upz * ny;
        float tay = upz * nx - upx * nz;
        float taz = upx * ny - upy * nx;
        float tlen = sqrtf(tax * tax + tay * tay + taz * taz);
        if (tlen > 1e-6f) {
          tax /= tlen;
          tay /= tlen;
          taz /= tlen;
        }
        float bax = ny * taz - nz * tay;
        float bay = nz * tax - nx * taz;
        float baz = nx * tay - ny * tax;

        float accum[3] = {0, 0, 0};
        float total_weight = 0.0f;

        for (int s = 0; s < PREFILT_SAMPLES; s++) {
          float xi1 = (float)s / (float)PREFILT_SAMPLES;
          uint32_t bits = (uint32_t)s;
          bits = (bits << 16u) | (bits >> 16u);
          bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
          bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
          bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
          bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
          float xi2 = (float)bits * 2.3283064365386963e-10f;

          /* GGX importance sampling for half vector */
          float cos_theta_h =
              sqrtf((1.0f - xi1) / (1.0f + (alpha2 - 1.0f) * xi1));
          float sin_theta_h = sqrtf(1.0f - cos_theta_h * cos_theta_h);
          float phi_h = 2.0f * (float)M_PI * xi2;

          /* Half vector in local space → world space */
          float hx_l = sin_theta_h * cosf(phi_h);
          float hy_l = cos_theta_h;
          float hz_l = sin_theta_h * sinf(phi_h);
          float hx = tax * hx_l + nx * hy_l + bax * hz_l;
          float hy = tay * hx_l + ny * hy_l + bay * hz_l;
          float hz = taz * hx_l + nz * hy_l + baz * hz_l;

          /* Reflect view around half vector: L = 2 * dot(V,H) * H - V */
          float vdh = vx * hx + vy * hy + vz * hz;
          float lx = 2.0f * vdh * hx - vx;
          float ly = 2.0f * vdh * hy - vy;
          float lz = 2.0f * vdh * hz - vz;

          float ndl = nx * lx + ny * ly + nz * lz;
          if (ndl <= 0.0f)
            continue;

          float su, sv;
          dir_to_equirect(lx, ly, lz, 0.0f, &su, &sv);
          float sample[4];
          sample_hdr_bilinear(vp->env_hdr_data, vp->env_width, vp->env_height,
                              su, sv, sample);

          accum[0] += sample[0] * ndl;
          accum[1] += sample[1] * ndl;
          accum[2] += sample[2] * ndl;
          total_weight += ndl;
        }

        size_t idx = (offset + (size_t)y * w + x) * 4;
        if (total_weight > 0.0f) {
          buf[idx + 0] = accum[0] / total_weight;
          buf[idx + 1] = accum[1] / total_weight;
          buf[idx + 2] = accum[2] / total_weight;
        }
        buf[idx + 3] = 1.0f;
      }
    }
    offset += (size_t)w * h;
  }

  vp->env_prefiltered_data = buf;
  vp->env_prefiltered_w = PREFILT_BASE_W;
  vp->env_prefiltered_h = PREFILT_BASE_H;
  vp->env_prefiltered_levels = PREFILT_LEVELS;

  /* Create GPU texture from level 0 only (levels available via CPU sampling) */
  if (vp->rhi->texture_create_hdr) {
    vp->env_prefiltered = vp->rhi->texture_create_hdr(
        vp->device, PREFILT_BASE_W, PREFILT_BASE_H, buf);
  }
}

/* -------------------------------------------------------------------------
 * IBL: Precompute split-sum BRDF LUT
 *
 * 256x256 texture, x = NdotV, y = roughness → (scale, bias) for
 * F0 * scale + bias approximation.
 * ------------------------------------------------------------------------- */

#define BRDF_LUT_SIZE 256
#define BRDF_LUT_SAMPLES 64

static void precompute_brdf_lut(MopViewport *vp) {
  size_t px = (size_t)BRDF_LUT_SIZE * BRDF_LUT_SIZE;
  float *lut = calloc(px * 4, sizeof(float));
  if (!lut)
    return;

  for (int y = 0; y < BRDF_LUT_SIZE; y++) {
    float roughness = ((float)y + 0.5f) / (float)BRDF_LUT_SIZE;
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    if (alpha2 < 1e-7f)
      alpha2 = 1e-7f;
    float k = alpha * 0.5f;

    for (int x = 0; x < BRDF_LUT_SIZE; x++) {
      float ndotv = ((float)x + 0.5f) / (float)BRDF_LUT_SIZE;
      if (ndotv < 1e-4f)
        ndotv = 1e-4f;

      /* View vector in tangent space (N = (0,0,1)) */
      float sin_v = sqrtf(1.0f - ndotv * ndotv);
      float vx_t = sin_v, vy_t = 0.0f, vz_t = ndotv;

      float scale = 0.0f, bias = 0.0f;

      for (int s = 0; s < BRDF_LUT_SAMPLES; s++) {
        float xi1 = (float)s / (float)BRDF_LUT_SAMPLES;
        uint32_t bits = (uint32_t)s;
        bits = (bits << 16u) | (bits >> 16u);
        bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
        bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
        bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
        bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
        float xi2 = (float)bits * 2.3283064365386963e-10f;

        /* GGX importance sample half vector in tangent space */
        float cos_theta_h =
            sqrtf((1.0f - xi1) / (1.0f + (alpha2 - 1.0f) * xi1));
        float sin_theta_h = sqrtf(1.0f - cos_theta_h * cos_theta_h);
        float phi_h = 2.0f * (float)M_PI * xi2;

        float hx = sin_theta_h * cosf(phi_h);
        float hy = sin_theta_h * sinf(phi_h);
        float hz = cos_theta_h;

        /* L = reflect(-V, H) */
        float vdh = vx_t * hx + vy_t * hy + vz_t * hz;
        /* float lx = 2.0f * vdh * hx - vx_t; — unused (N·L = lz) */
        /* float ly = 2.0f * vdh * hy - vy_t; — unused (N·L = lz) */
        float lz = 2.0f * vdh * hz - vz_t;

        float ndl = lz; /* N = (0,0,1) in tangent space */
        if (ndl <= 0.0f)
          continue;

        float ndh = hz;
        float vdh_c = vdh > 0.0f ? vdh : 0.0f;

        /* Geometry term (Smith-Schlick) */
        float g1v_t = ndotv / (ndotv * (1.0f - k) + k);
        float g1l_t = ndl / (ndl * (1.0f - k) + k);
        float g = g1v_t * g1l_t;

        /* Visibility = G * VdotH / (NdotH * NdotV) */
        float vis = (g * vdh_c) / (ndh * ndotv + 1e-6f);

        /* Fresnel: (1 - VdotH)^5 */
        float fc = 1.0f - vdh_c;
        float fc2 = fc * fc;
        float fc5 = fc2 * fc2 * fc;

        scale += vis * (1.0f - fc5);
        bias += vis * fc5;
      }

      size_t idx = ((size_t)y * BRDF_LUT_SIZE + x) * 4;
      lut[idx + 0] = scale / (float)BRDF_LUT_SAMPLES;
      lut[idx + 1] = bias / (float)BRDF_LUT_SAMPLES;
      lut[idx + 2] = 0.0f;
      lut[idx + 3] = 1.0f;
    }
  }

  vp->env_brdf_lut_data = lut;

  if (vp->rhi->texture_create_hdr) {
    vp->env_brdf_lut = vp->rhi->texture_create_hdr(vp->device, BRDF_LUT_SIZE,
                                                   BRDF_LUT_SIZE, lut);
  }
}

/* -------------------------------------------------------------------------
 * Procedural sky: Preetham analytical sky model
 *
 * Generates an equirectangular HDR image from sun direction + turbidity.
 * Uses Perez's luminance distribution with Preetham coefficients.
 * ------------------------------------------------------------------------- */

#define SKY_W 512
#define SKY_H 256

/* Perez function: F(theta, gamma) = (1 + A*exp(B/cos(theta))) *
 *                                    (1 + C*exp(D*gamma) + E*cos^2(gamma)) */
static float perez(float theta, float gamma, float A, float B, float C, float D,
                   float E) {
  float ct = cosf(theta);
  if (ct < 0.001f)
    ct = 0.001f;
  float cg = cosf(gamma);
  return (1.0f + A * expf(B / ct)) * (1.0f + C * expf(D * gamma) + E * cg * cg);
}

static void generate_procedural_sky(MopViewport *vp) {
  float T = vp->sky_desc.turbidity;
  if (T < 2.0f)
    T = 2.0f;
  if (T > 10.0f)
    T = 10.0f;

  MopVec3 sun = vp->sky_desc.sun_direction;
  float slen = sqrtf(sun.x * sun.x + sun.y * sun.y + sun.z * sun.z);
  if (slen < 1e-6f)
    sun = (MopVec3){0.0f, 1.0f, 0.0f};
  else {
    sun.x /= slen;
    sun.y /= slen;
    sun.z /= slen;
  }

  /* Sun zenith angle */
  float theta_s = acosf(sun.y > 1.0f ? 1.0f : (sun.y < -1.0f ? -1.0f : sun.y));

  /* Preetham sky model coefficients for Y (luminance) */
  float Ay = 0.1787f * T - 1.4630f;
  float By = -0.3554f * T + 0.4275f;
  float Cy = -0.0227f * T + 5.3251f;
  float Dy = 0.1206f * T - 2.5771f;
  float Ey = -0.0670f * T + 0.3703f;

  /* Zenith luminance (simplified Preetham) */
  float chi = (4.0f / 9.0f - T / 120.0f) * ((float)M_PI - 2.0f * theta_s);
  float Yz = (4.0453f * T - 4.9710f) * tanf(chi) - 0.2155f * T + 2.4192f;
  if (Yz < 0.0f)
    Yz = 0.0f;

  /* F(0, theta_s) for normalization */
  float F0 = perez(0.0f, theta_s, Ay, By, Cy, Dy, Ey);
  if (F0 < 1e-6f)
    F0 = 1e-6f;

  size_t px = (size_t)SKY_W * SKY_H;
  float *sky = calloc(px * 4, sizeof(float));
  if (!sky)
    return;

  for (int y = 0; y < SKY_H; y++) {
    for (int x = 0; x < SKY_W; x++) {
      float u = ((float)x + 0.5f) / (float)SKY_W;
      float v = ((float)y + 0.5f) / (float)SKY_H;
      float phi = (u - 0.5f) * 2.0f * (float)M_PI;
      float theta = (v - 0.5f) * (float)M_PI;

      float ct = cosf(theta);
      float dx = ct * cosf(phi);
      float dy = sinf(theta);
      float dz = ct * sinf(phi);

      /* Below horizon: ground color */
      if (dy < 0.0f) {
        float ground = vp->sky_desc.ground_albedo * Yz * 0.1f;
        size_t idx = ((size_t)y * SKY_W + x) * 4;
        sky[idx + 0] = ground * 0.8f;
        sky[idx + 1] = ground * 0.85f;
        sky[idx + 2] = ground;
        sky[idx + 3] = 1.0f;
        continue;
      }

      /* Zenith angle of this pixel */
      float theta_p = acosf(dy);
      if (theta_p < 0.001f)
        theta_p = 0.001f;

      /* Angle between pixel direction and sun */
      float gamma = acosf(dx * sun.x + dy * sun.y + dz * sun.z);

      /* Perez luminance model */
      float Fp = perez(theta_p, gamma, Ay, By, Cy, Dy, Ey);
      float lum = Yz * (Fp / F0);
      if (lum < 0.0f)
        lum = 0.0f;

      /* Simple blue sky color ramp based on zenith angle */
      float sky_r = 0.3f + 0.1f * (1.0f - dy);
      float sky_g = 0.5f + 0.2f * (1.0f - dy);
      float sky_b = 0.9f + 0.1f * dy;

      /* Sun disk */
      float sun_intensity = 0.0f;
      if (gamma < 0.02f) {
        sun_intensity = 100.0f * (1.0f - gamma / 0.02f);
      } else if (gamma < 0.05f) {
        sun_intensity = 5.0f * (1.0f - gamma / 0.05f);
      }

      float scale = lum * 0.5f;
      size_t idx = ((size_t)y * SKY_W + x) * 4;
      sky[idx + 0] = sky_r * scale + sun_intensity;
      sky[idx + 1] = sky_g * scale + sun_intensity * 0.95f;
      sky[idx + 2] = sky_b * scale + sun_intensity * 0.8f;
      sky[idx + 3] = 1.0f;
    }
  }

  /* Store as environment data — shares same pipeline as HDRI */
  free(vp->env_hdr_data);
  vp->env_hdr_data = sky;
  vp->env_width = SKY_W;
  vp->env_height = SKY_H;

  if (vp->env_texture) {
    vp->rhi->texture_destroy(vp->device, vp->env_texture);
    vp->env_texture = NULL;
  }
  if (vp->rhi->texture_create_hdr) {
    vp->env_texture =
        vp->rhi->texture_create_hdr(vp->device, SKY_W, SKY_H, sky);
  }
}

/* -------------------------------------------------------------------------
 * EXR loading helper (via tinyexr)
 * ------------------------------------------------------------------------- */

static bool has_extension(const char *path, const char *ext) {
  size_t plen = strlen(path);
  size_t elen = strlen(ext);
  if (plen < elen)
    return false;
  const char *suffix = path + plen - elen;
  for (size_t i = 0; i < elen; i++) {
    char a = suffix[i];
    char b = ext[i];
    if (a >= 'A' && a <= 'Z')
      a += 32;
    if (b >= 'A' && b <= 'Z')
      b += 32;
    if (a != b)
      return false;
  }
  return true;
}

static float *load_exr(const char *path, int *w, int *h) {
  float *rgba = NULL;
  const char *err = NULL;
  int ret = LoadEXR(&rgba, w, h, path, &err);
  if (ret != TINYEXR_SUCCESS) {
    if (err) {
      MOP_ERROR("failed to load EXR: %s", err);
      FreeEXRErrorMessage(err);
    }
    return NULL;
  }
  return rgba; /* already RGBA float32 */
}

/* -------------------------------------------------------------------------
 * Public API: mop_viewport_set_environment
 * ------------------------------------------------------------------------- */

bool mop_viewport_set_environment(MopViewport *vp,
                                  const MopEnvironmentDesc *desc) {
  if (!vp || !desc)
    return false;

  /* Clean up previous environment */
  env_cleanup(vp);

  vp->env_type = desc->type;
  vp->env_rotation = desc->rotation;
  vp->env_intensity = desc->intensity > 0.0f ? desc->intensity : 1.0f;

  if (desc->type == MOP_ENV_NONE || desc->type == MOP_ENV_GRADIENT) {
    return true;
  }

  if (desc->type == MOP_ENV_PROCEDURAL_SKY) {
    /* Procedural sky — will be generated when sky_desc is set,
     * or use defaults if not yet configured */
    if (vp->sky_desc.turbidity < 2.0f) {
      vp->sky_desc = (MopProceduralSkyDesc){
          .sun_direction = {0.5f, 0.7f, 0.3f},
          .turbidity = 3.0f,
          .ground_albedo = 0.3f,
      };
    }
    generate_procedural_sky(vp);
    precompute_irradiance(vp);
    precompute_prefiltered(vp);
    precompute_brdf_lut(vp);
    return true;
  }

  /* MOP_ENV_HDRI */
  if (!desc->hdr_path) {
    MOP_ERROR("MOP_ENV_HDRI requires hdr_path");
    vp->env_type = MOP_ENV_GRADIENT;
    return false;
  }

  int w, h;
  float *rgba = NULL;
  bool is_exr = has_extension(desc->hdr_path, ".exr");

  if (is_exr) {
    /* EXR via tinyexr — returns RGBA float32 directly */
    rgba = load_exr(desc->hdr_path, &w, &h);
    if (!rgba) {
      MOP_ERROR("failed to load EXR image: %s", desc->hdr_path);
      vp->env_type = MOP_ENV_GRADIENT;
      return false;
    }
  } else {
    /* HDR (or other stbi-supported) via stb_image */
    int channels;
    float *rgb_data = stbi_loadf(desc->hdr_path, &w, &h, &channels, 0);
    if (!rgb_data) {
      MOP_ERROR("failed to load HDR image: %s", desc->hdr_path);
      vp->env_type = MOP_ENV_GRADIENT;
      return false;
    }

    /* Convert to RGBA float */
    size_t px = (size_t)w * h;
    rgba = malloc(px * 4 * sizeof(float));
    if (!rgba) {
      stbi_image_free(rgb_data);
      vp->env_type = MOP_ENV_GRADIENT;
      return false;
    }

    if (channels == 3) {
      for (size_t i = 0; i < px; i++) {
        rgba[i * 4 + 0] = rgb_data[i * 3 + 0];
        rgba[i * 4 + 1] = rgb_data[i * 3 + 1];
        rgba[i * 4 + 2] = rgb_data[i * 3 + 2];
        rgba[i * 4 + 3] = 1.0f;
      }
    } else if (channels == 4) {
      memcpy(rgba, rgb_data, px * 4 * sizeof(float));
    } else {
      /* Grayscale or other — replicate to RGBA */
      for (size_t i = 0; i < px; i++) {
        float val = rgb_data[i * channels];
        rgba[i * 4 + 0] = val;
        rgba[i * 4 + 1] = val;
        rgba[i * 4 + 2] = val;
        rgba[i * 4 + 3] = 1.0f;
      }
    }
    stbi_image_free(rgb_data);
  }

  vp->env_hdr_data = rgba;
  vp->env_width = w;
  vp->env_height = h;

  /* Create GPU texture */
  if (vp->rhi->texture_create_hdr) {
    vp->env_texture = vp->rhi->texture_create_hdr(vp->device, w, h, rgba);
    if (!vp->env_texture) {
      MOP_WARN("GPU HDR texture creation failed — CPU skybox still works");
    }
  }

  /* Auto-exposure: compute log-average luminance and set exposure so that
   * the average scene luminance maps to ~0.18 (middle gray) after ACES. */
  {
    size_t px = (size_t)w * h;
    double log_sum = 0.0;
    size_t count = 0;
    for (size_t i = 0; i < px; i++) {
      float r = rgba[i * 4 + 0];
      float g = rgba[i * 4 + 1];
      float b = rgba[i * 4 + 2];
      float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
      if (lum > 1e-6f) {
        log_sum += log(lum);
        count++;
      }
    }
    if (count > 0) {
      float avg_lum = (float)exp(log_sum / (double)count);
      float auto_exp = 0.18f / avg_lum;
      /* Clamp to reasonable range */
      if (auto_exp < 0.01f)
        auto_exp = 0.01f;
      if (auto_exp > 16.0f)
        auto_exp = 16.0f;
      vp->exposure = auto_exp;
      if (vp->rhi->set_exposure)
        vp->rhi->set_exposure(vp->device, auto_exp);
      MOP_INFO("auto-exposure: avg_lum=%.3f, exposure=%.3f", avg_lum, auto_exp);
    }
  }

  /* Precompute IBL maps */
  precompute_irradiance(vp);
  precompute_prefiltered(vp);
  precompute_brdf_lut(vp);

  MOP_INFO("loaded %s environment: %dx%d (%s)", is_exr ? "EXR" : "HDR", w, h,
           desc->hdr_path);
  return true;
}

/* -------------------------------------------------------------------------
 * Public API: rotation / intensity setters
 * ------------------------------------------------------------------------- */

void mop_viewport_set_environment_rotation(MopViewport *vp, float rotation) {
  if (vp)
    vp->env_rotation = rotation;
}

void mop_viewport_set_environment_intensity(MopViewport *vp, float intensity) {
  if (vp)
    vp->env_intensity = intensity > 0.0f ? intensity : 0.0f;
}

void mop_viewport_set_environment_background(MopViewport *vp, bool show) {
  if (vp)
    vp->show_env_background = show;
}

/* -------------------------------------------------------------------------
 * Public API: procedural sky parameters
 * ------------------------------------------------------------------------- */

void mop_viewport_set_procedural_sky(MopViewport *vp,
                                     const MopProceduralSkyDesc *desc) {
  if (!vp || !desc)
    return;
  vp->sky_desc = *desc;
  if (vp->env_type == MOP_ENV_PROCEDURAL_SKY) {
    /* Regenerate sky texture */
    generate_procedural_sky(vp);
    /* Re-precompute IBL */
    if (vp->env_irradiance) {
      vp->rhi->texture_destroy(vp->device, vp->env_irradiance);
      vp->env_irradiance = NULL;
    }
    free(vp->env_irradiance_data);
    vp->env_irradiance_data = NULL;
    precompute_irradiance(vp);

    if (vp->env_prefiltered) {
      vp->rhi->texture_destroy(vp->device, vp->env_prefiltered);
      vp->env_prefiltered = NULL;
    }
    free(vp->env_prefiltered_data);
    vp->env_prefiltered_data = NULL;
    precompute_prefiltered(vp);
  }
}

/* -------------------------------------------------------------------------
 * Sampling functions for CPU rasterizer integration
 * ------------------------------------------------------------------------- */

/* Sample the irradiance map at a world-space normal direction.
 * Returns diffuse irradiance RGB. Falls back to ambient if no env map. */
void mop_env_sample_irradiance(const MopViewport *vp, MopVec3 normal,
                               float out[3]) {
  out[0] = out[1] = out[2] = 0.0f;
  if (!vp->env_irradiance_data)
    return;

  float len =
      sqrtf(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
  if (len < 1e-6f)
    return;
  float nx = normal.x / len, ny = normal.y / len, nz = normal.z / len;

  float u, v;
  dir_to_equirect(nx, ny, nz, vp->env_rotation, &u, &v);

  float sample[4];
  sample_hdr_bilinear(vp->env_irradiance_data, vp->env_irradiance_w,
                      vp->env_irradiance_h, u, v, sample);
  out[0] = sample[0] * vp->env_intensity;
  out[1] = sample[1] * vp->env_intensity;
  out[2] = sample[2] * vp->env_intensity;
}

/* Sample the prefiltered specular map at a reflection direction + roughness.
 * Returns specular radiance RGB. */
void mop_env_sample_prefiltered(const MopViewport *vp, MopVec3 reflect_dir,
                                float roughness, float out[3]) {
  out[0] = out[1] = out[2] = 0.0f;
  if (!vp->env_prefiltered_data)
    return;

  float len =
      sqrtf(reflect_dir.x * reflect_dir.x + reflect_dir.y * reflect_dir.y +
            reflect_dir.z * reflect_dir.z);
  if (len < 1e-6f)
    return;
  float rx = reflect_dir.x / len;
  float ry = reflect_dir.y / len;
  float rz = reflect_dir.z / len;

  /* Select mip level from roughness */
  float level_f = roughness * (float)(vp->env_prefiltered_levels - 1);
  int level = (int)level_f;
  if (level >= vp->env_prefiltered_levels - 1)
    level = vp->env_prefiltered_levels - 1;

  /* Find offset for this level */
  size_t offset = 0;
  for (int l = 0; l < level; l++) {
    int lw = vp->env_prefiltered_w >> l;
    int lh = vp->env_prefiltered_h >> l;
    if (lw < 1)
      lw = 1;
    if (lh < 1)
      lh = 1;
    offset += (size_t)lw * lh;
  }

  int lw = vp->env_prefiltered_w >> level;
  int lh = vp->env_prefiltered_h >> level;
  if (lw < 1)
    lw = 1;
  if (lh < 1)
    lh = 1;

  float u, v;
  dir_to_equirect(rx, ry, rz, vp->env_rotation, &u, &v);

  float sample[4];
  sample_hdr_bilinear(vp->env_prefiltered_data + offset * 4, lw, lh, u, v,
                      sample);
  out[0] = sample[0] * vp->env_intensity;
  out[1] = sample[1] * vp->env_intensity;
  out[2] = sample[2] * vp->env_intensity;
}

/* Sample the BRDF LUT at (NdotV, roughness).
 * Returns (scale, bias) for split-sum: specular = prefiltered * (F0*scale +
 * bias) */
void mop_env_sample_brdf_lut(const MopViewport *vp, float ndotv,
                             float roughness, float *scale, float *bias) {
  *scale = 1.0f;
  *bias = 0.0f;
  if (!vp->env_brdf_lut_data)
    return;

  if (ndotv < 0.0f)
    ndotv = 0.0f;
  if (ndotv > 1.0f)
    ndotv = 1.0f;
  if (roughness < 0.0f)
    roughness = 0.0f;
  if (roughness > 1.0f)
    roughness = 1.0f;

  int x = (int)(ndotv * (float)(BRDF_LUT_SIZE - 1) + 0.5f);
  int y = (int)(roughness * (float)(BRDF_LUT_SIZE - 1) + 0.5f);
  if (x >= BRDF_LUT_SIZE)
    x = BRDF_LUT_SIZE - 1;
  if (y >= BRDF_LUT_SIZE)
    y = BRDF_LUT_SIZE - 1;

  size_t idx = ((size_t)y * BRDF_LUT_SIZE + x) * 4;
  *scale = vp->env_brdf_lut_data[idx + 0];
  *bias = vp->env_brdf_lut_data[idx + 1];
}
