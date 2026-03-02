// straylight::nix::primitives::comparator - Three-way comparison utilities
//
// Modern C++20/23 comparison utilities replacing nix/util/comparator.h.
// Provides:
//   - Concepts for three-way comparable types
//   - STRAYLIGHT_DEFINE_COMPARISON macro for generating operator<=> and operator==
//   - compare_by() function template for creating custom comparators
//   - Helper functions for member-based comparison

#pragma once

#include <compare>
#include <concepts>
#include <functional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace straylight::nix::util {

// ─────────────────────────────────────────────────────────────────────────────
// Comparison category traits
// ─────────────────────────────────────────────────────────────────────────────

/// Check if a type is a comparison category (strong_ordering, weak_ordering, partial_ordering).
template <typename T>
concept comparison_category =
    std::same_as<T, std::strong_ordering> || std::same_as<T, std::weak_ordering> ||
    std::same_as<T, std::partial_ordering>;

/// Determine the common comparison category for multiple types.
/// Used to compute the result type when comparing heterogeneous tuples.
template <typename... Categories>
  requires(comparison_category<Categories> && ...)
using common_comparison_category_t = std::common_comparison_category_t<Categories...>;

// ─────────────────────────────────────────────────────────────────────────────
// Concepts for three-way comparison
// ─────────────────────────────────────────────────────────────────────────────

/// Concept for types that support three-way comparison via a key function.
///
/// A type T satisfies this concept if:
///   - KeyFunc is invocable with const T&
///   - The result of KeyFunc is three-way comparable with itself
///
/// Usage:
///   template <three_way_comparable_with_key<decltype(my_key)> T>
///   auto compare(const T& a, const T& b) { ... }
template <typename T, typename KeyFunc>
concept three_way_comparable_with_key =
    std::invocable<KeyFunc, const T&> &&
    std::three_way_comparable<std::invoke_result_t<KeyFunc, const T&>>;

/// Concept for types that can be compared via projection.
///
/// A type T satisfies this concept if:
///   - Projection is invocable with const T&
///   - The result of Projection supports <=>
template <typename T, typename Projection>
concept projectable_for_comparison =
    std::invocable<Projection, const T&> && requires(const T& value, Projection projection) {
      { std::invoke(projection, value) <=> std::invoke(projection, value) };
    };

/// Concept for types that are naturally three-way comparable.
template <typename T>
concept naturally_three_way_comparable = std::three_way_comparable<T>;

/// Concept for types that have both operator<=> and operator==.
template <typename T>
concept fully_ordered = std::three_way_comparable<T> && std::equality_comparable<T>;

// ─────────────────────────────────────────────────────────────────────────────
// Key extraction utilities
// ─────────────────────────────────────────────────────────────────────────────

/// Extract a comparison key from an object using multiple member projections.
///
/// Usage:
///   auto key = make_comparison_key(my_object, &MyClass::field1, &MyClass::field2);
///   // key is std::tuple<Field1Type, Field2Type>
template <typename T, typename... Projections>
  requires(std::invocable<Projections, const T&> && ...)
[[nodiscard]] constexpr auto make_comparison_key(const T& object, Projections&&... projections) {
  return std::tuple{std::invoke(std::forward<Projections>(projections), object)...};
}

/// Create a key extractor function from multiple projections.
///
/// Usage:
///   auto key_func = key_extractor(&MyClass::name, &MyClass::age);
///   auto key = key_func(my_object);
template <typename... Projections>
[[nodiscard]] constexpr auto key_extractor(Projections&&... projections) {
  return [... projs = std::forward<Projections>(projections)]<typename T>(const T& object) {
    return std::tuple{std::invoke(projs, object)...};
  };
}

// ─────────────────────────────────────────────────────────────────────────────
// Comparison result type deduction
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

/// Helper to get the comparison category from a single type.
template <typename T>
struct comparison_category_for {
  using type = std::compare_three_way_result_t<T, T>;
};

template <typename T>
using comparison_category_for_t = typename comparison_category_for<T>::type;

