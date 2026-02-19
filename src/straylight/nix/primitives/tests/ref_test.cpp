// straylight::nix::primitives::tests
//
// Tests for non-nullable Ref<T> smart pointer.
// Unit tests and property-based tests.

// Catch2 must be included before rapidcheck/catch.h
#include <catch2/catch_test_macros.hpp>

// RapidCheck for property-based testing
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include "../ref.h"

using namespace straylight::nix::primitives;

// ─────────────────────────────────────────────────────────────────────────────
// Test fixtures and helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

struct Base {
  virtual ~Base() = default;
  virtual auto value() const -> int { return 0; }
};

struct Derived : Base {
  int val;
  explicit Derived(int v) : val(v) {}
  auto value() const -> int override { return val; }
};

struct Unrelated {
  int x;
};

// Track construction/destruction for lifetime tests
struct Tracked {
  static inline int instances = 0;
  static inline int constructions = 0;

  int id;

  Tracked() : id(++constructions) { ++instances; }
  explicit Tracked(int i) : id(i) {
    ++constructions;
    ++instances;
  }
  Tracked(const Tracked& other) : id(other.id) { ++instances; }
  Tracked(Tracked&& other) noexcept : id(other.id) { ++instances; }
  ~Tracked() { --instances; }

  static void reset() {
    instances = 0;
    constructions = 0;
  }
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Construction tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("make_ref creates non-null Ref", "[ref][construction]") {
  auto r = make_ref<int>(42);
  REQUIRE(*r == 42);
  REQUIRE(r.get() != nullptr);
}

TEST_CASE("make_ref with complex type", "[ref][construction]") {
  auto r = make_ref<std::string>("hello world");
  REQUIRE(*r == "hello world");
  REQUIRE(r->size() == 11);
}

TEST_CASE("make_ref with multiple arguments", "[ref][construction]") {
  auto r = make_ref<std::vector<int>>(5, 42);
  REQUIRE(r->size() == 5);
  REQUIRE((*r)[0] == 42);
}

TEST_CASE("Ref from shared_ptr", "[ref][construction]") {
  auto sp = std::make_shared<int>(123);
  Ref<int> r(sp);
  REQUIRE(*r == 123);
  REQUIRE(sp.use_count() == 2); // Both sp and r hold a reference
}

TEST_CASE("Ref from rvalue shared_ptr", "[ref][construction]") {
  Ref<int> r(std::make_shared<int>(456));
  REQUIRE(*r == 456);
}

TEST_CASE("Ref from raw pointer", "[ref][construction]") {
  Ref<int> r(new int(789));
  REQUIRE(*r == 789);
}

TEST_CASE("Ref throws on null shared_ptr", "[ref][construction]") {
  std::shared_ptr<int> null_sp;
  REQUIRE_THROWS_AS(Ref<int>(null_sp), std::invalid_argument);
}

TEST_CASE("Ref throws on null rvalue shared_ptr", "[ref][construction]") {
  REQUIRE_THROWS_AS(Ref<int>(std::shared_ptr<int>()), std::invalid_argument);
}

TEST_CASE("Ref throws on null raw pointer", "[ref][construction]") {
  REQUIRE_THROWS_AS(Ref<int>(static_cast<int*>(nullptr)), std::invalid_argument);
}

// ─────────────────────────────────────────────────────────────────────────────
// Accessor tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("operator* returns reference", "[ref][accessor]") {
  auto r = make_ref<int>(42);
  REQUIRE(*r == 42);
  *r = 100;
  REQUIRE(*r == 100);
}

TEST_CASE("operator-> accesses members", "[ref][accessor]") {
  auto r = make_ref<std::string>("test");
  REQUIRE(r->length() == 4);
  r->append("ing");
  REQUIRE(*r == "testing");
}

TEST_CASE("get() returns raw pointer", "[ref][accessor]") {
  auto r = make_ref<int>(42);
  int* p = r.get();
  REQUIRE(p != nullptr);
  REQUIRE(*p == 42);
}

TEST_CASE("get_ptr() returns shared_ptr", "[ref][accessor]") {
  auto r = make_ref<int>(42);
  std::shared_ptr<int> sp = r.get_ptr();
  REQUIRE(sp.use_count() == 2);
  REQUIRE(*sp == 42);
}

TEST_CASE("get_ptr() rvalue moves shared_ptr", "[ref][accessor]") {
  auto r = make_ref<int>(42);
  std::shared_ptr<int> sp = std::move(r).get_ptr();
  REQUIRE(sp.use_count() == 1);
  REQUIRE(*sp == 42);
}

