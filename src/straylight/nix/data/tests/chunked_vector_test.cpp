// straylight::nix::data::ChunkedVector tests
//
// Property-based testing with rapidcheck for chunked vector primitives.
// Tests stable reference guarantees, iterator correctness, and container semantics.

// Catch2 MUST be included before rapidcheck/catch.h for v3 compatibility
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include "../chunked_vector.h"

namespace chunked_vector = straylight::nix::data;

// Use a smaller chunk size for testing to exercise chunk boundaries
using TestVector = chunked_vector::ChunkedVector<int, 16>;
using TestVectorLarge = chunked_vector::ChunkedVector<int, 1024>;
using TestVectorString = chunked_vector::ChunkedVector<std::string, 8>;

// ─────────────────────────────────────────────────────────────────────────────
// Basic Construction Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ChunkedVector default construction", "[chunked_vector][construction]") {
  TestVector vec;
  REQUIRE(vec.size() == 0);
  REQUIRE(vec.empty());
  REQUIRE(vec.capacity() == 0);
  REQUIRE(vec.chunk_count() == 0);
}

TEST_CASE("ChunkedVector with reserve", "[chunked_vector][construction]") {
  TestVector vec(4); // Reserve 4 chunks
  REQUIRE(vec.size() == 0);
  REQUIRE(vec.empty());
  REQUIRE(vec.capacity() == 0); // No chunks allocated yet
}

TEST_CASE("ChunkedVector chunk_size constant", "[chunked_vector][construction]") {
  REQUIRE(TestVector::chunk_size() == 16);
  REQUIRE(TestVectorLarge::chunk_size() == 1024);
  REQUIRE(TestVectorString::chunk_size() == 8);
}

// ─────────────────────────────────────────────────────────────────────────────
// Push/Emplace Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ChunkedVector push_back single element", "[chunked_vector][push]") {
  TestVector vec;
  vec.push_back(42);

  REQUIRE(vec.size() == 1);
  REQUIRE(!vec.empty());
  REQUIRE(vec[0] == 42);
  REQUIRE(vec.front() == 42);
  REQUIRE(vec.back() == 42);
  REQUIRE(vec.chunk_count() == 1);
  REQUIRE(vec.capacity() == 16);
}

TEST_CASE("ChunkedVector push_back multiple elements", "[chunked_vector][push]") {
  TestVector vec;

  for (int i = 0; i < 100; ++i) {
    vec.push_back(i);
  }

  REQUIRE(vec.size() == 100);
  REQUIRE(vec.chunk_count() == 7); // 100 / 16 = 6.25 -> 7 chunks

  for (int i = 0; i < 100; ++i) {
    REQUIRE(vec[static_cast<std::size_t>(i)] == i);
  }
}

TEST_CASE("ChunkedVector emplace_back", "[chunked_vector][push]") {
  TestVectorString vec;
  vec.emplace_back("hello");
  vec.emplace_back("world");
  vec.emplace_back(5, 'x');

  REQUIRE(vec.size() == 3);
  REQUIRE(vec[0] == "hello");
  REQUIRE(vec[1] == "world");
  REQUIRE(vec[2] == "xxxxx");
}

TEST_CASE("ChunkedVector add (Nix API compatibility)", "[chunked_vector][push]") {
  TestVector vec;

  auto [ref1, idx1] = vec.add(10);
  auto [ref2, idx2] = vec.add(20);
  auto [ref3, idx3] = vec.add(30);

  REQUIRE(idx1 == 0);
  REQUIRE(idx2 == 1);
  REQUIRE(idx3 == 2);
  REQUIRE(ref1 == 10);
  REQUIRE(ref2 == 20);
  REQUIRE(ref3 == 30);

  // Modify through reference
  ref1 = 100;
  REQUIRE(vec[0] == 100);
}

