#pragma once
///@file

#include <nlohmann/json_fwd.hpp>

#include "nix/store/derived-path.h"
#include "nix/util/comparator.h"
#include "nix/util/variant-wrapper.h"

namespace nix {

class BadNixStringContextElem : public Error {
public:
  std::string_view raw;

  template <typename... Args>
  BadNixStringContextElem(std::string_view raw_, const Args&... args) : Error("") {
    raw = raw_;
    auto hf = hint_fmt_t(args...);
    err.msg = hint_fmt_t("Bad String Context element: %1%: %2%", uncolored_t(hf.str()), raw);
  }
};

/**
 * @todo This should be renamed to `StringContextBuilderElem`, since:
 *
 * 1. We use `*Builder` for off-heap temporary data structures
 *
 * 2. The `Nix*` is totally redundant. (And my mistake from a long time
 * ago.)
 */
struct NixStringContextElem {
  /**
   * Plain opaque path to some store object.
   *
   * Encoded as just the path: `<path>`.
   */
  using opaque_t = SingleDerivedPath::opaque_t;

  /**
   * Path to a derivation and its entire build closure.
   *
   * The path doesn't just refer to derivation itself and its closure, but
   * also all outputs of all derivations in that closure (including the
   * root derivation).
   *
   * Encoded in the form `=<drv_path>`.
   */
  struct DrvDeep {
    StorePath drv_path;

    GENERATE_CMP(DrvDeep, me->drv_path);
  };

  /**
   * Derivation output.
   *
   * Encoded in the form `!<output>!<drv_path>`.
   */
  using Built = SingleDerivedPath::Built;

  /**
   * A store path that will not result in a store reference when
   * used in a derivation or toFile.
   *
   * When you apply `builtins.toString` to a path value representing
   * a path in the Nix store (as is the case with flake inputs),
   * historically you got a string without context
   * (e.g. `/nix/store/...-source`). This is broken, since it allows
   * you to pass a store path to a derivation/toFile without a
   * proper store reference. This is especially a problem with lazy
   * trees, since the store path is a virtual path that doesn't
   * exist.
   *
   * For backwards compatibility, and to warn users about this
   * unsafe use of `toString`, we keep track of such strings as a
   * special type of context.
   */
  struct Path {
    StorePath store_path;

    GENERATE_CMP(Path, me->store_path);
  };

  using raw_t = std::variant<opaque_t, DrvDeep, Built, Path>;

  raw_t raw;

  GENERATE_CMP(NixStringContextElem, me->raw);

  MAKE_WRAPPER_CONSTRUCTOR(NixStringContextElem);

  /**
   * Decode a context string, one of:
   * - `<path>`
   * - `=<path>`
   * - `!<name>!<path>`
   *
   * @param xp_settings Stop-gap to avoid globals during unit tests.
   */
  static NixStringContextElem
  parse(std::string_view s,
        const experimental_feature_settings_t& xp_settings = experimental_feature_settings);
  std::string to_string() const;
};

/**
 * @todo This should be renamed to `StringContextBuilder`.
 *
 * @see NixStringContextElem for explanation why.
 */
typedef std::set<NixStringContextElem> NixStringContext;

/**
 * Returns false if `context` has no elements other than
 * `NixStringContextElem::Path`.
 */
bool has_context(const NixStringContext& context);

} // namespace nix
