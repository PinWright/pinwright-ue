// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for E-geometry-warp-extent-semantics lives in
// TestGeometryWarpExtentSemanticsDocs.cpp.
//
// Two fix hosts independently authored the same regression test for this ticket
// under different filenames. Both defined the identical automation-test classes
// (FGeometryWarpExtentNamespaceDocTest, FGeometryTwistExtentDocTest,
// FGeometryTaperExtentDocTest, FGeometryBendExtentDocTest) with identical
// automation-test path names, so compiling both produced an LNK2005 duplicate-symbol
// link error. They cannot coexist as separate definitions.
//
// Resolution (lose no coverage): the four test classes are kept once, in
// TestGeometryWarpExtentSemanticsDocs.cpp, which was enriched with this file's
// extra assertions — the explicit `symmetric half-extent` phrasing on the
// twist/taper/bend method pages and the taper `flareX` / `not a percentage` trap.
// This file is intentionally left with no test definitions to break the symbol
// collision while preserving the merged regression coverage from both hosts.