// ─────────────────────────────────────────────────────────────────────────────
// Stable Reference Tests (Critical Property)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ChunkedVector stable references", "[chunked_vector][stable]") {
  TestVector vec;

  // Add first element and save pointer
  vec.push_back(1);
  int* ptr1 = &vec[0];

  // Add many more elements (causing multiple chunk allocations)
  for (int i = 2; i <= 1000; ++i) {
    vec.push_back(i);
  }

  // Original pointer must still be valid and point to same value
  REQUIRE(*ptr1 == 1);
  REQUIRE(ptr1 == &vec[0]);
}

TEST_CASE("ChunkedVector stable references across chunks", "[chunked_vector][stable]") {
  TestVector vec;
  std::vector<int*> pointers;

  // Add elements and save all pointers
  for (int i = 0; i < 100; ++i) {
    vec.push_back(i);
    pointers.push_back(&vec[static_cast<std::size_t>(i)]);
  }

  // Add more elements to trigger more allocations
  for (int i = 100; i < 1000; ++i) {
    vec.push_back(i);
  }

  // All original pointers must still be valid
  for (int i = 0; i < 100; ++i) {
    REQUIRE(*pointers[static_cast<std::size_t>(i)] == i);
    REQUIRE(pointers[static_cast<std::size_t>(i)] == &vec[static_cast<std::size_t>(i)]);
  }
}

TEST_CASE("ChunkedVector stable reference from add()", "[chunked_vector][stable]") {
  TestVectorString vec;
  std::vector<std::string*> refs;

  // Add elements using add() and save references
  for (int i = 0; i < 50; ++i) {
    auto [ref, idx] = vec.add("element_" + std::to_string(i));
    refs.push_back(&ref);
    REQUIRE(idx == static_cast<std::size_t>(i));
  }

  // Add more elements
  for (int i = 50; i < 200; ++i) {
    vec.add("element_" + std::to_string(i));
  }

  // All references must still be valid
  for (int i = 0; i < 50; ++i) {
    REQUIRE(*refs[static_cast<std::size_t>(i)] == "element_" + std::to_string(i));
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Element Access Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ChunkedVector at() bounds checking", "[chunked_vector][access]") {
  TestVector vec;
  vec.push_back(1);
  vec.push_back(2);

  REQUIRE(vec.at(0) == 1);
  REQUIRE(vec.at(1) == 2);
  REQUIRE_THROWS_AS(vec.at(2), std::out_of_range);
  REQUIRE_THROWS_AS(vec.at(100), std::out_of_range);
}

TEST_CASE("ChunkedVector front() and back()", "[chunked_vector][access]") {
  TestVector vec;
  vec.push_back(10);
  REQUIRE(vec.front() == 10);
  REQUIRE(vec.back() == 10);

  vec.push_back(20);
  vec.push_back(30);
  REQUIRE(vec.front() == 10);
  REQUIRE(vec.back() == 30);

  // Modify through front/back
  vec.front() = 100;
  vec.back() = 300;
  REQUIRE(vec[0] == 100);
  REQUIRE(vec[2] == 300);
}

TEST_CASE("ChunkedVector const access", "[chunked_vector][access]") {
  TestVector vec;
  for (int i = 0; i < 50; ++i) {
    vec.push_back(i);
  }

  const TestVector& cvec = vec;
  REQUIRE(cvec.size() == 50);
  REQUIRE(cvec[25] == 25);
  REQUIRE(cvec.at(25) == 25);
  REQUIRE(cvec.front() == 0);
  REQUIRE(cvec.back() == 49);
}

// ─────────────────────────────────────────────────────────────────────────────
// Iterator Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ChunkedVector iterator basic", "[chunked_vector][iterator]") {
  TestVector vec;
  for (int i = 0; i < 50; ++i) {
    vec.push_back(i);
  }

  // Forward iteration
  int expected = 0;
  for (auto it = vec.begin(); it != vec.end(); ++it) {
    REQUIRE(*it == expected);
    ++expected;
  }
  REQUIRE(expected == 50);
}

