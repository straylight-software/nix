/**
 * Legacy nix-store compatibility implementation
 *
 * This provides nix-store for NixOS compatibility.
 * The nix-store command is used by:
 * - NixOS bootloader installer (nix-store -q --requisites)
 * - NixOS garbage collection service (nix-store --gc)
 * - Build systems and CI pipelines (nix-store -r)
 * - Various NixOS scripts (nix-store --dump, --restore, etc.)
 *
 * Implemented operations:
 * - --query / -q: Path queries (--requisites, --references, --referrers, --deriver, --outputs)
 * - --realise / -r: Build derivations
 * - --gc: Garbage collection
 * - --dump: Dump path as NAR
 * - --restore: Restore path from NAR
 * - --verify: Verify store integrity
 * - --delete: Delete store paths
 * - --add: Add path to store
 * - --print-roots: Print GC roots
 * - --optimise: Hard-link deduplication
 */

#include <cstring>
#include <filesystem>
#include <iostream>
#include <vector>

#include "nix/cmd/legacy.h"
#include "nix/store/build-result.h"
#include "nix/store/gc-store.h"
#include "nix/store/store-api.h"
#include "nix/store/store-cast.h"
#include "nix/store/store-open.h"
#include "nix/util/archive.h"
#include "nix/util/args.h"
#include "nix/util/posix-source-accessor.h"
#include "nix/util/serialise.h"

namespace nix {

namespace {

// Operation type
enum class NixStoreOp : uint8_t {
  None,
  Query,
  Realise,
  GC,
  Delete,
  Add,
  Dump,
  Restore,
  Export,
  Import,
  Verify,
  Optimise,
  PrintRoots,
  ReadLog,
};

// Query sub-operation
enum class QueryOp : uint8_t {
  None,
  Requisites,   // --requisites / -R
  References,   // --references
  Referrers,    // --referrers
  Deriver,      // --deriver / -d
  Outputs,      // --outputs
  Graph,        // --graph
  Tree,         // --tree
  Binding,      // --binding / -b
  Hash,         // --hash
  Size,         // --size
  Roots,        // --roots
  ValidDerivers // --valid-derivers
};

struct NixStoreArgs {
  NixStoreOp op = NixStoreOp::None;
  QueryOp query_op = QueryOp::None;
  std::vector<std::string> paths;
  std::string binding_name;

  // GC options
  bool gc_print_dead = false;
  bool gc_print_live = false;
  bool gc_delete = true;
  uint64_t gc_max_freed = std::numeric_limits<uint64_t>::max();

  // Verification options
  bool check_contents = false;
  bool repair = false;

  // General options
  bool dry_run = false;
  bool verbose = false;
  bool show_help = false;
};

NixStoreArgs parse_args(int argc, char** argv) {
  NixStoreArgs args;

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];

    // Help/version
    if (arg == "--help" || arg == "-h" || arg == "-?") {
      args.show_help = true;
      return args;
    }
    if (arg == "--version") {
      std::cout << "nix-store (straylight)\n";
      std::exit(0);
    }

