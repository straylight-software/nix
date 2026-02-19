#include "nix/cmd/editor-for.h"

#include "nix/util/environment-variables.h"
#include "nix/util/source-path.h"

namespace nix {

strings_t editor_for(const source_path_t& file, uint32_t line) {
  auto path = file.get_physical_path();
  if (!path)
    throw Error("cannot open '%s' in an editor because it has no physical path", file);
  auto editor = get_env("EDITOR").value_or("cat");
  auto args = tokenize_string<strings_t>(editor);
  if (line > 0 &&
      (editor.find("emacs") != std::string::npos || editor.find("nano") != std::string::npos ||
       editor.find("vim") != std::string::npos || editor.find("kak") != std::string::npos))
    args.push_back(fmt("+%d", line));
  args.push_back(path->string());
  return args;
}

} // namespace nix
