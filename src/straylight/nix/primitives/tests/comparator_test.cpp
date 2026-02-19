// straylight::nix::primitives::comparator tests
//
// Tests for comparison utilities: concepts, compare_by, comparators, and macros

#include <algorithm>
#include <compare>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "straylight/nix/primitives/comparator.h"

namespace cmp = straylight::nix::primitives;

// ─────────────────────────────────────────────────────────────────────────────
// Test types
// ─────────────────────────────────────────────────────────────────────────────

struct Point {
  int x;
  int y;

  STRAYLIGHT_DEFINE_COMPARISON(Point, x, y)
};

struct Person {
  std::string name;
  int age;
  double height;

  STRAYLIGHT_DEFINE_COMPARISON(Person, name, age, height)
};

struct SimpleRecord {
  int id;
  std::string label;

  // Test defaulted comparison for simple cases
  auto operator<=>(const SimpleRecord&) const = default;
  bool operator==(const SimpleRecord&) const = default;
};

struct WeaklyOrdered {
  double value;

  // double gives weak_ordering via spaceship
  auto operator<=>(const WeaklyOrdered&) const = default;
  bool operator==(const WeaklyOrdered&) const = default;
};

struct PartiallyOrdered {
  std::optional<int> value;

  // optional gives partial_ordering when containing partial types or via custom impl
  std::partial_ordering operator<=>(const PartiallyOrdered& other) const {
    if (!value.has_value() && !other.value.has_value()) {
      return std::partial_ordering::equivalent;
    }
    if (!value.has_value() || !other.value.has_value()) {
      return std::partial_ordering::unordered;
    }
    return *value <=> *other.value;
  }
  bool operator==(const PartiallyOrdered& other) const {
    return (*this <=> other) == std::partial_ordering::equivalent;
  }
};

// ─────────────────────────────────────────────────────────────────────────────
// Concept tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("comparison_category concept recognizes ordering types", "[comparator][concepts]") {
  STATIC_REQUIRE(cmp::comparison_category<std::strong_ordering>);
  STATIC_REQUIRE(cmp::comparison_category<std::weak_ordering>);
  STATIC_REQUIRE(cmp::comparison_category<std::partial_ordering>);

  STATIC_REQUIRE_FALSE(cmp::comparison_category<int>);
  STATIC_REQUIRE_FALSE(cmp::comparison_category<bool>);
  STATIC_REQUIRE_FALSE(cmp::comparison_category<std::string>);
}

TEST_CASE("naturally_three_way_comparable concept", "[comparator][concepts]") {
  STATIC_REQUIRE(cmp::naturally_three_way_comparable<int>);
  STATIC_REQUIRE(cmp::naturally_three_way_comparable<std::string>);
  STATIC_REQUIRE(cmp::naturally_three_way_comparable<Point>);
  STATIC_REQUIRE(cmp::naturally_three_way_comparable<SimpleRecord>);
}

TEST_CASE("fully_ordered concept", "[comparator][concepts]") {
  STATIC_REQUIRE(cmp::fully_ordered<int>);
  STATIC_REQUIRE(cmp::fully_ordered<std::string>);
  STATIC_REQUIRE(cmp::fully_ordered<Point>);
  STATIC_REQUIRE(cmp::fully_ordered<Person>);
}

TEST_CASE("three_way_comparable_with_key concept", "[comparator][concepts]") {
  auto get_age = [](const Person& p) { return p.age; };
  STATIC_REQUIRE(cmp::three_way_comparable_with_key<Person, decltype(get_age)>);
  STATIC_REQUIRE(cmp::three_way_comparable_with_key<Person, decltype(&Person::age)>);
  STATIC_REQUIRE(cmp::three_way_comparable_with_key<Person, decltype(&Person::name)>);
}

TEST_CASE("projectable_for_comparison concept", "[comparator][concepts]") {
  STATIC_REQUIRE(cmp::projectable_for_comparison<Person, decltype(&Person::age)>);
  STATIC_REQUIRE(cmp::projectable_for_comparison<Point, decltype(&Point::x)>);

  auto get_length = [](const std::string& s) { return s.length(); };
  STATIC_REQUIRE(cmp::projectable_for_comparison<std::string, decltype(get_length)>);
}