    // Main operations
    if (arg == "--query" || arg == "-q") {
      args.op = NixStoreOp::Query;
    } else if (arg == "--realise" || arg == "--realize" || arg == "-r") {
      args.op = NixStoreOp::Realise;
    } else if (arg == "--gc") {
      args.op = NixStoreOp::GC;
    } else if (arg == "--delete") {
      args.op = NixStoreOp::Delete;
    } else if (arg == "--add") {
      args.op = NixStoreOp::Add;
    } else if (arg == "--dump") {
      args.op = NixStoreOp::Dump;
    } else if (arg == "--restore") {
      args.op = NixStoreOp::Restore;
    } else if (arg == "--export") {
      args.op = NixStoreOp::Export;
    } else if (arg == "--import") {
      args.op = NixStoreOp::Import;
    } else if (arg == "--verify") {
      args.op = NixStoreOp::Verify;
    } else if (arg == "--optimise" || arg == "--optimize") {
      args.op = NixStoreOp::Optimise;
    } else if (arg == "--print-roots") {
      args.op = NixStoreOp::PrintRoots;
    } else if (arg == "--read-log" || arg == "-l") {
      args.op = NixStoreOp::ReadLog;
    }
    // Query sub-operations
    else if (arg == "--requisites" || arg == "-R") {
      args.query_op = QueryOp::Requisites;
    } else if (arg == "--references") {
      args.query_op = QueryOp::References;
    } else if (arg == "--referrers") {
      args.query_op = QueryOp::Referrers;
    } else if (arg == "--deriver" || arg == "-d") {
      args.query_op = QueryOp::Deriver;
    } else if (arg == "--outputs") {
      args.query_op = QueryOp::Outputs;
    } else if (arg == "--graph") {
      args.query_op = QueryOp::Graph;
    } else if (arg == "--tree") {
      args.query_op = QueryOp::Tree;
    } else if (arg == "--binding" || arg == "-b") {
      args.query_op = QueryOp::Binding;
      if (i + 1 < argc) {
        args.binding_name = argv[++i];
      }
    } else if (arg == "--hash") {
      args.query_op = QueryOp::Hash;
    } else if (arg == "--size") {
      args.query_op = QueryOp::Size;
    } else if (arg == "--roots") {
      args.query_op = QueryOp::Roots;
    } else if (arg == "--valid-derivers") {
      args.query_op = QueryOp::ValidDerivers;
    }
    // GC options
    else if (arg == "--print-dead") {
      args.gc_print_dead = true;
      args.gc_delete = false;
    } else if (arg == "--print-live") {
      args.gc_print_live = true;
      args.gc_delete = false;
    } else if (arg == "--max-freed") {
      if (i + 1 < argc) {
        args.gc_max_freed = std::stoull(argv[++i]);
      }
    }
    // Verify options
    else if (arg == "--check-contents") {
      args.check_contents = true;
    } else if (arg == "--repair") {
      args.repair = true;
    }
    // General options
    else if (arg == "--dry-run") {
      args.dry_run = true;
    } else if (arg == "--verbose" || arg == "-v") {
      args.verbose = true;
    } else if (arg[0] != '-') {
      args.paths.push_back(arg);
    }
  }

  return args;
}

void show_help() {
  std::cout << R"(nix-store (straylight)

Operations:
  --query / -q           Query store paths
  --realise / -r         Build derivations
  --gc                   Garbage collection
  --delete               Delete specific paths
  --add                  Add path to store
  --dump                 Dump path as NAR to stdout
  --restore              Restore path from NAR on stdin
  --verify               Verify store integrity
  --optimise             Hard-link deduplication
  --print-roots          Print GC roots
  --read-log / -l        Show build log

Query sub-operations:
  --requisites / -R      Print closure (all dependencies)
  --references           Print immediate references
  --referrers            Print immediate referrers
  --deriver / -d         Print deriver of path
  --outputs              Print outputs of derivation
  --hash                 Print NAR hash
  --size                 Print NAR size
  --roots                Print GC roots for path
  --valid-derivers       Print valid derivers

GC options:
  --print-dead           Print unreachable paths (don't delete)
  --print-live           Print reachable paths
  --max-freed N          Stop after freeing N bytes

Verify options:
  --check-contents       Check NAR hashes
  --repair               Repair invalid paths

General options:
  --dry-run              Don't actually perform operation
  --verbose / -v         Verbose output
  --help                 Show this help
  --version              Show version

Examples:
  nix-store -q --requisites /nix/store/...
  nix-store -r /nix/store/....drv
  nix-store --gc
  nix-store --gc --print-dead
)";
}

