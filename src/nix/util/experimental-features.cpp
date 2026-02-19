#include "nix/util/experimental-features.h"

#include <nlohmann/json.hpp>

#include "nix/util/fmt.h"
#include "nix/util/strings.h"
#include "nix/util/util.h"

namespace nix {

struct experimental_feature_details_t {
  experimental_feature_t tag;
  std::string_view name;
  std::string_view description;
  std::string_view tracking_url;
};

/**
 * If two different PRs both add an experimental feature, and we just
 * used a number for this, we *wouldn't* get merge conflict and the
 * counter will be incremented once instead of twice, causing a build
 * failure.
 *
 * By instead defining this instead as 1 + the bottom experimental
 * feature, we either have no issue at all if few features are not added
 * at the end of the list, or a proper merge conflict if they are.
 */
constexpr size_t num_xp_features = 1 + static_cast<size_t>(xp_t::parallel_eval);

constexpr std::array<experimental_feature_details_t, num_xp_features> xp_feature_details = {{
    {
        .tag = xp_t::ca_derivations,
        .name = "ca-derivations",
        .description = R"(
            Allow derivations to be content-addressed in order to prevent
            rebuilds when changes to the derivation do not result in changes to
            the derivation's output. See
            [__contentAddressed](@docroot@/language/advanced-attributes.md#adv-attr-__contentAddressed)
            for details.
        )",
        .tracking_url = "https://github.com/NixOS/nix/milestone/35",
    },
    {
        .tag = xp_t::impure_derivations,
        .name = "impure-derivations",
        .description = R"(
            Allow derivations to produce non-fixed outputs by setting the
            `__impure` derivation attribute to `true`. An impure derivation can
            have differing outputs each time it is built.

            Example:

            ```
            derivation {
              name = "impure";
              builder = /bin/sh;
              __impure = true; # mark this derivation as impure
              args = [ "-c" "read -n 10 random < /dev/random; echo $random > $out" ];
              system = builtins.currentSystem;
            }
            ```

            Each time this derivation is built, it can produce a different
            output (as the builder outputs random bytes to `$out`).  Impure
            derivations also have access to the network, and only fixed-output
            or other impure derivations can rely on impure derivations. finally_t,
            an impure derivation cannot also be
            [content-addressed](#xp-feature-ca-derivations).

            This is a more explicit alternative to using [`builtins.currentTime`](@docroot@/language/builtins.md#builtins-currentTime).
        )",
        .tracking_url = "https://github.com/NixOS/nix/milestone/42",
    },
    {
        .tag = xp_t::fetch_tree,
        .name = "fetch-tree",
        .description = R"(
            *Enabled for Determinate Nix Installer users since 2.24*

            Enable the use of the [`fetch_tree`](@docroot@/language/builtins.md#builtins-fetch_tree) built-in function in the Nix language.

            `fetch_tree` exposes a generic interface for fetching remote file system trees from different types of remote sources.
            This built-in was previously guarded by the `flakes` experimental feature because of that overlap.

            Enabling just this feature serves as a "release candidate", allowing users to try it out in isolation.
        )",
        .tracking_url = "https://github.com/NixOS/nix/milestone/31",
    },
    {
        .tag = xp_t::git_hashing,
        .name = "git-hashing",
        .description = R"(
            Allow creating (content-addressed) store objects which are hashed via git's hashing algorithm.
            These store objects aren't understandable by older versions of Nix.
        )",
        .tracking_url = "https://github.com/NixOS/nix/milestone/41",
    },
    {
        .tag = xp_t::recursive_nix,
        .name = "recursive-nix",
        .description = R"(
            Allow derivation builders to call Nix, and thus build derivations
            recursively.

            Example:

            ```
            with import <nixpkgs> {};

            runCommand "foo"
              {
                 # Optional: let Nix know "foo" requires the experimental feature
                 requiredSystemFeatures = [ "recursive-nix" ];
                 buildInputs = [ nix jq ];
                 NIX_PATH = "nixpkgs=${<nixpkgs>}";
              }
              ''
                hello=$(nix-build -E '(import <nixpkgs> {}).hello.overrideDerivation (args: { name = "recursive-hello"; })')

                mkdir -p $out/bin
                ln -s $hello/bin/hello $out/bin/hello
              ''
            ```

            An important restriction on recursive builders is disallowing
            arbitrary substitutions. For example, running

            ```
            nix-store -r /nix/store/lrs9qfm60jcgsk83qhyypj3m4jqsgdid-hello-2.10
            ```

            in the above `runCommand` script would be disallowed, as this could
            lead to derivations with hidden dependencies or breaking
            reproducibility by relying on the current state of the Nix store. An
            exception would be if
            `/nix/store/lrs9qfm60jcgsk83qhyypj3m4jqsgdid-hello-2.10` were
            already in the build inputs or built by a previous recursive Nix
            call.
        )",
        .tracking_url = "https://github.com/NixOS/nix/milestone/47",
    },
    {
        .tag = xp_t::no_url_literals,
        .name = "no-url-literals",
        .description = R"(
            Disallow unquoted URLs as part of the Nix language syntax. The Nix
            language allows for URL literals, like so:

            ```
            $ nix repl
            Welcome to Nix 2.15.0. Type :? for help.

            nix-repl> http://foo
            "http://foo"
            ```

            But enabling this experimental feature causes the Nix parser to
            throw an error when encountering a URL literal:

            ```
            $ nix repl --extra-experimental-features 'no-url-literals'
            Welcome to Nix 2.15.0. Type :? for help.

            nix-repl> http://foo
            error: URL literals are disabled

            at «string»:1:1:

            1| http://foo
             | ^

            ```

            While this is currently an experimental feature, unquoted URLs are
            being deprecated and their usage is discouraged.

            The reason is that, as opposed to path literals, URLs have no
            special properties that distinguish them from regular strings, URLs
            containing parameters have to be quoted anyway, and unquoted URLs
            may confuse external tooling.
        )",
        .tracking_url = "https://github.com/NixOS/nix/milestone/44",
    },
    {
        .tag = xp_t::fetch_closure,
        .name = "fetch-closure",
        .description = R"(
            Enable the use of the [`fetchClosure`](@docroot@/language/builtins.md#builtins-fetchClosure) built-in function in the Nix language.
        )",
        .tracking_url = "https://github.com/NixOS/nix/milestone/40",
    },
    {
        .tag = xp_t::auto_allocate_uids,
        .name = "auto-allocate-uids",
        .description = R"(
            Allows Nix to automatically pick UIDs for builds, rather than creating
            `nixbld*` user accounts. See the [`auto-allocate-uids`](@docroot@/command-ref/conf-file.md#conf-auto-allocate-uids) setting for details.
        )",
        .tracking_url = "https://github.com/NixOS/nix/milestone/34",
    },
    {
        .tag = xp_t::cgroups,
        .name = "cgroups",
        .description = R"(
            Allows Nix to execute builds inside cgroups. See
            the [`use-cgroups`](@docroot@/command-ref/conf-file.md#conf-use-cgroups) setting for details.
        )",
        .tracking_url = "https://github.com/NixOS/nix/milestone/36",
    },
    {
        .tag = xp_t::daemon_trust_override,
        .name = "daemon-trust-override",
        .description = R"(
            Allow forcing trusting or not trusting clients with
            `nix-daemon`. This is useful for testing, but possibly also
            useful for various experiments with `nix-daemon --stdio`
            networking.
        )",
        .tracking_url = "https://github.com/NixOS/nix/milestone/38",
    },
    {
        .tag = xp_t::dynamic_derivations,
        .name = "dynamic-derivations",
        .description = R"(
            Allow the use of a few things related to dynamic derivations:

              - "text hashing" derivation outputs, so we can build .drv
                files.

              - dependencies in derivations on the outputs of
                derivations that are themselves derivations outputs.
        )",
        .tracking_url = "https://github.com/NixOS/nix/milestone/39",
    },
    {
        .tag = xp_t::parse_toml_timestamps,
        .name = "parse-toml-timestamps",
        .description = R"(
            Allow parsing of timestamps in builtins.fromTOML.
        )",
        .tracking_url = "https://github.com/NixOS/nix/milestone/45",
    },
    {
        .tag = xp_t::read_only_local_store,
        .name = "read-only-local-store",
        .description = R"(
            Allow the use of the `read-only` parameter in [local store](@docroot@/store/types/local-store.md) URIs.
        )",
        .tracking_url = "https://github.com/NixOS/nix/milestone/46",
    },
    {
        .tag = xp_t::local_overlay_store,
        .name = "local-overlay-store",
        .description = R"(
            Allow the use of [local overlay store](@docroot@/command-ref/new-cli/nix3-help-stores.md#experimental-local-overlay-store).
        )",
        .tracking_url = "https://github.com/NixOS/nix/milestone/50",
    },
    {
        .tag = xp_t::configurable_impure_env,
        .name = "configurable-impure-env",
        .description = R"(
            Allow the use of the [impure-env](@docroot@/command-ref/conf-file.md#conf-impure-env) setting.
        )",
        .tracking_url = "https://github.com/NixOS/nix/milestone/37",
    },
    {
        .tag = xp_t::mounted_ssh_store_t,
        .name = "mounted-ssh-store",
        .description = R"(
            Allow the use of the [`mounted SSH store`](@docroot@/command-ref/new-cli/nix3-help-stores.html#experimental-ssh-store-with-filesystem-mounted).
        )",
        .tracking_url = "https://github.com/NixOS/nix/milestone/43",
    },
    {
        .tag = xp_t::verified_fetches,
        .name = "verified-fetches",
        .description = R"(
            Enables verification of git commit signatures through the [`fetchGit`](@docroot@/language/builtins.md#builtins-fetchGit) built-in.
        )",
        .tracking_url = "https://github.com/NixOS/nix/milestone/48",
    },
    {
        .tag = xp_t::pipe_operators,
        .name = "pipe-operators",
        .description = R"(
            Add `|>` and `<|` operators to the Nix language.
        )",
        .tracking_url = "https://github.com/NixOS/nix/milestone/55",
    },
    {
        .tag = xp_t::external_builders,
        .name = "external-builders",
        .description = R"(
            Enables support for external builders / sandbox providers.
        )",
        .tracking_url = "",
    },
    {
        .tag = xp_t::blak_e3_hashes,
        .name = "blake3-hashes",
        .description = R"(
            Enables support for BLAKE3 hashes.
        )",
        .tracking_url = "",
    },
    {
        .tag = xp_t::build_time_fetch_tree,
        .name = "build-time-fetch-tree",
        .description = R"(
            Enable the built-in derivation `builtin:fetch-tree`, as well as the flake input attribute `buildTime`.
        )",
        .tracking_url = "",
    },
    {
        .tag = xp_t::parallel_eval,
        .name = "parallel-eval",
        .description = R"(
            Enable built-in functions for parallel evaluation.
        )",
        .tracking_url = "",
    },
}};

