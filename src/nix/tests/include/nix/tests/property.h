// straylight // nix // tests
//
// Property-based testing utilities
//
// This header provides the correct include order for Catch2 + RapidCheck.
// ALWAYS use this header instead of including Catch2/RapidCheck directly.
//
// Usage:
//   #include "nix/tests/property.h"
//
// This gives you:
//   - Catch2 TEST_CASE, REQUIRE, SECTION, etc.
//   - RapidCheck rc::prop() for property-based tests
//   - RapidCheck generators (rc::gen::*)

#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// Catch2 v3 - MUST be included first
// ─────────────────────────────────────────────────────────────────────────────
// RapidCheck's catch.h checks for CATCH_TEST_MACROS_HPP_INCLUDED to detect
// Catch2 v3. If not defined, it tries to include the non-existent catch.hpp.

#include <catch2/catch_test_macros.hpp>

// ─────────────────────────────────────────────────────────────────────────────
// RapidCheck - property-based testing
// ─────────────────────────────────────────────────────────────────────────────

#include <rapidcheck.h>
#include <rapidcheck/catch.h>