// ─────────────────────────────────────────────────────────────────────────────
// STRAYLIGHT_DEFINE_COMPARISON macro tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("STRAYLIGHT_DEFINE_COMPARISON generates equality", "[comparator][macro]") {
  Point p1{1, 2};
  Point p2{1, 2};
  Point p3{1, 3};

  REQUIRE(p1 == p2);
  REQUIRE_FALSE(p1 == p3);
  REQUIRE(p1 != p3);
}

TEST_CASE("STRAYLIGHT_DEFINE_COMPARISON generates spaceship operator", "[comparator][macro]") {
  Point p1{1, 2};
  Point p2{2, 1};
  Point p3{1, 3};

  REQUIRE((p1 <=> p2) < 0); // x=1 < x=2
  REQUIRE((p2 <=> p1) > 0);
  REQUIRE((p1 <=> p3) < 0); // x equal, y=2 < y=3
}

TEST_CASE("STRAYLIGHT_DEFINE_COMPARISON works with strings", "[comparator][macro]") {
  Person alice{"Alice", 30, 1.65};
  Person bob{"Bob", 25, 1.80};
  Person alice2{"Alice", 30, 1.65};

  REQUIRE(alice == alice2);
  REQUIRE(alice != bob);
  REQUIRE((alice <=> bob) < 0); // "Alice" < "Bob"
}

TEST_CASE("STRAYLIGHT_DEFINE_COMPARISON provides total ordering", "[comparator][macro]") {
  std::vector<Point> points{{3, 1}, {1, 2}, {1, 1}, {2, 2}};
  std::sort(points.begin(), points.end());

  REQUIRE(points[0] == Point{1, 1});
  REQUIRE(points[1] == Point{1, 2});
  REQUIRE(points[2] == Point{2, 2});
  REQUIRE(points[3] == Point{3, 1});
}

TEST_CASE("STRAYLIGHT_DEFINE_COMPARISON works in std::set", "[comparator][macro]") {
  std::set<Point> point_set;
  point_set.insert({1, 2});
  point_set.insert({3, 4});
  point_set.insert({1, 2}); // duplicate

  REQUIRE(point_set.size() == 2);
  REQUIRE(point_set.contains({1, 2}));
  REQUIRE(point_set.contains({3, 4}));
}

// ─────────────────────────────────────────────────────────────────────────────
// compare_by function tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("compare_by with single projection", "[comparator][compare_by]") {
  Person alice{"Alice", 30, 1.65};
  Person bob{"Bob", 25, 1.80};

  // Compare by age
  auto result = cmp::compare_by(alice, bob, &Person::age);
  REQUIRE(result > 0); // 30 > 25

  // Compare by name
  result = cmp::compare_by(alice, bob, &Person::name);
  REQUIRE(result < 0); // "Alice" < "Bob"
}

TEST_CASE("compare_by with lambda projection", "[comparator][compare_by]") {
  std::string s1 = "hello";
  std::string s2 = "hi";

  auto by_length = [](const std::string& s) { return s.length(); };
  auto result = cmp::compare_by(s1, s2, by_length);
  REQUIRE(result > 0); // 5 > 2
}

TEST_CASE("compare_by with multiple projections", "[comparator][compare_by]") {
  Person alice{"Alice", 30, 1.65};
  Person alice_older{"Alice", 35, 1.65};
  Person bob{"Bob", 30, 1.80};

  // Same name, different age
  auto result = cmp::compare_by(alice, alice_older, &Person::name, &Person::age);
  REQUIRE(result < 0); // name equal, 30 < 35

  // Different name
  result = cmp::compare_by(alice, bob, &Person::name, &Person::age);
  REQUIRE(result < 0); // "Alice" < "Bob", age not checked
}

