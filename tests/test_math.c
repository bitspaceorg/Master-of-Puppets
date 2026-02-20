/*
 * Master of Puppets — Math Tests
 * test_math.c — Vec3, Mat4, column-major layout, TRS compose
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <mop/types.h>
#include "test_harness.h"

/* -------------------------------------------------------------------------
 * Vec3 tests
 * ------------------------------------------------------------------------- */

static void test_vec3_add(void) {
    TEST_BEGIN("vec3_add");
    MopVec3 r = mop_vec3_add((MopVec3){1,2,3}, (MopVec3){4,5,6});
    TEST_ASSERT_VEC3_EQ(r, 5.0f, 7.0f, 9.0f);
    TEST_END();
}

static void test_vec3_sub(void) {
    TEST_BEGIN("vec3_sub");
    MopVec3 r = mop_vec3_sub((MopVec3){5,7,9}, (MopVec3){1,2,3});
    TEST_ASSERT_VEC3_EQ(r, 4.0f, 5.0f, 6.0f);
    TEST_END();
}

static void test_vec3_scale(void) {
    TEST_BEGIN("vec3_scale");
    MopVec3 r = mop_vec3_scale((MopVec3){1,2,3}, 2.0f);
    TEST_ASSERT_VEC3_EQ(r, 2.0f, 4.0f, 6.0f);
    TEST_END();
}

static void test_vec3_dot(void) {
    TEST_BEGIN("vec3_dot");
    float d = mop_vec3_dot((MopVec3){1,0,0}, (MopVec3){0,1,0});
    TEST_ASSERT_FLOAT_EQ(d, 0.0f);
    d = mop_vec3_dot((MopVec3){1,2,3}, (MopVec3){4,5,6});
    TEST_ASSERT_FLOAT_EQ(d, 32.0f);
    TEST_END();
}

static void test_vec3_cross(void) {
    TEST_BEGIN("vec3_cross");
    MopVec3 r = mop_vec3_cross((MopVec3){1,0,0}, (MopVec3){0,1,0});
    TEST_ASSERT_VEC3_EQ(r, 0.0f, 0.0f, 1.0f);
    TEST_END();
}

static void test_vec3_length(void) {
    TEST_BEGIN("vec3_length");
    TEST_ASSERT_FLOAT_EQ(mop_vec3_length((MopVec3){3,4,0}), 5.0f);
    TEST_ASSERT_FLOAT_EQ(mop_vec3_length((MopVec3){0,0,0}), 0.0f);
    TEST_END();
}

static void test_vec3_normalize(void) {
    TEST_BEGIN("vec3_normalize");
    MopVec3 n = mop_vec3_normalize((MopVec3){3,0,0});
    TEST_ASSERT_VEC3_EQ(n, 1.0f, 0.0f, 0.0f);
    /* Zero vector returns zero */
    MopVec3 z = mop_vec3_normalize((MopVec3){0,0,0});
    TEST_ASSERT_VEC3_EQ(z, 0.0f, 0.0f, 0.0f);
    TEST_END();
}

/* -------------------------------------------------------------------------
 * Mat4 tests
 * ------------------------------------------------------------------------- */

#define M(mat, r, c) ((mat).d[(c) * 4 + (r)])

static void test_identity(void) {
    TEST_BEGIN("mat4_identity");
    MopMat4 I = mop_mat4_identity();
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            TEST_ASSERT_FLOAT_EQ(M(I, r, c), (r == c) ? 1.0f : 0.0f);
    TEST_END();
}

static void test_column_major_layout(void) {
    TEST_BEGIN("column_major_layout");
    MopMat4 t = mop_mat4_translate((MopVec3){10, 20, 30});
    /* Translation lives in column 3 (indices 12,13,14) */
    TEST_ASSERT_FLOAT_EQ(t.d[12], 10.0f);
    TEST_ASSERT_FLOAT_EQ(t.d[13], 20.0f);
    TEST_ASSERT_FLOAT_EQ(t.d[14], 30.0f);
    /* M(r,c) accessor should match */
    TEST_ASSERT_FLOAT_EQ(M(t, 0, 3), 10.0f);
    TEST_ASSERT_FLOAT_EQ(M(t, 1, 3), 20.0f);
    TEST_ASSERT_FLOAT_EQ(M(t, 2, 3), 30.0f);
    TEST_END();
}

static void test_perspective(void) {
    TEST_BEGIN("mat4_perspective");
    float fov = 60.0f * (3.14159265f / 180.0f);
    MopMat4 p = mop_mat4_perspective(fov, 1.0f, 0.1f, 100.0f);
    /* d[11] should be -1 (w = -z) */
    TEST_ASSERT_FLOAT_EQ(p.d[11], -1.0f);
    /* M(0,0) == 1/(aspect*tan(fov/2)), aspect=1 */
    float expected = 1.0f / tanf(fov * 0.5f);
    TEST_ASSERT_FLOAT_EQ(M(p, 0, 0), expected);
    TEST_ASSERT_FLOAT_EQ(M(p, 1, 1), expected);
    TEST_END();
}