TEST_CASE("use_count() returns reference count", "[ref][accessor]") {
  auto r1 = make_ref<int>(42);
  REQUIRE(r1.use_count() == 1);

  auto r2 = r1;
  REQUIRE(r1.use_count() == 2);
  REQUIRE(r2.use_count() == 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// Conversion tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("implicit conversion to shared_ptr", "[ref][conversion]") {
  auto r = make_ref<int>(42);
  std::shared_ptr<int> sp = r;
  REQUIRE(*sp == 42);
  REQUIRE(sp.use_count() == 2);
}

TEST_CASE("implicit conversion to const T&", "[ref][conversion]") {
  auto r = make_ref<std::string>("hello");
  const std::string& ref = r;
  REQUIRE(ref == "hello");
}

TEST_CASE("Ref<Derived> converts to Ref<Base>", "[ref][conversion]") {
  auto derived = make_ref<Derived>(42);
  Ref<Base> base = derived;
  REQUIRE(base->value() == 42);
}

TEST_CASE("function accepting Ref<Base> takes Ref<Derived>", "[ref][conversion]") {
  auto fn = [](const Ref<Base>& b) { return b->value(); };
  auto d = make_ref<Derived>(99);
  REQUIRE(fn(d) == 99);
}

// ─────────────────────────────────────────────────────────────────────────────
// Casting tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("cast() to derived type succeeds", "[ref][casting]") {
  Ref<Base> base(std::make_shared<Derived>(42));
  Ref<Derived> derived = base.cast<Derived>();
  REQUIRE(derived->val == 42);
}

TEST_CASE("cast() to wrong type throws", "[ref][casting]") {
  auto base = make_ref<Base>();
  REQUIRE_THROWS_AS(base.cast<Derived>(), std::invalid_argument);
}

TEST_CASE("ref_cast function works", "[ref][casting]") {
  Ref<Base> base(std::make_shared<Derived>(100));
  auto derived = ref_cast<Derived>(base);
  REQUIRE(derived->val == 100);
}

TEST_CASE("dynamic_pointer_cast returns shared_ptr", "[ref][casting]") {
  Ref<Base> base(std::make_shared<Derived>(50));
  auto derived_sp = base.dynamic_pointer_cast<Derived>();
  REQUIRE(derived_sp != nullptr);
  REQUIRE(derived_sp->val == 50);
}

TEST_CASE("dynamic_pointer_cast returns null on failure", "[ref][casting]") {
  auto base = make_ref<Base>();
  auto derived_sp = base.dynamic_pointer_cast<Derived>();
  REQUIRE(derived_sp == nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// Comparison tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("equality comparison", "[ref][comparison]") {
  auto r1 = make_ref<int>(42);
  auto r2 = r1;
  auto r3 = make_ref<int>(42);

  REQUIRE(r1 == r2); // Same object
  REQUIRE(r1 != r3); // Different objects, same value
}

TEST_CASE("three-way comparison", "[ref][comparison]") {
  auto r1 = make_ref<int>(1);
  auto r2 = make_ref<int>(2);
  auto r3 = r1;

  REQUIRE((r1 <=> r3) == std::strong_ordering::equal);
  // Ordering is by address, not value
  REQUIRE((r1 <=> r2) != std::strong_ordering::equal);
}

TEST_CASE("comparison with different but compatible types", "[ref][comparison]") {
  auto derived = make_ref<Derived>(42);
  Ref<Base> base = derived;

  REQUIRE(base == derived);
}

// ─────────────────────────────────────────────────────────────────────────────
// Hash tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("std::hash specialization works", "[ref][hash]") {
  auto r1 = make_ref<int>(42);
  auto r2 = r1;
  auto r3 = make_ref<int>(42);

  std::hash<Ref<int>> hasher;

  REQUIRE(hasher(r1) == hasher(r2)); // Same object, same hash
  REQUIRE(hasher(r1) != hasher(r3)); // Different objects, likely different hash
}

TEST_CASE("Ref works in unordered_set", "[ref][hash]") {
  std::unordered_set<Ref<int>> set;

  auto r1 = make_ref<int>(1);
  auto r2 = make_ref<int>(2);
  auto r3 = r1;

  set.insert(r1);
  set.insert(r2);
  set.insert(r3); // Same as r1, shouldn't add

  REQUIRE(set.size() == 2);
  REQUIRE(set.count(r1) == 1);
  REQUIRE(set.count(r3) == 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// Lifetime tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Ref properly manages lifetime", "[ref][lifetime]") {
  Tracked::reset();

  {
    auto r = make_ref<Tracked>();
    REQUIRE(Tracked::instances == 1);

    {
      auto r2 = r;
      REQUIRE(Tracked::instances == 1); // Still one object
      REQUIRE(r.use_count() == 2);
    }

    REQUIRE(Tracked::instances == 1); // r2 gone, object still alive
    REQUIRE(r.use_count() == 1);
  }

  REQUIRE(Tracked::instances == 0); // Object destroyed
}

TEST_CASE("move constructor doesn't copy object", "[ref][lifetime]") {
  Tracked::reset();

  auto r1 = make_ref<Tracked>(42);
  REQUIRE(Tracked::instances == 1);

  auto r2 = std::move(r1);
  REQUIRE(Tracked::instances == 1); // No additional copy
  REQUIRE(r2->id == 42);
}

// ─────────────────────────────────────────────────────────────────────────────
// Copy and move semantics
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("copy constructor shares ownership", "[ref][copy]") {
  auto r1 = make_ref<int>(42);
  auto r2 = r1;

  REQUIRE(r1.get() == r2.get());
  REQUIRE(r1.use_count() == 2);
}

TEST_CASE("copy assignment shares ownership", "[ref][copy]") {
  auto r1 = make_ref<int>(42);
  auto r2 = make_ref<int>(100);

  r2 = r1;

  REQUIRE(r1.get() == r2.get());
  REQUIRE(*r2 == 42);
}

TEST_CASE("move constructor transfers ownership", "[ref][move]") {
  auto r1 = make_ref<int>(42);
  int* raw = r1.get();
  auto r2 = std::move(r1);

  REQUIRE(r2.get() == raw);
  REQUIRE(*r2 == 42);
}

TEST_CASE("move assignment transfers ownership", "[ref][move]") {
  auto r1 = make_ref<int>(42);
  auto r2 = make_ref<int>(100);
  int* raw = r1.get();

  r2 = std::move(r1);

  REQUIRE(r2.get() == raw);
  REQUIRE(*r2 == 42);
}

// ─────────────────────────────────────────────────────────────────────────────
// Property-based tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("make_ref never returns null", "[ref][property]") {
  rc::prop("make_ref<int> is never null", []() {
    auto val = *rc::gen::arbitrary<int>();
    auto r = make_ref<int>(val);

    RC_ASSERT(r.get() != nullptr);
    RC_ASSERT(*r == val);
  });
}