TEST_CASE("ChunkedVector range-based for", "[chunked_vector][iterator]") {
  TestVector vec;
  for (int i = 0; i < 50; ++i) {
    vec.push_back(i * 2);
  }

  int expected = 0;
  for (int val : vec) {
    REQUIRE(val == expected * 2);
    ++expected;
  }
  REQUIRE(expected == 50);
}

TEST_CASE("ChunkedVector iterator arithmetic", "[chunked_vector][iterator]") {
  TestVector vec;
  for (int i = 0; i < 100; ++i) {
    vec.push_back(i);
  }

  auto it = vec.begin();

  // Jump forward
  it += 50;
  REQUIRE(*it == 50);

  // Jump backward
  it -= 25;
  REQUIRE(*it == 25);

  // Iterator difference
  auto diff = vec.end() - vec.begin();
  REQUIRE(diff == 100);

  // Comparison
  REQUIRE(vec.begin() < vec.end());
  REQUIRE(vec.end() > vec.begin());
  REQUIRE(vec.begin() <= vec.begin());
  REQUIRE(vec.begin() >= vec.begin());
}

TEST_CASE("ChunkedVector iterator subscript", "[chunked_vector][iterator]") {
  TestVector vec;
  for (int i = 0; i < 100; ++i) {
    vec.push_back(i);
  }

  auto it = vec.begin();
  for (int i = 0; i < 100; ++i) {
    REQUIRE(it[i] == i);
  }
}

TEST_CASE("ChunkedVector reverse iterator", "[chunked_vector][iterator]") {
  TestVector vec;
  for (int i = 0; i < 50; ++i) {
    vec.push_back(i);
  }

  int expected = 49;
  for (auto it = vec.rbegin(); it != vec.rend(); ++it) {
    REQUIRE(*it == expected);
    --expected;
  }
  REQUIRE(expected == -1);
}

TEST_CASE("ChunkedVector const iterator", "[chunked_vector][iterator]") {
  TestVector vec;
  for (int i = 0; i < 50; ++i) {
    vec.push_back(i);
  }

  const TestVector& cvec = vec;
  int expected = 0;
  for (auto it = cvec.begin(); it != cvec.end(); ++it) {
    REQUIRE(*it == expected);
    ++expected;
  }

  // cbegin/cend
  expected = 0;
  for (auto it = vec.cbegin(); it != vec.cend(); ++it) {
    REQUIRE(*it == expected);
    ++expected;
  }
}

TEST_CASE("ChunkedVector iterator with STL algorithms", "[chunked_vector][iterator]") {
  TestVector vec;
  for (int i = 0; i < 100; ++i) {
    vec.push_back(i);
  }

  // std::find
  auto it = std::find(vec.begin(), vec.end(), 50);
  REQUIRE(it != vec.end());
  REQUIRE(*it == 50);

  // std::count_if
  auto count = std::count_if(vec.begin(), vec.end(), [](int x) { return x % 2 == 0; });
  REQUIRE(count == 50);

  // std::accumulate
  auto sum = std::accumulate(vec.begin(), vec.end(), 0);
  REQUIRE(sum == 4950); // 0+1+2+...+99 = 99*100/2 = 4950

  // std::transform (modify in place)
  std::transform(vec.begin(), vec.end(), vec.begin(), [](int x) { return x * 2; });
  REQUIRE(vec[0] == 0);
  REQUIRE(vec[1] == 2);
  REQUIRE(vec[50] == 100);
}

TEST_CASE("ChunkedVector iterator across chunk boundaries", "[chunked_vector][iterator]") {
  TestVector vec; // chunk size = 16
  for (int i = 0; i < 64; ++i) {
    vec.push_back(i);
  }

  // Verify iteration crosses all 4 chunks correctly
  auto it = vec.begin();
  for (int i = 0; i < 64; ++i) {
    REQUIRE(*it == i);
    ++it;
  }
  REQUIRE(it == vec.end());

  // Verify jumping across chunk boundaries
  it = vec.begin();
  it += 15; // Last in first chunk
  REQUIRE(*it == 15);
  ++it; // First in second chunk
  REQUIRE(*it == 16);

  it += 16; // Jump to third chunk
  REQUIRE(*it == 32);
}