static_assert(
    []() constexpr {
      for (auto [index, feature] : enumerate(xp_feature_details)) {
        if (index != (size_t)feature.tag) {
          return false;
}
}
      return true;
    }(),
    "array order does not match enum tag order");

/**
 * A set of previously experimental features that are now considered
 * stable. We don't warn if users have these in `experimental-features`.
 */
std::set<std::string> stabilized_features{"flakes", "nix-command"};

const std::optional<experimental_feature_t> parse_experimental_feature(const std::string_view& name) {
  using reverse_xp_map_t = std::map<std::string_view, experimental_feature_t>;

  static std::unique_ptr<reverse_xp_map_t> reverse_xp_map = []() {
    auto reverse_xp_map = std::make_unique<reverse_xp_map_t>();
    for (auto& xp_feature : xp_feature_details) {
      (*reverse_xp_map)[xp_feature.name] = xp_feature.tag;
}
    return reverse_xp_map;
  }();

  if (auto feature = get(*reverse_xp_map, name)) {
    return *feature;
  } else {
    return std::nullopt;
}
}

std::string_view show_experimental_feature(const experimental_feature_t tag) {
  assert((size_t)tag < xp_feature_details.size());
  return xp_feature_details[(size_t)tag].name;
}

nlohmann::json document_experimental_features() {
  string_map_t res;
  for (auto& xp_feature : xp_feature_details) {
    std::stringstream doc_oss;
    doc_oss << strip_indentation(xp_feature.description);
    doc_oss << fmt("\nRefer to [%1% tracking issue](%2%) for feature tracking.", xp_feature.name,
                  xp_feature.tracking_url);
    res[std::string{xp_feature.name}] = trim(doc_oss.str());
  }
  return (nlohmann::json)res;
}

std::set<experimental_feature_t> parse_features(const string_set_t& raw_features) {
  std::set<experimental_feature_t> res;
  for (auto& raw_feature : raw_features) {
    if (auto feature = parse_experimental_feature(raw_feature)) {
      res.insert(*feature);
}
}
  return res;
}

missing_experimental_feature_t::missing_experimental_feature_t(experimental_feature_t feature,
                                                       std::string reason)
    : Error("experimental Nix feature '%1%' is disabled%2%; add '--extra-experimental-features "
            "%1%' to enable it",
            show_experimental_feature(feature), uncolored_t(optional_bracket(" (", reason, ")"))),
      missing_feature(feature),
      reason{reason} {}

std::ostream& operator<<(std::ostream& str, const experimental_feature_t& feature) {
  return str << show_experimental_feature(feature);
}

void to_json(nlohmann::json& j, const experimental_feature_t& feature) {
  j = show_experimental_feature(feature);
}

void from_json(const nlohmann::json& j, experimental_feature_t& feature) {
  const std::string input = j;
  const auto parsed = parse_experimental_feature(input);

  if (parsed.has_value()) {
    feature = *parsed;
  } else {
    throw Error("Unknown experimental feature '%s' in JSON input", input);
}
}

} // namespace nix