TEST_CASE("compare_by short-circuits on non-equal", "[comparator][compare_by]") {
  int projection_count = 0;
  auto counting_projection = [&projection_count](const Person& p) {
    ++projection_count;
    return p.age;
  };

  Person alice{"Alice", 30, 1.65};
  Person bob{"Bob", 25, 1.80};

  // First projection (name) differs, second (age) should not be called
  cmp::compare_by(alice, bob, &Person::name, counting_projection);
  REQUIRE(projection_count == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// key_comparator tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("key_comparator basic usage", "[comparator][key_comparator]") {
  cmp::key_comparator by_age{&Person::age};

  Person alice{"Alice", 30, 1.65};
  Person bob{"Bob", 25, 1.80};

  REQUIRE(by_age(bob, alice));       // 25 < 30
  REQUIRE_FALSE(by_age(alice, bob)); // 30 < 25 is false
}

TEST_CASE("key_comparator with lambda", "[comparator][key_comparator]") {
  auto by_length = cmp::key_comparator{[](const std::string& s) { return s.length(); }};

  REQUIRE(by_length("hi", "hello"));       // 2 < 5
  REQUIRE_FALSE(by_length("hello", "hi")); // 5 < 2 is false
}

TEST_CASE("key_comparator in std::sort", "[comparator][key_comparator]") {
  std::vector<Person> people{{"Bob", 25, 1.80}, {"Alice", 30, 1.65}, {"Charlie", 20, 1.75}};

  std::sort(people.begin(), people.end(), cmp::key_comparator{&Person::age});

  REQUIRE(people[0].name == "Charlie"); // age 20
  REQUIRE(people[1].name == "Bob");     // age 25
  REQUIRE(people[2].name == "Alice");   // age 30
}

TEST_CASE("key_comparator compare() method returns three-way result",
          "[comparator][key_comparator]") {
  cmp::key_comparator by_age{&Person::age};

  Person alice{"Alice", 30, 1.65};
  Person bob{"Bob", 25, 1.80};
  Person charlie{"Charlie", 30, 1.75};

  REQUIRE(by_age.compare(bob, alice) < 0);
  REQUIRE(by_age.compare(alice, bob) > 0);
  REQUIRE(by_age.compare(alice, charlie) == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// multi_key_comparator tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("multi_key_comparator basic usage", "[comparator][multi_key_comparator]") {
  cmp::multi_key_comparator by_name_then_age{&Person::name, &Person::age};

  Person alice30{"Alice", 30, 1.65};
  Person alice25{"Alice", 25, 1.65};
  Person bob{"Bob", 20, 1.80};

  // Alice < Bob (by name)
  REQUIRE(by_name_then_age(alice30, bob));
  // Alice25 < Alice30 (same name, by age)
  REQUIRE(by_name_then_age(alice25, alice30));
}

TEST_CASE("multi_key_comparator in std::sort", "[comparator][multi_key_comparator]") {
  std::vector<Person> people{
      {"Bob", 25, 1.80}, {"Alice", 30, 1.65}, {"Alice", 25, 1.70}, {"Bob", 20, 1.75}};

  std::sort(people.begin(), people.end(), cmp::multi_key_comparator{&Person::name, &Person::age});

  REQUIRE(people[0].name == "Alice");
  REQUIRE(people[0].age == 25);
  REQUIRE(people[1].name == "Alice");
  REQUIRE(people[1].age == 30);
  REQUIRE(people[2].name == "Bob");
  REQUIRE(people[2].age == 20);
  REQUIRE(people[3].name == "Bob");
  REQUIRE(people[3].age == 25);
}

TEST_CASE("multi_key_comparator compare() method", "[comparator][multi_key_comparator]") {
  cmp::multi_key_comparator by_name_age{&Person::name, &Person::age};

  Person alice30{"Alice", 30, 1.65};
  Person alice30_copy{"Alice", 30, 1.70}; // different height, same name/age

  REQUIRE(by_name_age.compare(alice30, alice30_copy) == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// make_comparator tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("make_comparator with single projection", "[comparator][make_comparator]") {
  auto by_age = cmp::make_comparator(&Person::age);

  Person alice{"Alice", 30, 1.65};
  Person bob{"Bob", 25, 1.80};

  REQUIRE(by_age(bob, alice));
}

TEST_CASE("make_comparator with multiple projections", "[comparator][make_comparator]") {
  auto by_name_age = cmp::make_comparator(&Person::name, &Person::age);

  Person alice25{"Alice", 25, 1.65};
  Person alice30{"Alice", 30, 1.65};

  REQUIRE(by_name_age(alice25, alice30));
}

// ─────────────────────────────────────────────────────────────────────────────
// reverse_comparator tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("reverse_comparator reverses ordering", "[comparator][reverse_comparator]") {
  cmp::reverse_comparator descending{cmp::key_comparator{&Person::age}};

  Person alice{"Alice", 30, 1.65};
  Person bob{"Bob", 25, 1.80};

  // Normal: bob < alice (25 < 30)
  // Reversed: alice < bob
  REQUIRE(descending(alice, bob));
  REQUIRE_FALSE(descending(bob, alice));
}

TEST_CASE("reverse_comparator in std::sort for descending order",
          "[comparator][reverse_comparator]") {
  std::vector<Person> people{{"Bob", 25, 1.80}, {"Alice", 30, 1.65}, {"Charlie", 20, 1.75}};

  std::sort(people.begin(), people.end(),
            cmp::reverse_comparator{cmp::key_comparator{&Person::age}});

  REQUIRE(people[0].name == "Alice");   // age 30 (highest)
  REQUIRE(people[1].name == "Bob");     // age 25
  REQUIRE(people[2].name == "Charlie"); // age 20 (lowest)
}

TEST_CASE("make_reverse_comparator convenience function", "[comparator][reverse_comparator]") {
  auto descending_by_age = cmp::make_reverse_comparator(&Person::age);

  std::vector<Person> people{{"Bob", 25, 1.80}, {"Alice", 30, 1.65}, {"Charlie", 20, 1.75}};
  std::sort(people.begin(), people.end(), descending_by_age);

  REQUIRE(people[0].age == 30);
  REQUIRE(people[1].age == 25);
  REQUIRE(people[2].age == 20);
}

// ─────────────────────────────────────────────────────────────────────────────
// equal_by tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("equal_by with single projection", "[comparator][equal_by]") {
  Person alice{"Alice", 30, 1.65};
  Person alice_taller{"Alice", 30, 1.75};
  Person bob{"Bob", 30, 1.65};

  REQUIRE(cmp::equal_by(alice, alice_taller, &Person::name));
  REQUIRE_FALSE(cmp::equal_by(alice, bob, &Person::name));
}

TEST_CASE("equal_by with multiple projections", "[comparator][equal_by]") {
  Person alice{"Alice", 30, 1.65};
  Person alice_taller{"Alice", 30, 1.75};
  Person alice_older{"Alice", 35, 1.65};

  // Same name and age, different height
  REQUIRE(cmp::equal_by(alice, alice_taller, &Person::name, &Person::age));

  // Same name, different age
  REQUIRE_FALSE(cmp::equal_by(alice, alice_older, &Person::name, &Person::age));
}

// ─────────────────────────────────────────────────────────────────────────────
// Key extraction utilities tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("make_comparison_key extracts tuple", "[comparator][key_extraction]") {
  Person alice{"Alice", 30, 1.65};

  auto key = cmp::make_comparison_key(alice, &Person::name, &Person::age);

  REQUIRE(std::get<0>(key) == "Alice");
  REQUIRE(std::get<1>(key) == 30);
}