// ─────────────────────────────────────────────────────────────────────────────
// Capacity Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ChunkedVector capacity grows by chunks", "[chunked_vector][capacity]") {
  TestVector vec; // chunk size = 16

  REQUIRE(vec.capacity() == 0);

  vec.push_back(1);
  REQUIRE(vec.capacity() == 16);
  REQUIRE(vec.chunk_count() == 1);

  for (int i = 2; i <= 16; ++i) {
    vec.push_back(i);
  }
  REQUIRE(vec.capacity() == 16);
  REQUIRE(vec.chunk_count() == 1);

  vec.push_back(17);
  REQUIRE(vec.capacity() == 32);
  REQUIRE(vec.chunk_count() == 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// Clear and Shrink Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ChunkedVector clear", "[chunked_vector][clear]") {
  TestVector vec;
  for (int i = 0; i < 100; ++i) {
    vec.push_back(i);
  }

  vec.clear();
  REQUIRE(vec.size() == 0);
  REQUIRE(vec.empty());
  // Chunks are retained after clear
  REQUIRE(vec.chunk_count() > 0);
  REQUIRE(vec.capacity() > 0);
}

TEST_CASE("ChunkedVector clear with destructors", "[chunked_vector][clear]") {
  static int destruct_count = 0;
  destruct_count = 0;

  struct Counter {
    Counter() = default;
    Counter(const Counter&) = default;
    Counter& operator=(const Counter&) = default;
    ~Counter() { ++destruct_count; }
  };

  {
    chunked_vector::ChunkedVector<Counter, 8> vec;
    for (int i = 0; i < 25; ++i) {
      vec.emplace_back();
    }
    REQUIRE(destruct_count == 0);
    vec.clear();
    REQUIRE(destruct_count == 25);
  }
  // Additional destructs from chunk destruction (but no elements to destroy)
}

TEST_CASE("ChunkedVector shrink_to_fit", "[chunked_vector][clear]") {
  TestVector vec;
  for (int i = 0; i < 100; ++i) {
    vec.push_back(i);
  }

  vec.shrink_to_fit();
  REQUIRE(vec.size() == 0);
  REQUIRE(vec.empty());
  REQUIRE(vec.capacity() == 0);
  REQUIRE(vec.chunk_count() == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Move Semantics Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ChunkedVector move construction", "[chunked_vector][move]") {
  TestVector vec1;
  for (int i = 0; i < 50; ++i) {
    vec1.push_back(i);
  }
  int* ptr = &vec1[25];

  TestVector vec2(std::move(vec1));
  REQUIRE(vec2.size() == 50);
  REQUIRE(vec2[25] == 25);
  REQUIRE(&vec2[25] == ptr); // Same memory location
  REQUIRE(vec1.size() == 0); // NOLINT: testing moved-from state
  REQUIRE(vec1.empty());     // NOLINT: testing moved-from state
}

TEST_CASE("ChunkedVector move assignment", "[chunked_vector][move]") {
  TestVector vec1;
  for (int i = 0; i < 50; ++i) {
    vec1.push_back(i);
  }

  TestVector vec2;
  vec2.push_back(999);

  vec2 = std::move(vec1);
  REQUIRE(vec2.size() == 50);
  REQUIRE(vec2[0] == 0);
  REQUIRE(vec1.size() == 0); // NOLINT: testing moved-from state
}

// ─────────────────────────────────────────────────────────────────────────────
// forEach Tests (Nix API Compatibility)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ChunkedVector forEach", "[chunked_vector][foreach]") {
  TestVector vec;
  for (int i = 0; i < 50; ++i) {
    vec.push_back(i);
  }

  int sum = 0;
  vec.forEach([&sum](int x) { sum += x; });
  REQUIRE(sum == 1225); // 0+1+2+...+49 = 49*50/2

  // Const forEach
  const TestVector& cvec = vec;
  int sum2 = 0;
  cvec.forEach([&sum2](int x) { sum2 += x; });
  REQUIRE(sum2 == 1225);
}

