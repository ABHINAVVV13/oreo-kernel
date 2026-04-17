// test_doc_identity_nolegacy.cpp — Phase 6 no-legacy acceptance suite.
//
// Mirrors the scenarios in test_doc_identity.cpp but uses ONLY the
// ctx-aware C API (oreo_ctx_*). This test is registered in both
// OREO_ENABLE_LEGACY_API=ON and OREO_ENABLE_LEGACY_API=OFF builds —
// gating it on OFF-only would hide regressions from ON-build CI, and
// there's no reason it shouldn't pass on either configuration.
//
// Phase 6 invariant: every acceptance criterion from
// docs/identity-model.md §§2–6 is re-proven without touching
// oreo_face_name / oreo_serialize / oreo_deserialize / oreo_free_buffer
// / oreo_init / oreo_last_error.

#include <gtest/gtest.h>

#include "oreo_kernel.h"

#include "core/kernel_context.h"
#include "core/tag_allocator.h"
#include "io/oreo_serialize.h"  // for oreo::OREO_SERIALIZE_FORMAT_VERSION

#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

// RAII to keep bodies terse and leak-free.
struct CtxGuard {
    OreoContext h = nullptr;
    CtxGuard() = default;
    explicit CtxGuard(OreoContext c) : h(c) {}
    ~CtxGuard() { if (h) oreo_context_free(h); }
    CtxGuard(const CtxGuard&) = delete;
    CtxGuard& operator=(const CtxGuard&) = delete;
};

struct SolidGuard {
    OreoSolid h = nullptr;
    SolidGuard() = default;
    explicit SolidGuard(OreoSolid s) : h(s) {}
    ~SolidGuard() { if (h) oreo_free_solid(h); }
    SolidGuard(const SolidGuard&) = delete;
    SolidGuard& operator=(const SolidGuard&) = delete;
};

// Helper: read a name into a std::string via the size-probe protocol.
std::string readName(OreoContext ctx, OreoSolid s, int index) {
    size_t needed = 0;
    int rc = oreo_ctx_face_name(ctx, s, index, nullptr, 0, &needed);
    if (rc != OREO_OK) return {};
    std::vector<char> buf(needed + 1);
    rc = oreo_ctx_face_name(ctx, s, index, buf.data(), buf.size(), &needed);
    if (rc != OREO_OK) return {};
    return std::string(buf.data());
}

// Helper: serialize into a std::vector via size-probe.
std::vector<uint8_t> serializeBuf(OreoContext ctx, OreoSolid s) {
    size_t needed = 0;
    int rc = oreo_ctx_serialize(ctx, s, nullptr, 0, &needed);
    if (rc != OREO_OK || needed == 0) return {};
    std::vector<uint8_t> buf(needed);
    rc = oreo_ctx_serialize(ctx, s, buf.data(), buf.size(), &needed);
    if (rc != OREO_OK) return {};
    return buf;
}

}  // anonymous namespace

// ─── 1. Canonical: full 64-bit identity survives every layer ────────

