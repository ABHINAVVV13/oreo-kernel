// SPDX-License-Identifier: LGPL-2.1-or-later

/* test_capi_consumer.c — Pure-C smoke test for the public oreo-kernel
 * API. Builds as C99 (no C++ allowed) so a regression that leaks a
 * C++ keyword (`class`, `template`, `nullptr`...) into oreo_kernel.h
 * produces an immediate compile failure on the kernel's own CI
 * before any downstream consumer hits it.
 *
 * The test does the smallest possible end-to-end exercise of the
 * ctx-aware surface: create a context, build a box, query its AABB,
 * read a face's shape ID, free everything. If any of those signals
 * an error, the binary returns a non-zero exit code and ctest
 * reports the test as failed.
 */

#include "oreo_kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(const char* what) {
    fprintf(stderr, "[capi-consumer] FAIL: %s\n", what);
    return 1;
}

int main(void) {
    /* Library identity smoke test — proves oreo_kernel_version() is
     * exported, callable from pure C, and returns a non-empty string
     * containing a SemVer-shaped version. */
    const char* ver = oreo_kernel_version();
    if (!ver || ver[0] == '\0') return fail("oreo_kernel_version returned empty");
    /* Must contain at least one '.' (SemVer X.Y.Z). */
    if (!strchr(ver, '.')) return fail("oreo_kernel_version not SemVer");
    printf("oreo-kernel version: %s\n", ver);

    OreoContext ctx = oreo_context_create_with_doc(0xC0FFEE0000000123ULL, NULL);
    if (!ctx) return fail("oreo_context_create_with_doc returned NULL");

    OreoSolid box = oreo_ctx_make_box(ctx, 10.0, 20.0, 30.0);
    if (!box) {
        oreo_context_free(ctx);
        return fail("oreo_ctx_make_box returned NULL");
    }

    /* Geometry sanity. */
    OreoBBox bb = oreo_ctx_aabb(ctx, box);
    if ((bb.xmax - bb.xmin) < 9.999 || (bb.xmax - bb.xmin) > 10.001) {
        oreo_free_solid(box);
        oreo_context_free(ctx);
        return fail("aabb dx mismatch");
    }

    /* Naming + identity sanity. */
    int faceCount = oreo_ctx_face_count(ctx, box);
    if (faceCount != 6) {
        oreo_free_solid(box);
        oreo_context_free(ctx);
        return fail("face count != 6");
    }

    OreoShapeId sid;
    sid.document_id = 0;
    sid.counter = 0;
    int rc = oreo_face_shape_id(box, 1, &sid);
    if (rc != OREO_OK) {
        oreo_free_solid(box);
        oreo_context_free(ctx);
        return fail("oreo_face_shape_id returned non-OK");
    }
    if (sid.document_id != 0xC0FFEE0000000123ULL) {
        oreo_free_solid(box);
        oreo_context_free(ctx);
        return fail("face docId does not match context");
    }

    /* Size-probe protocol: ask for the name length first. */
    size_t needed = 0;
    rc = oreo_ctx_face_name(ctx, box, 1, NULL, 0, &needed);
    if (rc != OREO_OK || needed == 0) {
        oreo_free_solid(box);
        oreo_context_free(ctx);
        return fail("oreo_ctx_face_name size-probe failed");
    }

    /* Now read the name. */
    char* buf = (char*)malloc(needed + 1);
    if (!buf) {
        oreo_free_solid(box);
        oreo_context_free(ctx);
        return fail("malloc failed");
    }
    rc = oreo_ctx_face_name(ctx, box, 1, buf, needed + 1, &needed);
    if (rc != OREO_OK) {
        free(buf);
        oreo_free_solid(box);
        oreo_context_free(ctx);
        return fail("oreo_ctx_face_name returned non-OK");
    }
    if (strlen(buf) == 0) {
        free(buf);
        oreo_free_solid(box);
        oreo_context_free(ctx);
        return fail("face name is empty");
    }

    free(buf);
    oreo_free_solid(box);
    oreo_context_free(ctx);
    return 0;
}