TEST_CASE("ChunkedVector forEach mutable", "[chunked_vector][foreach]") {
  TestVector vec;
  for (int i = 0; i < 50; ++i) {
    vec.push_back(i);
  }

  vec.forEach([](int& x) { x *= 2; });

  for (int i = 0; i < 50; ++i) {
    REQUIRE(vec[static_cast<std::size_t>(i)] == i * 2);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Property-Based Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ChunkedVector push_back preserves order", "[chunked_vector][property]") {
  rc::prop("elements are stored in push order", []() {
    auto values = *rc::gen::container<std::vector<int>>(rc::gen::arbitrary<int>());

    TestVector vec;
    for (int v : values) {
      vec.push_back(v);
    }

    RC_ASSERT(vec.size() == values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
      RC_ASSERT(vec[i] == values[i]);
    }
  });
}

TEST_CASE("ChunkedVector stable references property", "[chunked_vector][property][stable]") {
  rc::prop("pointers remain valid after push_back", []() {
    auto initial_count = *rc::gen::inRange<std::size_t>(1, 100);
    auto additional_count = *rc::gen::inRange<std::size_t>(0, 500);

    TestVector vec;
    std::vector<int*> pointers;

    // Add initial elements and save pointers
    for (std::size_t i = 0; i < initial_count; ++i) {
      vec.push_back(static_cast<int>(i));
      pointers.push_back(&vec[i]);
    }

    // Add more elements
    for (std::size_t i = 0; i < additional_count; ++i) {
      vec.push_back(static_cast<int>(initial_count + i));
    }

    // Verify all original pointers are still valid
    for (std::size_t i = 0; i < initial_count; ++i) {
      RC_ASSERT(*pointers[i] == static_cast<int>(i));
      RC_ASSERT(pointers[i] == &vec[i]);
    }
  });
}

TEST_CASE("ChunkedVector iterator distance property", "[chunked_vector][property][iterator]") {
  rc::prop("iterator distance equals size", []() {
    auto count = *rc::gen::inRange<std::size_t>(0, 1000);

    TestVector vec;
    for (std::size_t i = 0; i < count; ++i) {
      vec.push_back(static_cast<int>(i));
    }

    auto dist = std::distance(vec.begin(), vec.end());
    RC_ASSERT(static_cast<std::size_t>(dist) == count);
    RC_ASSERT(vec.size() == count);
  });
}

TEST_CASE("ChunkedVector iterator traversal property", "[chunked_vector][property][iterator]") {
  rc::prop("forward iteration visits all elements in order", []() {
    auto values = *rc::gen::container<std::vector<int>>(rc::gen::arbitrary<int>());

    TestVector vec;
    for (int v : values) {
      vec.push_back(v);
    }

    std::size_t idx = 0;
    for (auto it = vec.begin(); it != vec.end(); ++it) {
      RC_ASSERT(*it == values[idx]);
      ++idx;
    }
    RC_ASSERT(idx == values.size());
  });
}

TEST_CASE("ChunkedVector reverse iterator property", "[chunked_vector][property][iterator]") {
  rc::prop("reverse iteration visits all elements in reverse order", []() {
    auto values = *rc::gen::container<std::vector<int>>(rc::gen::arbitrary<int>());

    TestVector vec;
    for (int v : values) {
      vec.push_back(v);
    }

    std::size_t idx = values.size();
    for (auto it = vec.rbegin(); it != vec.rend(); ++it) {
      --idx;
      RC_ASSERT(*it == values[idx]);
    }
    RC_ASSERT(idx == 0);
  });
}

TEST_CASE("ChunkedVector iterator random access property", "[chunked_vector][property][iterator]") {
  rc::prop("iterator[n] equals *(iterator + n)", []() {
    auto count = *rc::gen::inRange<std::size_t>(1, 500);

    TestVector vec;
    for (std::size_t i = 0; i < count; ++i) {
      vec.push_back(static_cast<int>(i));
    }

    auto offset = *rc::gen::inRange<std::size_t>(0, count);
    auto it = vec.begin();

    RC_ASSERT(it[static_cast<std::ptrdiff_t>(offset)] ==
              *(it + static_cast<std::ptrdiff_t>(offset)));
    RC_ASSERT(it[static_cast<std::ptrdiff_t>(offset)] == static_cast<int>(offset));
  });
}

TEST_CASE("ChunkedVector capacity property", "[chunked_vector][property][capacity]") {
  rc::prop("capacity >= size and is multiple of chunk_size", []() {
    auto count = *rc::gen::inRange<std::size_t>(0, 1000);

    TestVector vec;
    for (std::size_t i = 0; i < count; ++i) {
      vec.push_back(static_cast<int>(i));
    }

    RC_ASSERT(vec.capacity() >= vec.size());
    if (vec.capacity() > 0) {
      RC_ASSERT(vec.capacity() % TestVector::chunk_size() == 0);
    }
    RC_ASSERT(vec.chunk_count() == (vec.capacity() / TestVector::chunk_size()));
  });
}

TEST_CASE("ChunkedVector clear property", "[chunked_vector][property][clear]") {
  rc::prop("clear sets size to zero but retains capacity", []() {
    auto count = *rc::gen::inRange<std::size_t>(0, 500);

    TestVector vec;
    for (std::size_t i = 0; i < count; ++i) {
      vec.push_back(static_cast<int>(i));
    }

    auto old_capacity = vec.capacity();
    auto old_chunk_count = vec.chunk_count();

    vec.clear();

    RC_ASSERT(vec.size() == 0);
    RC_ASSERT(vec.empty());
    RC_ASSERT(vec.capacity() == old_capacity);
    RC_ASSERT(vec.chunk_count() == old_chunk_count);
  });
}

TEST_CASE("ChunkedVector add returns correct indices", "[chunked_vector][property]") {
  rc::prop("add() returns sequential indices", []() {
    auto count = *rc::gen::inRange<std::size_t>(0, 500);

    TestVector vec;
    for (std::size_t i = 0; i < count; ++i) {
      auto [ref, idx] = vec.add(static_cast<int>(i * 10));
      RC_ASSERT(idx == i);
      RC_ASSERT(ref == static_cast<int>(i * 10));
      RC_ASSERT(&ref == &vec[i]);
    }
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Edge Cases
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ChunkedVector empty iterator range", "[chunked_vector][edge]") {
  TestVector vec;
  REQUIRE(vec.begin() == vec.end());
  REQUIRE(vec.rbegin() == vec.rend());
  REQUIRE(vec.cbegin() == vec.cend());

  std::size_t count = 0;
  for ([[maybe_unused]] int v : vec) {
    ++count;
  }
  REQUIRE(count == 0);
}

TEST_CASE("ChunkedVector single element", "[chunked_vector][edge]") {
  TestVector vec;
  vec.push_back(42);

  REQUIRE(vec.size() == 1);
  REQUIRE(vec.begin() != vec.end());
  REQUIRE(*vec.begin() == 42);
  REQUIRE(std::distance(vec.begin(), vec.end()) == 1);

  auto it = vec.begin();
  ++it;
  REQUIRE(it == vec.end());
}

TEST_CASE("ChunkedVector chunk boundary exact", "[chunked_vector][edge]") {
  TestVector vec; // chunk size = 16

  // Fill exactly one chunk
  for (int i = 0; i < 16; ++i) {
    vec.push_back(i);
  }
  REQUIRE(vec.chunk_count() == 1);
  REQUIRE(vec.capacity() == 16);

  // Add one more to trigger new chunk
  vec.push_back(16);
  REQUIRE(vec.chunk_count() == 2);
  REQUIRE(vec.capacity() == 32);

  // Verify all elements
  for (int i = 0; i <= 16; ++i) {
    REQUIRE(vec[static_cast<std::size_t>(i)] == i);
  }
}

TEST_CASE("ChunkedVector with non-trivial type", "[chunked_vector][edge]") {
  struct NonTrivial {
    std::string data;
    std::unique_ptr<int> ptr;

    NonTrivial() : data("default"), ptr(std::make_unique<int>(0)) {}
    explicit NonTrivial(std::string s) : data(std::move(s)), ptr(std::make_unique<int>(42)) {}
    NonTrivial(NonTrivial&&) = default;
    NonTrivial& operator=(NonTrivial&&) = default;
    ~NonTrivial() = default;

    // Non-copyable
    NonTrivial(const NonTrivial&) = delete;
    NonTrivial& operator=(const NonTrivial&) = delete;
  };

  chunked_vector::ChunkedVector<NonTrivial, 4> vec;
  vec.emplace_back("first");
  vec.emplace_back("second");
  vec.emplace_back("third");

  REQUIRE(vec[0].data == "first");
  REQUIRE(vec[1].data == "second");
  REQUIRE(vec[2].data == "third");
  REQUIRE(*vec[0].ptr == 42);
}

// ─────────────────────────────────────────────────────────────────────────────
// Large Scale Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ChunkedVector large scale", "[chunked_vector][large]") {
  TestVectorLarge vec;
  constexpr int count = 100000;

  for (int i = 0; i < count; ++i) {
    vec.push_back(i);
  }

  REQUIRE(vec.size() == count);
  REQUIRE(vec.chunk_count() == 98); // 100000 / 1024 = 97.65... -> 98

  // Verify some values
  REQUIRE(vec[0] == 0);
  REQUIRE(vec[50000] == 50000);
  REQUIRE(vec[99999] == 99999);

  // Verify iteration
  int sum = 0;
  for (int v : vec) {
    sum += v % 100; // Just sum remainders to avoid overflow
  }
  REQUIRE(sum > 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Stress Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ChunkedVector stress test mixed operations", "[chunked_vector][stress]") {
  rc::prop("mixed operations maintain invariants", []() {
    TestVector vec;
    std::vector<int> expected;

    auto ops = *rc::gen::inRange<int>(0, 200);
    for (int i = 0; i < ops; ++i) {
      int val = *rc::gen::arbitrary<int>();
      vec.push_back(val);
      expected.push_back(val);

      RC_ASSERT(vec.size() == expected.size());
      RC_ASSERT(vec.back() == expected.back());
    }

    // Verify all elements
    for (std::size_t i = 0; i < expected.size(); ++i) {
      RC_ASSERT(vec[i] == expected[i]);
    }

    // Verify iteration
    auto it = vec.begin();
    for (std::size_t i = 0; i < expected.size(); ++i) {
      RC_ASSERT(*it == expected[i]);
      ++it;
    }
    RC_ASSERT(it == vec.end());
  });
}

TEST_CASE("ChunkedVector fuzz test", "[chunked_vector][fuzz]") {
  rc::prop("arbitrary operations don't crash", []() {
    TestVector vec;

    auto ops = *rc::gen::inRange<int>(0, 1000);
    for (int i = 0; i < ops; ++i) {
      vec.push_back(*rc::gen::arbitrary<int>());
    }

    // Clear and refill
    vec.clear();
    RC_ASSERT(vec.empty());

    ops = *rc::gen::inRange<int>(0, 500);
    for (int i = 0; i < ops; ++i) {
      vec.emplace_back(*rc::gen::arbitrary<int>());
    }

    RC_ASSERT(vec.size() == static_cast<std::size_t>(ops));
  });
}