static void test_look_at(void) {
    TEST_BEGIN("mat4_look_at");
    MopVec3 eye = {0, 0, 5};
    MopVec3 center = {0, 0, 0};
    MopVec3 up = {0, 1, 0};
    MopMat4 v = mop_mat4_look_at(eye, center, up);
    /* Looking down -Z: forward = (0,0,-1) after normalize */
    /* The view matrix should produce identity-like rotation for this case */
    TEST_ASSERT_FLOAT_EQ(M(v, 0, 0), 1.0f);
    TEST_ASSERT_FLOAT_EQ(M(v, 1, 1), 1.0f);
    /* Translation z component should reflect the eye distance */
    TEST_ASSERT_FLOAT_EQ(M(v, 2, 3), -5.0f);
    TEST_END();
}

static void test_rotate_y_90(void) {
    TEST_BEGIN("mat4_rotate_y_90");
    float half_pi = 3.14159265f * 0.5f;
    MopMat4 r = mop_mat4_rotate_y(half_pi);
    /* Rotating (1,0,0) by 90 around Y should give ~(0,0,-1) */
    MopVec4 v = mop_mat4_mul_vec4(r, (MopVec4){1, 0, 0, 0});
    TEST_ASSERT_FLOAT_EQ(v.x, 0.0f);
    TEST_ASSERT_FLOAT_EQ(v.z, -1.0f);
    TEST_END();
}

static void test_scale(void) {
    TEST_BEGIN("mat4_scale");
    MopMat4 s = mop_mat4_scale((MopVec3){2, 3, 4});
    MopVec4 v = mop_mat4_mul_vec4(s, (MopVec4){1, 1, 1, 1});
    TEST_ASSERT_FLOAT_EQ(v.x, 2.0f);
    TEST_ASSERT_FLOAT_EQ(v.y, 3.0f);
    TEST_ASSERT_FLOAT_EQ(v.z, 4.0f);
    TEST_ASSERT_FLOAT_EQ(v.w, 1.0f);
    TEST_END();
}

static void test_multiply_identity(void) {
    TEST_BEGIN("mat4_multiply_identity");
    MopMat4 a = mop_mat4_translate((MopVec3){1, 2, 3});
    MopMat4 I = mop_mat4_identity();
    MopMat4 r = mop_mat4_multiply(a, I);
    for (int i = 0; i < 16; i++)
        TEST_ASSERT_FLOAT_EQ(r.d[i], a.d[i]);
    TEST_END();
}

static void test_compose_trs_identity(void) {
    TEST_BEGIN("compose_trs_identity");
    MopMat4 m = mop_mat4_compose_trs(
        (MopVec3){0,0,0}, (MopVec3){0,0,0}, (MopVec3){1,1,1});
    MopMat4 I = mop_mat4_identity();
    for (int i = 0; i < 16; i++)
        TEST_ASSERT_FLOAT_EQ(m.d[i], I.d[i]);
    TEST_END();
}

static void test_compose_trs_translate(void) {
    TEST_BEGIN("compose_trs_translate_only");
    MopMat4 m = mop_mat4_compose_trs(
        (MopVec3){5,10,15}, (MopVec3){0,0,0}, (MopVec3){1,1,1});
    MopVec4 v = mop_mat4_mul_vec4(m, (MopVec4){0,0,0,1});
    TEST_ASSERT_FLOAT_EQ(v.x, 5.0f);
    TEST_ASSERT_FLOAT_EQ(v.y, 10.0f);
    TEST_ASSERT_FLOAT_EQ(v.z, 15.0f);
    TEST_END();
}

static void test_mul_vec4(void) {
    TEST_BEGIN("mat4_mul_vec4");
    MopMat4 t = mop_mat4_translate((MopVec3){1, 2, 3});
    MopVec4 v = mop_mat4_mul_vec4(t, (MopVec4){0, 0, 0, 1});
    TEST_ASSERT_FLOAT_EQ(v.x, 1.0f);
    TEST_ASSERT_FLOAT_EQ(v.y, 2.0f);
    TEST_ASSERT_FLOAT_EQ(v.z, 3.0f);
    TEST_ASSERT_FLOAT_EQ(v.w, 1.0f);
    /* Direction vectors (w=0) should not be translated */
    MopVec4 d = mop_mat4_mul_vec4(t, (MopVec4){1, 0, 0, 0});
    TEST_ASSERT_FLOAT_EQ(d.x, 1.0f);
    TEST_ASSERT_FLOAT_EQ(d.y, 0.0f);
    TEST_END();
}

#undef M

/* -------------------------------------------------------------------------
 * Runner
 * ------------------------------------------------------------------------- */

int main(void) {
    TEST_SUITE_BEGIN("math");

    TEST_RUN(test_vec3_add);
    TEST_RUN(test_vec3_sub);
    TEST_RUN(test_vec3_scale);
    TEST_RUN(test_vec3_dot);
    TEST_RUN(test_vec3_cross);
    TEST_RUN(test_vec3_length);
    TEST_RUN(test_vec3_normalize);
    TEST_RUN(test_identity);
    TEST_RUN(test_column_major_layout);
    TEST_RUN(test_perspective);
    TEST_RUN(test_look_at);
    TEST_RUN(test_rotate_y_90);
    TEST_RUN(test_scale);
    TEST_RUN(test_multiply_identity);
    TEST_RUN(test_compose_trs_identity);
    TEST_RUN(test_compose_trs_translate);
    TEST_RUN(test_mul_vec4);

    TEST_REPORT();
    TEST_EXIT();
}