TEST_CASE("copy preserves value", "[ref][property]") {
  rc::prop("copying Ref preserves the pointed value", []() {
    auto val = *rc::gen::arbitrary<int>();
    auto r1 = make_ref<int>(val);
    auto r2 = r1;

    RC_ASSERT(*r1 == *r2);
    RC_ASSERT(r1.get() == r2.get());
  });
}

TEST_CASE("copy increases use_count", "[ref][property]") {
  rc::prop("copying increases reference count", []() {
    auto val = *rc::gen::arbitrary<int>();
    auto r1 = make_ref<int>(val);
    auto initial_count = r1.use_count();

    auto r2 = r1;

    RC_ASSERT(r1.use_count() == initial_count + 1);
    RC_ASSERT(r2.use_count() == initial_count + 1);
  });
}

TEST_CASE("shared_ptr conversion roundtrip", "[ref][property]") {
  rc::prop("Ref -> shared_ptr -> Ref preserves identity", []() {
    auto val = *rc::gen::arbitrary<int>();
    auto r1 = make_ref<int>(val);

    std::shared_ptr<int> sp = r1;
    Ref<int> r2(sp);

    RC_ASSERT(r1.get() == r2.get());
    RC_ASSERT(*r1 == *r2);
  });
}

TEST_CASE("equality is reflexive", "[ref][property]") {
  rc::prop("r == r is always true", []() {
    auto val = *rc::gen::arbitrary<int>();
    auto r = make_ref<int>(val);

    RC_ASSERT(r == r);
  });
}

TEST_CASE("equality is symmetric", "[ref][property]") {
  rc::prop("r1 == r2 implies r2 == r1", []() {
    auto val = *rc::gen::arbitrary<int>();
    auto r1 = make_ref<int>(val);
    auto r2 = r1;

    RC_ASSERT((r1 == r2) == (r2 == r1));
  });
}

TEST_CASE("different make_ref calls create different objects", "[ref][property]") {
  rc::prop("two make_ref calls create distinct objects", []() {
    auto val = *rc::gen::arbitrary<int>();
    auto r1 = make_ref<int>(val);
    auto r2 = make_ref<int>(val);

    RC_ASSERT(r1 != r2);   // Different objects
    RC_ASSERT(*r1 == *r2); // Same value
  });
}

TEST_CASE("hash consistency", "[ref][property]") {
  rc::prop("equal Refs have equal hashes", []() {
    auto val = *rc::gen::arbitrary<int>();
    auto r1 = make_ref<int>(val);
    auto r2 = r1;

    std::hash<Ref<int>> hasher;
    RC_ASSERT(hasher(r1) == hasher(r2));
  });
}

TEST_CASE("comparison is consistent with equality", "[ref][property]") {
  rc::prop("(r1 <=> r2) == equal implies r1 == r2", []() {
    auto v1 = *rc::gen::arbitrary<int>();
    auto v2 = *rc::gen::arbitrary<int>();
    auto r1 = make_ref<int>(v1);
    auto r2 = make_ref<int>(v2);
    auto r3 = r1;

    RC_ASSERT((r1 == r3) == ((r1 <=> r3) == std::strong_ordering::equal));
    RC_ASSERT((r1 != r2) == ((r1 <=> r2) != std::strong_ordering::equal));
  });
}

TEST_CASE("string Ref operations", "[ref][property]") {
  rc::prop("Ref<string> operations work correctly", []() {
    auto str = *rc::gen::arbitrary<std::string>();
    auto r = make_ref<std::string>(str);

    RC_ASSERT(*r == str);
    RC_ASSERT(r->size() == str.size());
    RC_ASSERT(r->empty() == str.empty());
  });
}