/// Helper to get the common comparison category from a tuple of types.
template <typename Tuple>
struct tuple_comparison_category;

template <typename... Ts>
struct tuple_comparison_category<std::tuple<Ts...>> {
  using type = std::common_comparison_category_t<std::compare_three_way_result_t<Ts, Ts>...>;
};

template <typename Tuple>
using tuple_comparison_category_t = typename tuple_comparison_category<Tuple>::type;

/// Helper to invoke a projection and get its result type.
template <typename T, typename Projection>
using projected_t = std::invoke_result_t<Projection, const T&>;

/// Compute the comparison category when comparing via projections.
template <typename T, typename... Projections>
using projections_comparison_category_t = std::common_comparison_category_t<
    std::compare_three_way_result_t<projected_t<T, Projections>, projected_t<T, Projections>>...>;

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// compare_by - Create comparators from projections
// ─────────────────────────────────────────────────────────────────────────────

/// Compare two values by applying a projection/key function.
///
/// Usage:
///   auto result = compare_by(person1, person2, &Person::age);
///   if (result < 0) { /* person1 is younger */ }
template <typename T, typename Projection>
  requires projectable_for_comparison<T, Projection>
[[nodiscard]] constexpr auto compare_by(const T& lhs, const T& rhs, Projection&& projection)
    -> std::compare_three_way_result_t<detail::projected_t<T, Projection>,
                                       detail::projected_t<T, Projection>> {
  return std::invoke(std::forward<Projection>(projection), lhs) <=>
         std::invoke(std::forward<Projection>(projection), rhs);
}

/// Compare two values by applying multiple projections in order.
///
/// Comparison proceeds left-to-right through projections. If a projection
/// gives a non-equal result, that result is returned immediately.
///
/// Usage:
///   auto result = compare_by(p1, p2, &Person::last_name, &Person::first_name, &Person::age);
template <typename T, typename Projection, typename... MoreProjections>
  requires projectable_for_comparison<T, Projection> &&
           (projectable_for_comparison<T, MoreProjections> && ...)
[[nodiscard]] constexpr auto compare_by(const T& lhs, const T& rhs, Projection&& projection,
                                        MoreProjections&&... more_projections)
    -> detail::projections_comparison_category_t<T, Projection, MoreProjections...> {
  if (auto result = compare_by(lhs, rhs, std::forward<Projection>(projection)); result != 0) {
    return result;
  }
  return compare_by(lhs, rhs, std::forward<MoreProjections>(more_projections)...);
}

// ─────────────────────────────────────────────────────────────────────────────
// Comparator function objects
// ─────────────────────────────────────────────────────────────────────────────

/// A comparator that uses a key function for ordering.
///
/// The key function is called on each value to produce a comparable key.
/// Values are ordered by their keys.
///
/// Usage:
///   std::set<Person, key_comparator<decltype(&Person::age)>> by_age{&Person::age};
///   std::sort(people.begin(), people.end(), key_comparator{&Person::name});
template <typename KeyFunc>
class key_comparator {
public:
  explicit constexpr key_comparator(KeyFunc key_func) noexcept(
      std::is_nothrow_move_constructible_v<KeyFunc>)
      : key_func_(std::move(key_func)) {}

  template <typename T>
    requires std::invocable<const KeyFunc&, const T&>
  [[nodiscard]] constexpr bool operator()(const T& lhs, const T& rhs) const
      noexcept(noexcept(std::invoke(key_func_, lhs) < std::invoke(key_func_, rhs))) {
    return std::invoke(key_func_, lhs) < std::invoke(key_func_, rhs);
  }

  /// Three-way comparison support for use with <=> aware containers.
  template <typename T>
    requires std::invocable<const KeyFunc&, const T&>
  [[nodiscard]] constexpr auto compare(const T& lhs, const T& rhs) const
      noexcept(noexcept(std::invoke(key_func_, lhs) <=> std::invoke(key_func_, rhs)))
          -> std::compare_three_way_result_t<std::invoke_result_t<const KeyFunc&, const T&>,
                                             std::invoke_result_t<const KeyFunc&, const T&>> {
    return std::invoke(key_func_, lhs) <=> std::invoke(key_func_, rhs);
  }

private:
  KeyFunc key_func_;
};

