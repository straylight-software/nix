#include "nix/store/builtins.h"
#include "nix/util/tarfile.h"

namespace nix {

static void builtin_unpack_channel(const BuiltinBuilderContext& ctx) {
  auto get_attr = [&](const std::string& name) -> const std::string& {
    auto i = ctx.drv.env.find(name);
    if (i == ctx.drv.env.end()) {
      throw Error("attribute '%s' missing", name);
    }
    return i->second;
  };

  std::filesystem::path out{ctx.outputs.at("out")};
  auto& channel_name = get_attr("channelName");
  auto& src = get_attr("src");

  if (std::filesystem::path{channel_name}.filename().string() != channel_name) {
    throw Error("channelName is not allowed to contain filesystem separators, got %1%",
                channel_name);
  }

  create_dirs(out);

  unpack_tarfile(src, out);

  size_t file_count;
  std::string file_name;
  auto entries = directory_iterator_t{out};
  file_name = entries->path().string();
  file_count = std::distance(entries.begin(), entries.end());

  if (file_count != 1) {
    throw Error("channel tarball '%s' contains more than one file", src);
  }

  auto target = out / channel_name;
  try {
    std::filesystem::rename(file_name, target);
  } catch (std::filesystem::filesystem_error&) {
    throw sys_error_t("failed to rename %1% to %2%", file_name, target.string());
  }
}

static RegisterBuiltinBuilder register_unpack_channel("unpack-channel", builtin_unpack_channel);

} // namespace nix