TEST_CASE("key_extractor creates reusable key function", "[comparator][key_extraction]") {
  auto get_key = cmp::key_extractor(&Person::name, &Person::age);

  Person alice{"Alice", 30, 1.65};
  Person bob{"Bob", 25, 1.80};

  auto alice_key = get_key(alice);
  auto bob_key = get_key(bob);

  REQUIRE(alice_key < bob_key); // ("Alice", 30) < ("Bob", 25)
}

// ─────────────────────────────────────────────────────────────────────────────
// Comparison category propagation tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("compare_by propagates strong_ordering", "[comparator][ordering]") {
  Point p1{1, 2};
  Point p2{2, 3};

  auto result = cmp::compare_by(p1, p2, &Point::x);
  STATIC_REQUIRE(std::same_as<decltype(result), std::strong_ordering>);
}

TEST_CASE("multi_key_comparator preserves ordering category", "[comparator][ordering]") {
  cmp::multi_key_comparator comp{&Person::age, &Person::name};

  Person p1{"Alice", 30, 1.65};
  Person p2{"Bob", 25, 1.80};

  auto result = comp.compare(p1, p2);
  STATIC_REQUIRE(std::same_as<decltype(result), std::strong_ordering>);
}

// ─────────────────────────────────────────────────────────────────────────────
// constexpr tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("compare_by is constexpr", "[comparator][constexpr]") {
  constexpr Point p1{1, 2};
  constexpr Point p2{2, 1};

  constexpr auto result = cmp::compare_by(p1, p2, &Point::x);
  STATIC_REQUIRE(result < 0);
}

