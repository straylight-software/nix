// straylight::nix::primitives::url_fast
//
// Implementation of lazy decoded accessors

#include "url_fast.h"

#include "url.h" // For percent_decode, parse_query

namespace straylight::nix::primitives {

const std::vector<std::string>& url_view::path() const {
  if (!path_cache_) {
    std::vector<std::string> segments;
    auto path_str = path_encoded();

    std::string_view view = path_str;
    while (!view.empty()) {
      auto slash_pos = view.find('/');
      if (slash_pos == 0) {
        segments.emplace_back("");
        view = view.substr(1);
      } else if (slash_pos == std::string_view::npos) {
        segments.push_back(percent_decode(view));
        break;
      } else {
        segments.push_back(percent_decode(view.substr(0, slash_pos)));
        view = view.substr(slash_pos + 1);
      }
    }

    path_cache_ = std::move(segments);
  }
  return *path_cache_;
}

const std::vector<std::pair<std::string, std::string>>& url_view::query() const {
  if (!query_cache_) {
    query_cache_ = parse_query(query_encoded());
  }
  return *query_cache_;
}

const std::string& url_view::fragment() const {
  if (!fragment_cache_) {
    fragment_cache_ = percent_decode(fragment_encoded());
  }
  return *fragment_cache_;
}

} // namespace straylight::nix::primitives
