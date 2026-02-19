#include "nix/util/pos-table.h"

#include <algorithm>

namespace nix {

/* Position table. */

pos_t pos_table_t::operator[](pos_idx_t p) const {
  auto origin = resolve(p);
  if (!origin) {
    return {};
}

  const auto offset = origin->offset_of(p);

  pos_t result{0, 0, origin->origin};
  auto lines_cache = this->lines_cache.lock();

  /* Try the origin's line cache */
  const auto* lines_for_input = lines_cache->get_or_nullptr(origin->offset);

  auto fill_cache_for_origin = [](std::string_view content) {
    auto content_lines = lines_t();

    const char* begin = content.data();
    for (pos_t::lines_iterator_t it(content), end; it != end; it++) {
      content_lines.push_back(it->data() - begin);
}
    if (content_lines.empty()) {
      content_lines.push_back(0);
}

    return content_lines;
  };

  /* Calculate line offsets and fill the cache */
  if (!lines_for_input) {
    auto origin_content = result.get_source().value_or("");
    lines_cache->upsert(origin->offset, fill_cache_for_origin(origin_content));
    lines_for_input = lines_cache->get_or_nullptr(origin->offset);
  }

  assert(lines_for_input);

  // as above: the first line starts at byte 0 and is always present
  auto line_start_offset =
      std::prev(std::upper_bound(lines_for_input->begin(), lines_for_input->end(), offset));
  result.line = 1 + (line_start_offset - lines_for_input->begin());
  result.column = 1 + (offset - *line_start_offset);
  return result;
}

} // namespace nix