// Deduction guide
template <typename KeyFunc>
key_comparator(KeyFunc) -> key_comparator<KeyFunc>;

/// A comparator that compares by multiple fields/projections.
///
/// Usage:
///   multi_key_comparator comparator{&Person::last_name, &Person::first_name};
///   std::sort(people.begin(), people.end(), comparator);
template <typename... Projections>
class multi_key_comparator {
public:
  explicit constexpr multi_key_comparator(Projections... projections) noexcept(
      (std::is_nothrow_move_constructible_v<Projections> && ...))
      : projections_(std::move(projections)...) {}

  template <typename T>
    requires(std::invocable<const Projections&, const T&> && ...)
  [[nodiscard]] constexpr bool operator()(const T& lhs, const T& rhs) const {
    return compare_impl(lhs, rhs, std::index_sequence_for<Projections...>{}) < 0;
  }

  template <typename T>
    requires(std::invocable<const Projections&, const T&> && ...)
  [[nodiscard]] constexpr auto compare(const T& lhs, const T& rhs) const
      -> detail::projections_comparison_category_t<T, Projections...> {
    return compare_impl(lhs, rhs, std::index_sequence_for<Projections...>{});
  }

private:
  template <typename T, std::size_t... Is>
  [[nodiscard]] constexpr auto compare_impl(const T& lhs, const T& rhs,
                                            std::index_sequence<Is...> /*indices*/) const
      -> detail::projections_comparison_category_t<T, Projections...> {
    return compare_by(lhs, rhs, std::get<Is>(projections_)...);
  }

  std::tuple<Projections...> projections_;
};

// Deduction guide
template <typename... Projections>
multi_key_comparator(Projections...) -> multi_key_comparator<Projections...>;