void op_query(ref<store_t> store, const NixStoreArgs& args) {
  if (args.paths.empty()) {
    throw Error("'nix-store --query' requires at least one path");
  }

  for (const auto& path_str : args.paths) {
    auto path = store->parseStorePath(path_str);

    switch (args.query_op) {
      case QueryOp::Requisites:
      case QueryOp::None: {
        // Default query is requisites (closure)
        store_path_set_t closure;
        store->computeFSClosure(path, closure, false, false);
        for (const auto& p : closure) {
          std::cout << store->printStorePath(p) << "\n";
        }
        break;
      }

      case QueryOp::References: {
        auto info = store->queryPathInfo(path);
        for (const auto& ref : info->references) {
          std::cout << store->printStorePath(ref) << "\n";
        }
        break;
      }

      case QueryOp::Referrers: {
        store_path_set_t referrers;
        store->query_referrers(path, referrers);
        for (const auto& ref : referrers) {
          std::cout << store->printStorePath(ref) << "\n";
        }
        break;
      }

      case QueryOp::Deriver: {
        auto info = store->queryPathInfo(path);
        if (info->deriver) {
          std::cout << store->printStorePath(*info->deriver) << "\n";
        }
        break;
      }

      case QueryOp::Outputs: {
        if (!path.is_derivation()) {
          throw Error("path '%s' is not a derivation", store->printStorePath(path));
        }
        auto outputs = store->queryPartialDerivationOutputMap(path);
        for (const auto& [name, outPath] : outputs) {
          if (outPath) {
            std::cout << store->printStorePath(*outPath) << "\n";
          }
        }
        break;
      }

      case QueryOp::Hash: {
        auto info = store->queryPathInfo(path);
        // SRI format: algo-base64hash
        std::cout << info->nar_hash.to_string(hash_format_t::sri, true) << "\n";
        break;
      }

      case QueryOp::Size: {
        auto info = store->queryPathInfo(path);
        std::cout << info->nar_size << "\n";
        break;
      }

      case QueryOp::ValidDerivers: {
        auto derivers = store->queryValidDerivers(path);
        for (const auto& d : derivers) {
          std::cout << store->printStorePath(d) << "\n";
        }
        break;
      }

      case QueryOp::Roots: {
        // Get GC roots that keep this path alive
        auto* gc_store = dynamic_cast<GcStore*>(&*store);
        if (!gc_store) {
          throw Error("store does not support garbage collection");
        }
        auto roots = gc_store->findRoots(false);
        store_path_set_t closure;
        store->computeFSClosure(path, closure, true, false); // reverse closure
        for (const auto& [root_path, root_info] : roots) {
          if (closure.count(root_path)) {
            for (const auto& info : root_info) {
              std::cout << info << "\n";
            }
          }
        }
        break;
      }

      case QueryOp::Graph:
      case QueryOp::Tree:
      case QueryOp::Binding:
        throw Error("query operation not yet implemented");
    }
  }
}

void op_realise(ref<store_t> store, const NixStoreArgs& args) {
  if (args.paths.empty()) {
    throw Error("'nix-store --realise' requires at least one path");
  }

  std::vector<derived_path_t> paths;
  for (const auto& path_str : args.paths) {
    auto path = store->parseStorePath(path_str);
    if (path.is_derivation()) {
      paths.push_back(derived_path_t::Built{
          .drv_path = makeConstantStorePathRef(path),
          .outputs = OutputsSpec::All{},
      });
    } else {
      paths.push_back(derived_path_t::opaque_t{.path = path});
    }
  }

  auto results = store->build_paths_with_results(paths, args.repair ? bmRepair : bmNormal);

  for (const auto& result : results) {
    if (auto* failure = result.tryGetFailure()) {
      throw Error("build of '%s' failed: %s", result.path.to_string(*store), failure->errorMsg);
    }
    // Print output paths
    if (const auto* success = result.tryGetSuccess()) {
      for (const auto& [outputName, realisation] : success->built_outputs) {
        std::cout << store->printStorePath(realisation.out_path) << "\n";
      }
    }
  }
}

void op_gc(ref<store_t> store, const NixStoreArgs& args) {
  auto* gc_store = dynamic_cast<GcStore*>(&*store);
  if (!gc_store) {
    throw Error("store does not support garbage collection");
  }

  GCOptions options;
  options.maxFreed = args.gc_max_freed;

  if (args.gc_print_live) {
    options.action = GCOptions::gcReturnLive;
  } else if (args.gc_print_dead) {
    options.action = GCOptions::gcReturnDead;
  } else if (args.dry_run) {
    options.action = GCOptions::gcReturnDead;
  } else {
    options.action = GCOptions::gcDeleteDead;
  }

  GCResults results;
  gc_store->collectGarbage(options, results);

  if (args.gc_print_live || args.gc_print_dead || args.dry_run) {
    for (const auto& path : results.paths) {
      std::cout << path << "\n";
    }
  }

  if (options.action == GCOptions::gcDeleteDead) {
    std::cerr << results.paths.size() << " store paths deleted, "
              << (results.bytes_freed / 1024.0 / 1024.0) << " MiB freed\n";
  }
}

