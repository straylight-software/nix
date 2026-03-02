#include "derivation-check.h"

#include <queue>

#include "nix/store/build-result.h"
#include "nix/store/store-api.h"

namespace nix {

void check_outputs(store_t& store, const store_path_t& drv_path,
                   const decltype(derivation_t::outputs)& drv_outputs,
                   const decltype(derivation_options_t<store_path_t>::output_checks)& output_checks,
                   const std::map<std::string, valid_path_info_t>& outputs, activity_t& act) {
  std::map<Path, const valid_path_info_t&> outputsByPath;
  for (auto& output : outputs) {
    outputsByPath.emplace(store.printStorePath(output.second.path), output.second);
  }

  for (auto& pair : outputs) {
    // We can't use auto destructuring here because
    // clang-tidy seems to complain about it.
    const std::string& output_name = pair.first;
    const auto& info = pair.second;

    auto* outputSpec = get(drv_outputs, output_name);
    assert(outputSpec);

    if (const auto* dof = std::get_if<derivation_output_t::CAFixed>(&outputSpec->raw)) {
      auto& wanted = dof->ca.hash;

      /* Check wanted hash */
      assert(info.ca);
      auto& got = info.ca->hash;
      if (wanted != got) {
        /* Throw an error after registering the path as
           valid. */
        act.result(res_hash_mismatch, {
                                          {"storePath", store.printStorePath(drv_path)},
                                          {"wanted", wanted},
                                          {"got", got},
                                      });
        throw build_error_t(
            build_result_t::Failure::HashMismatch,
            "hash mismatch in fixed-output derivation '%s':\n  specified: %s\n     got:    %s",
            store.printStorePath(drv_path), wanted.to_string(hash_format_t::sri, true),
            got.to_string(hash_format_t::sri, true));
      }
      if (!info.references.empty()) {
        auto numViolations = info.references.size();
        throw build_error_t(
            build_result_t::Failure::HashMismatch,
            "fixed-output derivations must not reference store paths: '%s' references "
            "%d distinct paths, e.g. '%s'",
            store.printStorePath(drv_path), numViolations,
            store.printStorePath(*info.references.begin()));
      }
    }

    /* Compute the closure and closure size of some output. This
       is slightly tricky because some of its references (namely
       other outputs) may not be valid yet. */
    auto getClosure = [&](const store_path_t& path) {
      uint64_t closureSize = 0;
      store_path_set_t pathsDone;
      std::queue<store_path_t> pathsLeft;
      pathsLeft.push(path);

      while (!pathsLeft.empty()) {
        auto path = pathsLeft.front();
        pathsLeft.pop();
        if (!pathsDone.insert(path).second) {
          continue;
        }

        auto i = outputsByPath.find(store.printStorePath(path));
        if (i != outputsByPath.end()) {
          closureSize += i->second.nar_size;
          for (auto& ref : i->second.references) {
            pathsLeft.push(ref);
          }
        } else {
          auto info = store.queryPathInfo(path);
          closureSize += info->nar_size;
          for (auto& ref : info->references) {
            pathsLeft.push(ref);
          }
        }
      }

      return std::make_pair(std::move(pathsDone), closureSize);
    };

    auto applyChecks = [&](const derivation_options_t<store_path_t>::OutputChecks& checks) {
      if (checks.max_size && info.nar_size > *checks.max_size) {
        throw build_error_t(build_result_t::Failure::OutputRejected,
                            "path '%s' is too large at %d bytes; limit is %d bytes",
                            store.printStorePath(info.path), info.nar_size, *checks.max_size);
      }

      if (checks.maxClosureSize) {
        uint64_t closureSize = getClosure(info.path).second;
        if (closureSize > *checks.maxClosureSize) {
          throw build_error_t(build_result_t::Failure::OutputRejected,
                              "closure of path '%s' is too large at %d bytes; limit is %d bytes",
                              store.printStorePath(info.path), closureSize, *checks.maxClosureSize);
        }
      }

      auto checkRefs = [&](const std::set<DrvRef<store_path_t>>& value, bool allowed,
                           bool recursive) {
        /* Parse a list of reference specifiers.  Each element must
           either be a store path, or the symbolic name of the output
           of the derivation (such as `out'). */
        store_path_set_t spec;
        for (auto& i : value) {
          std::visit(
              overloaded{[&](const store_path_t& path) { spec.insert(path); },
                         [&](const OutputName& refOutputName) {
                           if (auto output = get(outputs, refOutputName)) {
                             spec.insert(output->path);
                           } else {
                             std::string outputsListing = concat_map_strings_sep(
                                 ", ", outputs, [](auto& o) { return o.first; });
                             throw build_error_t(
                                 build_result_t::Failure::OutputRejected,
                                 "derivation '%s' output check for '%s' contains output name '%s',"
                                 " but this is not a valid output of this derivation."
                                 " (Valid outputs are [%s].)",
                                 store.printStorePath(drv_path), output_name, refOutputName,
                                 outputsListing);
                           }
                         }},
              i);
        }

        auto used = recursive ? getClosure(info.path).first : info.references;

        if (recursive && checks.ignoreSelfRefs) {
          used.erase(info.path);
        }

        store_path_set_t badPaths;

        for (auto& i : used) {
          if (allowed) {
            if (!spec.count(i)) {
              badPaths.insert(i);
            }
          } else {
            if (spec.count(i)) {
              badPaths.insert(i);
            }
          }
        }

        if (!badPaths.empty()) {
          std::string badPathsStr;
          for (auto& i : badPaths) {
            badPathsStr += "\n  ";
            badPathsStr += store.printStorePath(i);
          }
          throw build_error_t(build_result_t::Failure::OutputRejected,
                              "output '%s' is not allowed to refer to the following paths:%s",
                              store.printStorePath(info.path), badPathsStr);
        }
      };

      /* Mandatory check: absent whitelist, and present but empty
         whitelist mean very different things. */
      if (auto& refs = checks.allowedReferences) {
        checkRefs(*refs, true, false);
      }
      if (auto& refs = checks.allowedRequisites) {
        checkRefs(*refs, true, true);
      }

      /* Optimization: don't need to do anything when
         disallowed and empty set. */
      if (!checks.disallowedReferences.empty()) {
        checkRefs(checks.disallowedReferences, false, false);
      }
      if (!checks.disallowedRequisites.empty()) {
        checkRefs(checks.disallowedRequisites, false, true);
      }
    };

    std::visit(
        overloaded{
            [&](const derivation_options_t<store_path_t>::OutputChecks& checks) {
              applyChecks(checks);
            },
            [&](const std::map<std::string, derivation_options_t<store_path_t>::OutputChecks>&
                    checksPerOutput) {
              if (auto output_checks = get(checksPerOutput, output_name)) {
                applyChecks(*output_checks);
              }
            },
        },
        output_checks);
  }
}

} // namespace nix