/// Create a comparator from projections.
///
/// Usage:
///   auto comparator = make_comparator(&Person::age);
///   auto comparator = make_comparator(&Person::last_name, &Person::first_name);
template <typename... Projections>
[[nodiscard]] constexpr auto make_comparator(Projections&&... projections) {
  if constexpr (sizeof...(Projections) == 1) {
    return key_comparator{std::forward<Projections>(projections)...};
  } else {
    return multi_key_comparator{std::forward<Projections>(projections)...};
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Reverse comparator wrapper
// ─────────────────────────────────────────────────────────────────────────────

/// Wrapper that reverses the comparison order.
///
/// Usage:
///   reverse_comparator<key_comparator<...>> descending{key_comparator{&Person::age}};
///   std::sort(people.begin(), people.end(), descending);
template <typename Comparator>
class reverse_comparator {
public:
  explicit constexpr reverse_comparator(Comparator comparator) noexcept(
      std::is_nothrow_move_constructible_v<Comparator>)
      : comparator_(std::move(comparator)) {}

  template <typename T, typename U>
    requires std::invocable<const Comparator&, const T&, const U&>
  [[nodiscard]] constexpr bool operator()(const T& lhs, const U& rhs) const
      noexcept(noexcept(comparator_(rhs, lhs))) {
    return comparator_(rhs, lhs);
  }

private:
  Comparator comparator_;
};

// Deduction guide
template <typename Comparator>
reverse_comparator(Comparator) -> reverse_comparator<Comparator>;

/// Create a reverse comparator from projections.
///
/// Usage:
///   auto descending = make_reverse_comparator(&Person::age);
template <typename... Projections>
[[nodiscard]] constexpr auto make_reverse_comparator(Projections&&... projections) {
  return reverse_comparator{make_comparator(std::forward<Projections>(projections)...)};
}

// ─────────────────────────────────────────────────────────────────────────────
// Equality utilities
// ─────────────────────────────────────────────────────────────────────────────

/// Check equality by applying a projection.
///
/// Usage:
///   bool same_name = equal_by(person1, person2, &Person::name);
template <typename T, typename Projection>
  requires std::invocable<Projection, const T&>
[[nodiscard]] constexpr bool equal_by(const T& lhs, const T& rhs, Projection&& projection) noexcept(
    noexcept(std::invoke(std::forward<Projection>(projection), lhs) ==
             std::invoke(std::forward<Projection>(projection), rhs))) {
  return std::invoke(std::forward<Projection>(projection), lhs) ==
         std::invoke(std::forward<Projection>(projection), rhs);
}

/// Check equality by applying multiple projections.
///
/// Returns true only if all projections produce equal results.
template <typename T, typename... Projections>
  requires(std::invocable<Projections, const T&> && ...)
[[nodiscard]] constexpr bool equal_by(const T& lhs, const T& rhs, Projections&&... projections) {
  return (equal_by(lhs, rhs, std::forward<Projections>(projections)) && ...);
}

// ─────────────────────────────────────────────────────────────────────────────
// STRAYLIGHT_DEFINE_COMPARISON macro
// ─────────────────────────────────────────────────────────────────────────────

} // namespace straylight::nix::util

/// Generate operator<=> and operator== from member pointers or expressions.
///
/// This macro generates both operators for a class by comparing the specified
/// member expressions using std::tie for lexicographic comparison.
///
/// Usage within a class definition:
///   struct Point {
///     int x;
///     int y;
///     STRAYLIGHT_DEFINE_COMPARISON(Point, x, y)
///   };
///
///   struct Complex {
///     std::string name;
///     int priority;
///     STRAYLIGHT_DEFINE_COMPARISON(Complex, name, priority)
///   };
///
/// The generated operators are:
///   - friend auto operator<=>(const Type&, const Type&) = default style comparison
///   - friend bool operator==(const Type&, const Type&) = default style equality
///
/// Note: For simple cases where all members should be compared, prefer:
///   auto operator<=>(const Type&) const = default;
///   bool operator==(const Type&) const = default;
// Internal macro helper - applies prefix to a single member
#define STRAYLIGHT_COMPARE_MEMBER_(prefix, member) prefix.member

// Internal macro helpers for member list expansion (up to 8 members)
#define STRAYLIGHT_COMPARE_1_(p, m1) STRAYLIGHT_COMPARE_MEMBER_(p, m1)
#define STRAYLIGHT_COMPARE_2_(p, m1, m2)                                                           \
  STRAYLIGHT_COMPARE_MEMBER_(p, m1), STRAYLIGHT_COMPARE_MEMBER_(p, m2)
#define STRAYLIGHT_COMPARE_3_(p, m1, m2, m3)                                                       \
  STRAYLIGHT_COMPARE_2_(p, m1, m2), STRAYLIGHT_COMPARE_MEMBER_(p, m3)
#define STRAYLIGHT_COMPARE_4_(p, m1, m2, m3, m4)                                                   \
  STRAYLIGHT_COMPARE_3_(p, m1, m2, m3), STRAYLIGHT_COMPARE_MEMBER_(p, m4)
#define STRAYLIGHT_COMPARE_5_(p, m1, m2, m3, m4, m5)                                               \
  STRAYLIGHT_COMPARE_4_(p, m1, m2, m3, m4), STRAYLIGHT_COMPARE_MEMBER_(p, m5)
#define STRAYLIGHT_COMPARE_6_(p, m1, m2, m3, m4, m5, m6)                                           \
  STRAYLIGHT_COMPARE_5_(p, m1, m2, m3, m4, m5), STRAYLIGHT_COMPARE_MEMBER_(p, m6)
#define STRAYLIGHT_COMPARE_7_(p, m1, m2, m3, m4, m5, m6, m7)                                       \
  STRAYLIGHT_COMPARE_6_(p, m1, m2, m3, m4, m5, m6), STRAYLIGHT_COMPARE_MEMBER_(p, m7)
#define STRAYLIGHT_COMPARE_8_(p, m1, m2, m3, m4, m5, m6, m7, m8)                                   \
  STRAYLIGHT_COMPARE_7_(p, m1, m2, m3, m4, m5, m6, m7), STRAYLIGHT_COMPARE_MEMBER_(p, m8)

// Count arguments macro
#define STRAYLIGHT_COMPARE_COUNT_(_1, _2, _3, _4, _5, _6, _7, _8, N, ...) N
#define STRAYLIGHT_COMPARE_NARGS_(...)                                                             \
  STRAYLIGHT_COMPARE_COUNT_(__VA_ARGS__, 8, 7, 6, 5, 4, 3, 2, 1)

// Select the appropriate expansion macro
#define STRAYLIGHT_COMPARE_SELECT_(N) STRAYLIGHT_COMPARE_##N##_
#define STRAYLIGHT_COMPARE_EXPAND_(N) STRAYLIGHT_COMPARE_SELECT_(N)
#define STRAYLIGHT_COMPARE_MEMBERS_(prefix, ...)                                                   \
  STRAYLIGHT_COMPARE_EXPAND_(STRAYLIGHT_COMPARE_NARGS_(__VA_ARGS__))(prefix, __VA_ARGS__)

#define STRAYLIGHT_DEFINE_COMPARISON(Type, ...)                                                    \
  [[nodiscard]] friend constexpr auto operator<=>(const Type& lhs, const Type& rhs) noexcept {     \
    return std::tie(STRAYLIGHT_COMPARE_MEMBERS_(lhs, __VA_ARGS__)) <=>                             \
           std::tie(STRAYLIGHT_COMPARE_MEMBERS_(rhs, __VA_ARGS__));                                \
  }                                                                                                \
  [[nodiscard]] friend constexpr bool operator==(const Type& lhs, const Type& rhs) noexcept {      \
    return std::tie(STRAYLIGHT_COMPARE_MEMBERS_(lhs, __VA_ARGS__)) ==                              \
           std::tie(STRAYLIGHT_COMPARE_MEMBERS_(rhs, __VA_ARGS__));                                \
  }

/// Generate comparison operators for a type defined outside the class.
///
/// Usage:
///   // In .cpp file or after class definition
///   STRAYLIGHT_DEFINE_COMPARISON_EXT(MyNamespace::MyClass, name, value)
#define STRAYLIGHT_DEFINE_COMPARISON_EXT(Type, ...)                                                \
  [[nodiscard]] inline constexpr auto operator<=>(const Type& lhs, const Type& rhs) noexcept {     \
    return std::tie(STRAYLIGHT_COMPARE_MEMBERS_(lhs, __VA_ARGS__)) <=>                             \
           std::tie(STRAYLIGHT_COMPARE_MEMBERS_(rhs, __VA_ARGS__));                                \
  }                                                                                                \
  [[nodiscard]] inline constexpr bool operator==(const Type& lhs, const Type& rhs) noexcept {      \
    return std::tie(STRAYLIGHT_COMPARE_MEMBERS_(lhs, __VA_ARGS__)) ==                              \
           std::tie(STRAYLIGHT_COMPARE_MEMBERS_(rhs, __VA_ARGS__));                                \
  }

/// Generate comparison using a key expression.
///
/// Unlike STRAYLIGHT_DEFINE_COMPARISON which uses member names directly,
/// this allows arbitrary expressions for computing the comparison key.
/// Note: This macro only supports single-expression keys.
///
/// Usage:
///   struct CaseInsensitiveString {
///     std::string value;
///     STRAYLIGHT_DEFINE_COMPARISON_BY_KEY(CaseInsensitiveString, to_lower(value))
///   };
#define STRAYLIGHT_DEFINE_COMPARISON_BY_KEY(Type, key_expr)                                        \
  [[nodiscard]] friend constexpr auto operator<=>(const Type& lhs, const Type& rhs) noexcept(      \
      noexcept(lhs.key_expr <=> rhs.key_expr)) {                                                   \
    return lhs.key_expr <=> rhs.key_expr;                                                          \
  }                                                                                                \
  [[nodiscard]] friend constexpr bool operator==(const Type& lhs, const Type& rhs) noexcept(       \
      noexcept(lhs.key_expr == rhs.key_expr)) {                                                    \
    return lhs.key_expr == rhs.key_expr;                                                           \
  }
