#include <queue>

#include "nix/cmd/command.h"
#include "nix/main/shared.h"
#include "nix/store/path-references.h"
#include "nix/store/store-api.h"
#include "nix/util/source-accessor.h"

// Required for printError/printInfo macros which use these unqualified
using nix::fmt;
using nix::logger;

static std::string hilite(const std::string& s, size_t pos, size_t len,
                          const std::string& colour = ANSI_RED) {
  return std::string(s, 0, pos) + colour + std::string(s, pos, len) + ANSI_NORMAL +
         std::string(s, pos + len);
}

static std::string filter_printable(const std::string& s) {
  std::string res;
  for (char c : s)
    res += isprint(c) ? c : '.';
  return res;
}

struct cmd_why_depends_t : nix::SourceExprCommand, nix::MixOperateOnOptions {
  std::string _package, _dependency;
  bool all = false;
  bool precise = false;

  cmd_why_depends_t() {
    expect_args({
        .label = "package",
        .handler = {&_package},
        .completer = getCompleteInstallable(),
    });

    expect_args({
        .label = "dependency",
        .handler = {&_dependency},
        .completer = getCompleteInstallable(),
    });

    add_flag({
        .long_name = "all",
        .short_name = 'a',
        .description = "Show all edges in the dependency graph leading from *package* to "
                       "*dependency*, rather than just a shortest path.",
        .handler = {&all, true},
    });

    add_flag({
        .long_name = "precise",
        .description = "For each edge in the dependency graph, show the files in the parent that "
                       "cause the dependency.",
        .handler = {&precise, true},
    });
  }

  std::string description() override {
    return "show why a package has another package in its closure";
  }

  std::string doc() override {
    return
#include "why-depends.md"
        ;
  }

  category_t category() override { return nix::catSecondary; }

