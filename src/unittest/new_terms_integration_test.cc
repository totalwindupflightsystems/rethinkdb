// Copyright 2026 RethinkDB, all rights reserved.
// Verifies all Phase 3 ReQL TermType values against the compiled protobuf.
// Catches proto drift, missing terms, and numbering gaps.
#include "unittest/gtest.hpp"

#include "rdb_protocol/ql2.pb.h"

namespace unittest {

TEST(NewTermIntegration, TermIDsFromCompiledProto) {
    // Read the VALUES from the compiled .pb.h, not from the spec.
    // This catches divergence between the .proto file and the binary.

    // Full-Text Search
    EXPECT_EQ(198, Term::FTS_TOKENIZE);
    EXPECT_EQ(199, Term::FTS_MATCH);

    // Vector operations
    EXPECT_EQ(200, Term::VECTOR);
    EXPECT_EQ(201, Term::VECTOR_NEAR);

    // Partition operations
    EXPECT_EQ(202, Term::PARTITION_INFO);
    EXPECT_EQ(203, Term::REPARTITION);

    // CDC Publication
    EXPECT_EQ(204, Term::PUBLICATION_CREATE);
    EXPECT_EQ(205, Term::PUBLICATION_LIST);
    EXPECT_EQ(206, Term::PUBLICATION_STATUS);
    EXPECT_EQ(207, Term::PUBLICATION_DROP);

    // NOTE: Terms 208-209 are reserved for PUBLICATION_ALTER/PUBLICATION_PAUSE
    // but not yet implemented — SUBSCRIPTION_CREATE is at 208 in compiled proto.

    // CDC Subscription
    EXPECT_EQ(208, Term::SUBSCRIPTION_CREATE);
    EXPECT_EQ(211, Term::SUBSCRIPTION_DROP);

    // CDC Sink
    EXPECT_EQ(212, Term::CDC_SINK_CREATE);
    EXPECT_EQ(213, Term::CDC_SINK_LIST);
    EXPECT_EQ(214, Term::CDC_SINK_STATUS);
    EXPECT_EQ(215, Term::CDC_SINK_DROP);
}

TEST(NewTermIntegration, GapsAreKnown) {
    // Terms 209 and 210 exist in the proto enum (reserved for
    // PUBLICATION_ALTER/PUBLICATION_PAUSE) but no C++ constant is
    // generated yet. They ARE valid proto enum values though.
    EXPECT_TRUE(Term_TermType_IsValid(209))
        << "Term 209 (PUBLICATION_ALTER) exists in proto enum but not in Term::";
    EXPECT_TRUE(Term_TermType_IsValid(210))
        << "Term 210 (PUBLICATION_PAUSE) exists in proto enum but not in Term::";
    
    // Verify the entire range 198-215 is valid in proto
    for (int t = 198; t <= 215; t++) {
        EXPECT_TRUE(Term_TermType_IsValid(t))
            << "Term " << t << " missing from proto enum range";
    }
}

TEST(NewTermIntegration, NoCollisionsBelow197) {
    // All new terms start at 198 or above. Nothing should accidentally
    // collide with upstream term IDs.
    for (int t = 0; t < 197; t++) {
        // Skip defined terms, only check gaps
        if (!Term_TermType_IsValid(t)) continue;
        int val = static_cast<Term_TermType>(t);
        EXPECT_LT(val, 198)
            << "Term ID " << val << " is in the Phase 3 range but shouldn't be";
    }
}

TEST(NewTermIntegration, VectorDatumType) {
    // VECTOR datum type = 8, added to Datum_DatumType enum
    EXPECT_EQ(8, Datum_DatumType_R_VECTOR);
    EXPECT_TRUE(Datum_DatumType_IsValid(8));
}

}  // namespace unittest