TEST(DocIdentityNoLegacy, HighBitsSurviveEveryLayer) {
    constexpr std::uint64_t kHighDocId = 0xDEADBEEFCAFEBABEull;
    const std::string kExpectedDocHex = "deadbeefcafebabe";

    oreo::TagAllocator::clearDocumentIdRegistry();
    CtxGuard ctx(oreo_context_create_with_doc(kHighDocId, nullptr));
    ASSERT_NE(ctx.h, nullptr);
    ASSERT_EQ(oreo_context_document_id(ctx.h), kHighDocId);

    SolidGuard box(oreo_ctx_make_box(ctx.h, 10, 20, 30));
    ASSERT_NE(box.h, nullptr);

    const std::string face1 = readName(ctx.h, box.h, 1);
    ASSERT_FALSE(face1.empty());
    EXPECT_NE(face1.find(kExpectedDocHex), std::string::npos)
        << "face1 = '" << face1 << "' missing full 64-bit docId hex";

    // Shape-id accessor: full identity returned, not a v1-squeezed low32.
    OreoShapeId sid{};
    ASSERT_EQ(oreo_face_shape_id(box.h, 1, &sid), OREO_OK);
    EXPECT_EQ(sid.document_id, kHighDocId)
        << "oreo_face_shape_id squeezed docId to " << std::hex << sid.document_id;
    EXPECT_GT(sid.counter, 0u);

    // Serialize / deserialize round-trip — the v3 writer keeps the full
    // identity, and the v3 reader restores it without fabrication.
    auto buf = serializeBuf(ctx.h, box.h);
    ASSERT_FALSE(buf.empty());
    ASSERT_EQ(buf[0], oreo::OREO_SERIALIZE_FORMAT_VERSION);

    SolidGuard restored(oreo_ctx_deserialize(ctx.h, buf.data(), buf.size()));
    ASSERT_NE(restored.h, nullptr);
    const std::string restoredFace1 = readName(ctx.h, restored.h, 1);
    EXPECT_EQ(face1, restoredFace1);

    // Second context with a distinct high-bit docId must not collide.
    CtxGuard ctx2(oreo_context_create_with_doc(0xCAFEBABEDEADBEEFull, nullptr));
    ASSERT_NE(ctx2.h, nullptr);
    SolidGuard box2(oreo_ctx_make_box(ctx2.h, 10, 20, 30));
    ASSERT_NE(box2.h, nullptr);
    const std::string face1b = readName(ctx2.h, box2.h, 1);
    EXPECT_NE(face1, face1b);
}

// ─── 2. Size-probe protocol works for name + serialize ──────────────

TEST(DocIdentityNoLegacy, SizeProbeProtocol) {
    oreo::TagAllocator::clearDocumentIdRegistry();
    CtxGuard ctx(oreo_context_create_with_doc(1, nullptr));
    ASSERT_NE(ctx.h, nullptr);
    SolidGuard box(oreo_ctx_make_box(ctx.h, 1, 2, 3));
    ASSERT_NE(box.h, nullptr);

    // Face name probe.
    size_t needed = 0;
    EXPECT_EQ(oreo_ctx_face_name(ctx.h, box.h, 1, nullptr, 0, &needed), OREO_OK);
    EXPECT_GT(needed, 0u);
    std::vector<char> buf(needed + 1);
    EXPECT_EQ(oreo_ctx_face_name(ctx.h, box.h, 1, buf.data(), buf.size(), &needed),
              OREO_OK);
    EXPECT_EQ(buf[needed], '\0');

    // Too-small buffer returns BUFFER_TOO_SMALL and writes a truncated prefix.
    std::vector<char> tiny(3);
    EXPECT_EQ(oreo_ctx_face_name(ctx.h, box.h, 1, tiny.data(), tiny.size(), &needed),
              OREO_BUFFER_TOO_SMALL);
    EXPECT_EQ(tiny[tiny.size() - 1], '\0');  // truncation still NUL-terminates

    // Serialize probe.
    size_t serNeeded = 0;
    EXPECT_EQ(oreo_ctx_serialize(ctx.h, box.h, nullptr, 0, &serNeeded), OREO_OK);
    EXPECT_GT(serNeeded, 0u);
    std::vector<uint8_t> serBuf(serNeeded);
    EXPECT_EQ(oreo_ctx_serialize(ctx.h, box.h, serBuf.data(), serBuf.size(), &serNeeded),
              OREO_OK);
    EXPECT_EQ(serBuf[0], oreo::OREO_SERIALIZE_FORMAT_VERSION);
}

// ─── 3. UUID-derived documentId determinism ─────────────────────────

TEST(DocIdentityNoLegacy, UUIDDerivedDocumentIdStable) {
    oreo::TagAllocator::clearDocumentIdRegistry();
    uint64_t id1 = 0;
    {
        CtxGuard ctx(oreo_context_create_with_doc(0, "stable-uuid-xyz"));
        ASSERT_NE(ctx.h, nullptr);
        id1 = oreo_context_document_id(ctx.h);
        EXPECT_NE(id1, 0u);
    }
    uint64_t id2 = 0;
    {
        CtxGuard ctx(oreo_context_create_with_doc(0, "stable-uuid-xyz"));
        ASSERT_NE(ctx.h, nullptr);
        id2 = oreo_context_document_id(ctx.h);
    }
    EXPECT_EQ(id1, id2) << "SipHash on identical UUIDs must produce identical docIds";
}

