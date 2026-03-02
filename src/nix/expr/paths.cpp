#include "nix/expr/eval.h"
#include "nix/fetchers/fetch-to-store.h"
#include "nix/store/store-api.h"
#include "nix/util/mounted-source-accessor.h"

namespace nix {

source_path_t eval_state_t::root_path(canon_path_t path) {
  return {root_fs, std::move(path)};
}

source_path_t eval_state_t::root_path(path_view_t path) {
  return {root_fs, canon_path_t(abs_path(path))};
}

source_path_t eval_state_t::store_path(const store_path_t& path) {
  return {root_fs, canon_path_t{store->printStorePath(path)}};
}

store_path_t eval_state_t::devirtualize(const store_path_t& path, string_map_t* rewrites) {
  if (auto mount = storeFS->get_mount(canon_path_t(store->printStorePath(path)))) {
    auto store_path =
        fetch_to_store(fetch_settings, *store, source_path_t{ref(mount)},
                       settings.readOnlyMode ? FetchMode::DryRun : FetchMode::Copy, path.name());
    assert(store_path.name() == path.name());
    if (rewrites) {
      rewrites->emplace(path.hash_part(), store_path.hash_part());
    }
    return store_path;
  } else {
    return path;
  }
}

SingleDerivedPath eval_state_t::devirtualize(const SingleDerivedPath& path,
                                             string_map_t* rewrites) {
  if (auto o = std::get_if<SingleDerivedPath::opaque_t>(&path.raw())) {
    return SingleDerivedPath::opaque_t{devirtualize(o->path, rewrites)};
  } else {
    return path;
  }
}

std::string eval_state_t::devirtualize(std::string_view s, const NixStringContext& context) {
  string_map_t rewrites;

  for (auto& c : context) {
    if (auto o = std::get_if<NixStringContextElem::opaque_t>(&c.raw)) {
      devirtualize(o->path, &rewrites);
    }
  }

  return rewrite_strings(std::string(s), rewrites);
}

std::string eval_state_t::computeBaseName(const source_path_t& path, pos_idx_t pos) {
  if (path.accessor == root_fs) {
    if (auto store_path = store->maybeParseStorePath(path.path.abs())) {
      debug("Copying '%s' to the store again.\n"
            "You can make Nix evaluate faster and copy fewer files by replacing `./.` with the "
            "`self` flake input, "
            "or `builtins.path { path = ./.; name = \"source\"; }`.\n",
            path);
      return std::string(
          fetch_to_store(fetch_settings, *store, path, FetchMode::DryRun, store_path->name())
              .to_string());
    }
  }
  return std::string(path.base_name());
}

store_path_t eval_state_t::mountInput(fetchers::input_t& input,
                                      const fetchers::input_t& original_input,
                                      ref<source_accessor_t> accessor, bool require_lockable,
                                      bool forceNarHash) {
  auto store_path = settings.lazyTrees ? store_path_t::random(input.get_name())
                                       : fetch_to_store(fetch_settings, *store, accessor,
                                                        FetchMode::Copy, input.get_name());

  /* Fix for #8638: Register a temp root for the store path to prevent GC
     from collecting it while the flake input is being evaluated. This is
     critical for flake inputs that are fetched during evaluation - without
     this, auto-GC could delete the store path while evaluation is still
     using it. */
  if (!settings.lazyTrees && store->isValidPath(store_path)) {
    store->addTempRoot(store_path);
  }

  allowPath(store_path); // FIXME: should just whitelist the entire virtual store

  std::optional<Hash> _narHash;

  auto getNarHash = [&]() {
    if (!_narHash) {
      if (store->isValidPath(store_path)) {
        _narHash = store->queryPathInfo(store_path)->nar_hash;
      } else {
        _narHash =
            fetch_to_store2(fetch_settings, *store, accessor, FetchMode::DryRun, input.get_name())
                .second;
      }
    }
    return _narHash;
  };

  storeFS->mount(canon_path_t(store->printStorePath(store_path)), accessor);

  if (forceNarHash ||
      (require_lockable &&
       (!settings.lazyTrees || !settings.lazyLocks || !input.isLocked(fetch_settings)) &&
       !input.getNarHash())) {
    input.attrs.insert_or_assign("narHash", getNarHash()->to_string(hash_format_t::sri, true));
  }

  if (original_input.getNarHash() && *getNarHash() != *original_input.getNarHash()) {
    throw Error((unsigned int)102, "NAR hash mismatch in input '%s', expected '%s' but got '%s'",
                original_input.to_string(), getNarHash()->to_string(hash_format_t::sri, true),
                original_input.getNarHash()->to_string(hash_format_t::sri, true));
  }

  return store_path;
}

} // namespace nix
