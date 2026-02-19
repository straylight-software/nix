#pragma once
///@file

#include <nlohmann/json_fwd.hpp>

#include "nix/flake/flakeref.h"

namespace nix {
class Store;
class StorePath;
} // namespace nix

namespace nix::flake {

typedef std::vector<FlakeId> InputAttrPath;

struct LockedNode;

/**
 * A node in the lock file. It has outgoing edges to other nodes (its
 * inputs). Only the root node has this type; all other nodes have
 * type LockedNode.
 */
struct Node : std::enable_shared_from_this<Node> {
  typedef std::variant<ref<LockedNode>, InputAttrPath> Edge;

  std::map<FlakeId, Edge> inputs;

  virtual ~Node() {}
};

/**
 * A non-root node in the lock file.
 */
struct LockedNode : Node {
  FlakeRef locked_ref, original_ref;
  bool is_flake = true;
  bool buildTime = false;

  /* The node relative to which relative source paths
     (e.g. 'path:../foo') are interpreted. */
  std::optional<InputAttrPath> parent_input_attr_path;

  LockedNode(const FlakeRef& locked_ref, const FlakeRef& original_ref, bool is_flake = true,
             bool buildTime = false, std::optional<InputAttrPath> parent_input_attr_path = {})
      : locked_ref(std::move(locked_ref)),
        original_ref(std::move(original_ref)),
        is_flake(is_flake),
        buildTime(buildTime),
        parent_input_attr_path(std::move(parent_input_attr_path)) {}

  LockedNode(const fetchers::settings_t& fetch_settings, const nlohmann::json& json);

  StorePath computeStorePath(Store& store) const;
};

struct LockFile {
  ref<Node> root = make_ref<Node>();

  LockFile() {};
  LockFile(const fetchers::settings_t& fetch_settings, std::string_view contents,
           std::string_view path);

  typedef std::map<ref<const Node>, std::string> KeyMap;

  std::pair<nlohmann::json, KeyMap> to_json() const;

  std::pair<std::string, KeyMap> to_string() const;

  /**
   * Check whether this lock file has any unlocked or non-final
   * inputs. If so, return one.
   */
  std::optional<FlakeRef> isUnlocked(const fetchers::settings_t& fetch_settings) const;

  bool operator==(const LockFile& other) const;

  std::shared_ptr<Node> findInput(const InputAttrPath& path);

  std::map<InputAttrPath, Node::Edge> getAllInputs() const;

  static std::string diff(const LockFile& oldLocks, const LockFile& newLocks);

  /**
   * Check that every 'follows' input target exists.
   */
  void check();
};

std::ostream& operator<<(std::ostream& stream, const LockFile& lock_file);

InputAttrPath parse_input_attr_path(std::string_view s);

std::string print_input_attr_path(const InputAttrPath& path);

} // namespace nix::flake
