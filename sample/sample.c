/* sample.c — Minimum viable consumer: build a box, query its bbox,
 * serialize/deserialize round-trip, free everything. Mirrors the
 * smoke test at tests/test_capi_consumer/test_capi_consumer.c, but
 * lives at the documented install-package boundary so a regression
 * in OreoKernelConfig.cmake.in / install() surfaces here. */

#include "oreo_kernel.h"

#include <stdio.h>
#include <stdlib.h>

int main(void) {
    OreoContext ctx = oreo_context_create();
    if (!ctx) { fprintf(stderr, "no ctx\n"); return 1; }

    OreoSolid box = oreo_ctx_make_box(ctx, 10.0, 20.0, 30.0);
    if (!box) {
        fprintf(stderr, "make_box failed: %s\n",
                oreo_context_last_error_message(ctx));
        oreo_context_free(ctx);
        return 1;
    }

    OreoBBox bb = oreo_ctx_aabb(ctx, box);
    printf("box bbox: %.2f x %.2f x %.2f\n",
           bb.xmax - bb.xmin, bb.ymax - bb.ymin, bb.zmax - bb.zmin);

    /* Serialize round-trip via the size-probe protocol. */
    size_t needed = 0;
    int rc = oreo_ctx_serialize(ctx, box, NULL, 0, &needed);
    if (rc != OREO_OK || needed == 0) {
        fprintf(stderr, "serialize size-probe failed (rc=%d)\n", rc);
        oreo_free_solid(box); oreo_context_free(ctx);
        return 1;
    }
    uint8_t* buf = (uint8_t*)malloc(needed);
    rc = oreo_ctx_serialize(ctx, box, buf, needed, &needed);
    if (rc != OREO_OK) {
        fprintf(stderr, "serialize emit failed (rc=%d)\n", rc);
        free(buf); oreo_free_solid(box); oreo_context_free(ctx);
        return 1;
    }
    OreoSolid clone = oreo_ctx_deserialize(ctx, buf, needed);
    if (!clone) {
        fprintf(stderr, "deserialize failed\n");
        free(buf); oreo_free_solid(box); oreo_context_free(ctx);
        return 1;
    }

    printf("oreo-kernel consumer OK (serialized %zu bytes)\n", needed);

    oreo_free_solid(clone);
    free(buf);
    oreo_free_solid(box);
    oreo_context_free(ctx);
    return 0;
}
