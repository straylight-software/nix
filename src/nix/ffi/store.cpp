/**
 * @file store.cpp
 * @brief Store operations for Nix FFI.
 */

#include <nix/store/derived-path.h>
#include <nix/store/store-open.h>

#include "internal.h"

extern "C" {

NixError nix_store_open(const char* uri, NixStore** store_out) {
  if (!store_out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "store_out is null");
  }

  return nix::ffi::catch_errors([&] {
    std::string store_uri = uri ? uri : "";
    auto store = nix::open_store(store_uri);

    auto* handle = new NixStore{std::move(store)};
    *store_out = handle;
    return NIX_OK;
  });
}

void nix_store_free(NixStore* store) {
  delete store;
}

NixError nix_store_get_dir(const NixStore* store, NixString* out) {
  if (!store || !out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "store or out is null");
  }

  return nix::ffi::catch_errors([&] {
    nix::ffi::string_set(out, store->store->store_dir);
    return NIX_OK;
  });
}

NixError nix_store_get_uri(const NixStore* store, NixString* out) {
  if (!store || !out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "store or out is null");
  }

  return nix::ffi::catch_errors([&] {
    nix::ffi::string_set(out, store->store->config.getHumanReadableURI());
    return NIX_OK;
  });
}

NixError nix_store_parse_path(const NixStore* store, const char* path, NixStorePath** path_out) {
  if (!store || !path || !path_out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  return nix::ffi::catch_errors([&] {
    auto parsed = store->store->parseStorePath(path);
    auto* handle = new NixStorePath{std::move(parsed)};
    *path_out = handle;
    return NIX_OK;
  });
}

void nix_store_path_free(NixStorePath* path) {
  delete path;
}

NixError nix_store_path_to_string(const NixStore* store, const NixStorePath* path, NixString* out) {
  if (!store || !path || !out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  return nix::ffi::catch_errors([&] {
    nix::ffi::string_set(out, store->store->printStorePath(path->path));
    return NIX_OK;
  });
}

NixError nix_store_path_hash(const NixStorePath* path, NixString* out) {
  if (!path || !out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  return nix::ffi::catch_errors([&] {
    nix::ffi::string_set(out, std::string(path->path.hash_part()));
    return NIX_OK;
  });
}

NixError nix_store_path_name(const NixStorePath* path, NixString* out) {
  if (!path || !out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  return nix::ffi::catch_errors([&] {
    nix::ffi::string_set(out, std::string(path->path.name()));
    return NIX_OK;
  });
}

NixError nix_store_is_valid_path(NixStore* store, const NixStorePath* path, bool* valid_out) {
  if (!store || !path || !valid_out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  return nix::ffi::catch_errors([&] {
    *valid_out = store->store->isValidPath(path->path);
    return NIX_OK;
  });
}

NixError nix_store_query_path_info(NixStore* store, const NixStorePath* path,
                                   uint64_t* nar_size_out, NixStorePath** deriver_out,
                                   NixStorePath*** refs_out, size_t* refs_count_out) {
  if (!store || !path) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  return nix::ffi::catch_errors([&] {
    auto info = store->store->queryPathInfo(path->path);

    if (nar_size_out) {
      *nar_size_out = info->nar_size;
    }

    if (deriver_out) {
      if (info->deriver) {
        *deriver_out = new NixStorePath{*info->deriver};
      } else {
        *deriver_out = nullptr;
      }
    }

    if (refs_out && refs_count_out) {
      *refs_count_out = info->references.size();
      if (info->references.empty()) {
        *refs_out = nullptr;
      } else {
        auto** arr = new NixStorePath*[info->references.size()];
        size_t i = 0;
        for (const auto& ref : info->references) {
          arr[i++] = new NixStorePath{ref};
        }
        *refs_out = arr;
      }
    }

    return NIX_OK;
  });
}

NixError nix_store_compute_closure(NixStore* store, const NixStorePath* const* paths,
                                   size_t paths_count, NixStorePath*** closure_out,
                                   size_t* closure_count_out) {
  if (!store || !paths || !closure_out || !closure_count_out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  return nix::ffi::catch_errors([&] {
    nix::store_path_set_t start_paths;
    for (size_t i = 0; i < paths_count; ++i) {
      start_paths.insert(paths[i]->path);
    }

    nix::store_path_set_t closure;
    store->store->computeFSClosure(start_paths, closure);

    *closure_count_out = closure.size();
    if (closure.empty()) {
      *closure_out = nullptr;
    } else {
      auto** arr = new NixStorePath*[closure.size()];
      size_t i = 0;
      for (const auto& p : closure) {
        arr[i++] = new NixStorePath{p};
      }
      *closure_out = arr;
    }

    return NIX_OK;
  });
}

void nix_store_path_array_free(NixStorePath** paths, size_t count) {
  if (!paths) {
    return;
  }
  for (size_t i = 0; i < count; ++i) {
    delete paths[i];
  }
  delete[] paths;
}

NixError nix_store_build_paths(NixStore* store, const char* const* paths, size_t paths_count) {
  if (!store || !paths) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  return nix::ffi::catch_errors([&] {
    std::vector<nix::derived_path_t> derived_paths;
    for (size_t i = 0; i < paths_count; ++i) {
      derived_paths.push_back(nix::derived_path_t::parse(*store->store, paths[i]));
    }
    store->store->build_paths(derived_paths);
    return NIX_OK;
  });
}

NixError nix_store_ensure_path(NixStore* store, const NixStorePath* path) {
  if (!store || !path) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  return nix::ffi::catch_errors([&] {
    store->store->ensure_path(path->path);
    return NIX_OK;
  });
}

} // extern "C"