void op_delete(ref<store_t> store, const NixStoreArgs& args) {
  if (args.paths.empty()) {
    throw Error("'nix-store --delete' requires at least one path");
  }

  auto* gc_store = dynamic_cast<GcStore*>(&*store);
  if (!gc_store) {
    throw Error("store does not support garbage collection");
  }

  GCOptions options;
  options.action = args.dry_run ? GCOptions::gcReturnDead : GCOptions::gcDeleteSpecific;

  for (const auto& path_str : args.paths) {
    options.pathsToDelete.insert(store->parseStorePath(path_str));
  }

  GCResults results;
  gc_store->collectGarbage(options, results);

  for (const auto& path : results.paths) {
    std::cout << path << "\n";
  }
}

void op_dump(ref<store_t> store, const NixStoreArgs& args) {
  if (args.paths.size() != 1) {
    throw Error("'nix-store --dump' requires exactly one path");
  }

  fd_sink_t sink(STDOUT_FILENO);
  store->nar_from_path(store->parseStorePath(args.paths[0]), sink);
}

void op_restore(const NixStoreArgs& args) {
  if (args.paths.size() != 1) {
    throw Error("'nix-store --restore' requires exactly one path (destination)");
  }

  fd_source_t source(STDIN_FILENO);
  restore_path(args.paths[0], source);
}

void op_verify(ref<store_t> store, const NixStoreArgs& args) {
  bool result = store->verifyStore(args.check_contents, args.repair ? Repair : NoRepair);

  if (result) {
    std::cout << "Store is valid\n";
  } else {
    throw Error("Store verification failed");
  }
}

void op_optimise(ref<store_t> store) {
  store->optimiseStore();
}

void op_print_roots(ref<store_t> store) {
  auto* gc_store = dynamic_cast<GcStore*>(&*store);
  if (!gc_store) {
    throw Error("store does not support garbage collection");
  }

  auto roots = gc_store->findRoots(false);
  for (const auto& [path, info_set] : roots) {
    for (const auto& info : info_set) {
      std::cout << info << " -> " << store->printStorePath(path) << "\n";
    }
  }
}

void op_add(ref<store_t> store, const NixStoreArgs& args) {
  if (args.paths.size() != 1) {
    throw Error("'nix-store --add' requires exactly one path");
  }

  auto src_path =
      posix_source_accessor_t::create_at_root(std::filesystem::weakly_canonical(args.paths[0]));
  auto path =
      store->add_to_store(std::filesystem::path(args.paths[0]).filename().string(), src_path);
  std::cout << store->printStorePath(path) << "\n";
}

void main_nix_store(int argc, char** argv) {
  auto args = parse_args(argc, argv);

  if (args.show_help) {
    show_help();
    return;
  }

  if (args.op == NixStoreOp::None) {
    throw Error("'nix-store' requires an operation. Run 'nix-store --help' for usage.");
  }

  auto store = open_store();

  switch (args.op) {
    case NixStoreOp::Query:
      op_query(store, args);
      break;
    case NixStoreOp::Realise:
      op_realise(store, args);
      break;
    case NixStoreOp::GC:
      op_gc(store, args);
      break;
    case NixStoreOp::Delete:
      op_delete(store, args);
      break;
    case NixStoreOp::Add:
      op_add(store, args);
      break;
    case NixStoreOp::Dump:
      op_dump(store, args);
      break;
    case NixStoreOp::Restore:
      op_restore(args);
      break;
    case NixStoreOp::Verify:
      op_verify(store, args);
      break;
    case NixStoreOp::Optimise:
      op_optimise(store);
      break;
    case NixStoreOp::PrintRoots:
      op_print_roots(store);
      break;
    case NixStoreOp::Export:
    case NixStoreOp::Import:
    case NixStoreOp::ReadLog:
      throw Error("operation not yet implemented");
    case NixStoreOp::None:
      // Already handled above
      break;
  }
}

} // anonymous namespace

static RegisterLegacyCommand r_nix_store("nix-store", main_nix_store);

} // namespace nix