  void run(nix::ref<nix::store_t> store) override {
    auto package = parseInstallable(store, _package);
    auto package_path = nix::Installable::toStorePath(getEvalStore(), store, nix::Realise::Outputs,
                                                      operateOn, package);

    /* We don't need to build `dependency`. We try to get the store
     * path if it's already known, and if not, then it's not a dependency.
     *
     * Why? If `package` does depends on `dependency`, then getting the
     * store path of `package` above necessitated having the store path
     * of `dependency`. The contrapositive is, if the store path of
     * `dependency` is not already known at this point (i.e. it's a CA
     * derivation which hasn't been built), then `package` did not need it
     * to build.
     */
    auto dependency = parseInstallable(store, _dependency);
    auto opt_dependency_path = [&]() -> std::optional<nix::store_path_t> {
      try {
        return {nix::Installable::toStorePath(getEvalStore(), store, nix::Realise::derivation_t,
                                              operateOn, dependency)};
      } catch (nix::MissingRealisation&) {
        return std::nullopt;
      }
    }();

    nix::store_path_set_t closure;
    store->computeFSClosure({package_path}, closure, false, false);

    if (!opt_dependency_path.has_value() || !closure.count(*opt_dependency_path)) {
      printError("'%s' does not depend on '%s'", package->what(), dependency->what());
      return;
    }

    auto dependency_path = *opt_dependency_path;
    auto dependency_path_hash = dependency_path.hash_part();

    auto const inf = std::numeric_limits<size_t>::max();

    struct Node {
      nix::store_path_t path;
      nix::store_path_set_t refs;
      nix::store_path_set_t rrefs;
      size_t dist = inf;
      Node* prev = nullptr;
      bool queued = false;
      bool visited = false;
    };

    std::map<nix::store_path_t, Node> graph;

    for (auto& path : closure)
      graph.emplace(path, Node{.path = path,
                               .refs = store->queryPathInfo(path)->references,
                               .dist = path == dependency_path ? 0 : inf});

    // Transpose the graph.
    for (auto& node : graph)
      for (auto& ref : node.second.refs)
        graph.find(ref)->second.rrefs.insert(node.first);

    /* Run Dijkstra's shortest path algorithm to get the distance
       of every path in the closure to 'dependency'. */
    std::priority_queue<Node*> queue;

    queue.push(&graph.at(dependency_path));

    while (!queue.empty()) {
      auto& node = *queue.top();
      queue.pop();

      for (auto& rref : node.rrefs) {
        auto& node2 = graph.at(rref);
        auto dist = node.dist + 1;
        if (dist < node2.dist) {
          node2.dist = dist;
          node2.prev = &node;
          if (!node2.queued) {
            node2.queued = true;
            queue.push(&node2);
          }
        }
      }
    }

    /* Print the subgraph of nodes that have 'dependency' in their
       closure (i.e., that have a non-infinite distance to
       'dependency'). Print every edge on a path between `package`
       and `dependency`. */
    std::function<void(Node&, const std::string&, const std::string&)> printNode;

    struct bail_out_t {};

    printNode = [&](Node& node, const std::string& firstPad, const std::string& tailPad) {
      assert(node.dist != inf);
      if (precise) {
        nix::logger->cout("%s%s%s%s" ANSI_NORMAL, firstPad, node.visited ? "\e[38;5;244m" : "",
                          firstPad != "" ? "→ " : "", store->printStorePath(node.path));
      }

      if (node.path == dependency_path && !all && package_path != dependency_path)
        throw bail_out_t();

      if (node.visited)
        return;
      if (precise)
        node.visited = true;

      /* Sort the references by distance to `dependency` to
         ensure that the shortest path is printed first. */
      std::multimap<size_t, Node*> refs;
      nix::store_path_set_t refPaths;

      for (auto& ref : node.refs) {
        if (ref == node.path && package_path != dependency_path)
          continue;
        auto& node2 = graph.at(ref);
        if (node2.dist == inf)
          continue;
        refs.emplace(node2.dist, &node2);
        refPaths.insert(node2.path);
      }

      /* For each reference, find the files and symlinks that
         contain the reference. */
      std::map<std::string, nix::strings_t> hits;

      auto accessor = store->requireStoreObjectAccessor(node.path);

      auto getColour = [&](const std::string& hash) {
        return hash == dependency_path_hash ? ANSI_GREEN : ANSI_BLUE;
      };

      if (precise) {
        // Use scanForReferencesDeep to find files containing references
        nix::scan_for_references_deep(
            *accessor, nix::canon_path_t::root, refPaths, [&](nix::FileRefScanResult result) {
              auto p2 = result.filePath.is_root() ? result.filePath.abs() : result.filePath.rel();
              auto st = accessor->lstat(result.filePath);

              if (st.type == nix::source_accessor_t::Type::t_regular) {
                auto contents = accessor->read_file(result.filePath);

                // For each reference found in this file, extract context
                for (auto& foundRef : result.found_refs) {
                  std::string hash(foundRef.hash_part());
                  auto pos = contents.find(hash);
                  if (pos != std::string::npos) {
                    size_t margin = 32;
                    auto pos2 = pos >= margin ? pos - margin : 0;
                    hits[hash].emplace_back(
                        nix::fmt("%s: …%s…", p2,
                                 hilite(filter_printable(std::string(
                                            contents, pos2, pos - pos2 + hash.size() + margin)),
                                        pos - pos2, nix::store_path_t::HashLen, getColour(hash))));
                  }
                }
              } else if (st.type == nix::source_accessor_t::Type::t_symlink) {
                auto target = accessor->read_link(result.filePath);

                // For each reference found in this symlink, show it
                for (auto& foundRef : result.found_refs) {
                  std::string hash(foundRef.hash_part());
                  auto pos = target.find(hash);
                  if (pos != std::string::npos)
                    hits[hash].emplace_back(
                        nix::fmt("%s -> %s", p2,
                                 hilite(target, pos, nix::store_path_t::HashLen, getColour(hash))));
                }
              }
            });
      }

      for (auto& ref : refs) {
        std::string hash(ref.second->path.hash_part());

        bool last = all ? ref == *refs.rbegin() : true;

        for (auto& hit : hits[hash]) {
          bool first = hit == *hits[hash].begin();
          nix::logger->cout("%s%s%s", tailPad,
                            (first ? (last ? nix::tree_last : nix::tree_conn)
                                   : (last ? nix::tree_null : nix::tree_line)),
                            hit);
          if (!all)
            break;
        }

        if (!precise) {
          nix::logger->cout(
              "%s%s%s%s" ANSI_NORMAL, firstPad, ref.second->visited ? "\e[38;5;244m" : "",
              last ? nix::tree_last : nix::tree_conn, store->printStorePath(ref.second->path));
          node.visited = true;
        }

        printNode(*ref.second, tailPad + (last ? nix::tree_null : nix::tree_line),
                  tailPad + (last ? nix::tree_null : nix::tree_line));
      }
    };

    nix::RunPager pager;
    try {
      if (!precise) {
        nix::logger->cout("%s", store->printStorePath(graph.at(package_path).path));
      }
      printNode(graph.at(package_path), "", "");
    } catch (bail_out_t&) {
    }
  }
};

static auto r_cmd_why_depends = nix::registerCommand<cmd_why_depends_t>("why-depends");