// ─── 4. Serialize v3 byte layout sanity ─────────────────────────────

TEST(DocIdentityNoLegacy, SerializeV3VersionByteAndTailIdentity) {
    oreo::TagAllocator::clearDocumentIdRegistry();
    CtxGuard ctx(oreo_context_create_with_doc(0xFEDCBA9876543210ull, nullptr));
    ASSERT_NE(ctx.h, nullptr);
    SolidGuard box(oreo_ctx_make_box(ctx.h, 1, 2, 3));
    ASSERT_NE(box.h, nullptr);
    auto buf = serializeBuf(ctx.h, box.h);
    ASSERT_GE(buf.size(), 17u);  // minimum v3 header + tail
    EXPECT_EQ(buf[0], 3);

    // Corrupt the version byte to 2 (reserved/illegal) — must reject.
    buf[0] = 2;
    SolidGuard restored(oreo_ctx_deserialize(ctx.h, buf.data(), buf.size()));
    EXPECT_EQ(restored.h, nullptr);
}

// ─── 5. Multi-thread: two contexts, distinct docIds, independent counters ─

TEST(DocIdentityNoLegacy, TwoThreadsTwoContextsIndependentCounters) {
    oreo::TagAllocator::clearDocumentIdRegistry();
    constexpr std::uint64_t kDocA = 0x1111222233334444ull;
    constexpr std::uint64_t kDocB = 0x5555666677778888ull;

    std::vector<std::string> facesA, facesB;
    std::thread tA([&] {
        CtxGuard ctx(oreo_context_create_with_doc(kDocA, nullptr));
        ASSERT_NE(ctx.h, nullptr);
        for (int i = 0; i < 8; ++i) {
            SolidGuard s(oreo_ctx_make_box(ctx.h, 1, 2, 3));
            ASSERT_NE(s.h, nullptr);
            facesA.push_back(readName(ctx.h, s.h, 1));
        }
    });
    std::thread tB([&] {
        CtxGuard ctx(oreo_context_create_with_doc(kDocB, nullptr));
        ASSERT_NE(ctx.h, nullptr);
        for (int i = 0; i < 8; ++i) {
            SolidGuard s(oreo_ctx_make_box(ctx.h, 1, 2, 3));
            ASSERT_NE(s.h, nullptr);
            facesB.push_back(readName(ctx.h, s.h, 1));
        }
    });
    tA.join();
    tB.join();

    ASSERT_EQ(facesA.size(), 8u);
    ASSERT_EQ(facesB.size(), 8u);

    // Every face in A must be distinct from every face in B — their
    // docId hex distinguishes them.
    for (const auto& a : facesA) {
        for (const auto& b : facesB) {
            EXPECT_NE(a, b) << "Cross-doc collision: " << a;
        }
        EXPECT_NE(a.find("1111222233334444"), std::string::npos)
            << "facesA entry '" << a << "' missing docA hex";
    }
    for (const auto& b : facesB) {
        EXPECT_NE(b.find("5555666677778888"), std::string::npos)
            << "facesB entry '" << b << "' missing docB hex";
    }
}

// ─── 6. Array-bounds guards on the ctx-aware fillet ─────────────────

TEST(DocIdentityNoLegacy, ArrayBoundsGuards) {
    oreo::TagAllocator::clearDocumentIdRegistry();
    CtxGuard ctx(oreo_context_create_with_doc(7, nullptr));
    ASSERT_NE(ctx.h, nullptr);
    SolidGuard box(oreo_ctx_make_box(ctx.h, 10, 10, 10));
    ASSERT_NE(box.h, nullptr);

    SolidGuard r1(oreo_ctx_fillet(ctx.h, box.h, nullptr, -1, 1.0));
    EXPECT_EQ(r1.h, nullptr);
    SolidGuard r2(oreo_ctx_fillet(ctx.h, box.h, nullptr, 3, 1.0));
    EXPECT_EQ(r2.h, nullptr);
    SolidGuard r3(oreo_ctx_fillet(ctx.h, box.h, nullptr, 0, 1.0));
    (void)r3;  // must not SEGV
}