TEST_CASE("STRAYLIGHT_DEFINE_COMPARISON generates constexpr operators", "[comparator][constexpr]") {
  constexpr Point p1{1, 2};
  constexpr Point p2{1, 2};
  constexpr Point p3{2, 1};

  STATIC_REQUIRE(p1 == p2);
  STATIC_REQUIRE(p1 != p3);
  STATIC_REQUIRE((p1 <=> p3) < 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Edge cases and special scenarios
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("compare_by with empty strings", "[comparator][edge_cases]") {
  Person empty{"", 30, 1.65};
  Person nonempty{"Alice", 30, 1.65};

  auto result = cmp::compare_by(empty, nonempty, &Person::name);
  REQUIRE(result < 0); // "" < "Alice"
}

TEST_CASE("comparators handle self-comparison", "[comparator][edge_cases]") {
  Person alice{"Alice", 30, 1.65};

  // Self-comparison should be equal
  REQUIRE(cmp::compare_by(alice, alice, &Person::name) == 0);
  REQUIRE(cmp::compare_by(alice, alice, &Person::name, &Person::age) == 0);
  REQUIRE(cmp::equal_by(alice, alice, &Person::name, &Person::age, &Person::height));
}

TEST_CASE("key_comparator handles equal values correctly", "[comparator][edge_cases]") {
  cmp::key_comparator by_age{&Person::age};

  Person alice{"Alice", 30, 1.65};
  Person bob{"Bob", 30, 1.80}; // same age

  // Neither is less than the other
  REQUIRE_FALSE(by_age(alice, bob));
  REQUIRE_FALSE(by_age(bob, alice));
}

TEST_CASE("comparison with nested member access", "[comparator][advanced]") {
  struct Inner {
    int value;
    auto operator<=>(const Inner&) const = default;
  };

  struct Outer {
    Inner inner;
    std::string name;
  };

  auto get_inner_value = [](const Outer& o) { return o.inner.value; };

  Outer a{{10}, "A"};
  Outer b{{20}, "B"};

  auto result = cmp::compare_by(a, b, get_inner_value);
  REQUIRE(result < 0);
}

TEST_CASE("comparator with const member functions", "[comparator][advanced]") {
  struct Widget {
    int value_;

    [[nodiscard]] int value() const { return value_; }
  };

  Widget w1{10};
  Widget w2{20};

  auto result = cmp::compare_by(w1, w2, &Widget::value);
  REQUIRE(result < 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Integration with standard library
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("key_comparator works with std::set", "[comparator][stdlib]") {
  std::set<Person, cmp::key_comparator<int Person::*>> by_age_set{
      cmp::key_comparator{&Person::age}};

  by_age_set.insert({"Alice", 30, 1.65});
  by_age_set.insert({"Bob", 25, 1.80});
  by_age_set.insert({"Charlie", 30, 1.75}); // same age as Alice

  // Only 2 elements because age 30 is a duplicate
  REQUIRE(by_age_set.size() == 2);
}

TEST_CASE("comparators work with std::ranges::sort", "[comparator][stdlib]") {
  std::vector<Person> people{{"Bob", 25, 1.80}, {"Alice", 30, 1.65}, {"Charlie", 20, 1.75}};

  std::ranges::sort(people, cmp::make_comparator(&Person::age));

  REQUIRE(people[0].age == 20);
  REQUIRE(people[1].age == 25);
  REQUIRE(people[2].age == 30);
}

TEST_CASE("comparators work with std::ranges::max_element", "[comparator][stdlib]") {
  std::vector<Person> people{{"Bob", 25, 1.80}, {"Alice", 30, 1.65}, {"Charlie", 20, 1.75}};

  auto oldest = std::ranges::max_element(people, cmp::make_comparator(&Person::age));

  REQUIRE(oldest->name == "Alice");
  REQUIRE(oldest->age == 30);
}

TEST_CASE("comparators work with std::ranges::min_element", "[comparator][stdlib]") {
  std::vector<Person> people{{"Bob", 25, 1.80}, {"Alice", 30, 1.65}, {"Charlie", 20, 1.75}};

  auto youngest = std::ranges::min_element(people, cmp::make_comparator(&Person::age));

  REQUIRE(youngest->name == "Charlie");
  REQUIRE(youngest->age == 20);
}
