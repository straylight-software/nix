// straylight::nix::primitives::table - Terminal table formatting
//
// Modern C++23 table formatter replacing nix/util/table.h.
// Provides flexible table building with:
//   - Column alignment (left, right, center)
//   - Border styles (none, ascii, unicode)
//   - Optional ANSI color support
//   - Header row support

#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace straylight::nix::text {

// ─────────────────────────────────────────────────────────────────────────────
// Enumerations
// ─────────────────────────────────────────────────────────────────────────────

/// Column alignment options.
enum class Alignment {
  left,
  right,
  center,
};

/// Border drawing style.
enum class BorderStyle {
  none,    // No borders
  ascii,   // ASCII characters: +, -, |
  unicode, // Unicode box-drawing: ┌, ─, │, etc.
};

// ─────────────────────────────────────────────────────────────────────────────
// Color support
// ─────────────────────────────────────────────────────────────────────────────

/// ANSI color codes for table styling.
struct Color {
  static constexpr std::string_view reset = "\033[0m";
  static constexpr std::string_view bold = "\033[1m";
  static constexpr std::string_view dim = "\033[2m";

  static constexpr std::string_view black = "\033[30m";
  static constexpr std::string_view red = "\033[31m";
  static constexpr std::string_view green = "\033[32m";
  static constexpr std::string_view yellow = "\033[33m";
  static constexpr std::string_view blue = "\033[34m";
  static constexpr std::string_view magenta = "\033[35m";
  static constexpr std::string_view cyan = "\033[36m";
  static constexpr std::string_view white = "\033[37m";

  static constexpr std::string_view bright_black = "\033[90m";
  static constexpr std::string_view bright_red = "\033[91m";
  static constexpr std::string_view bright_green = "\033[92m";
  static constexpr std::string_view bright_yellow = "\033[93m";
  static constexpr std::string_view bright_blue = "\033[94m";
  static constexpr std::string_view bright_magenta = "\033[95m";
  static constexpr std::string_view bright_cyan = "\033[96m";
  static constexpr std::string_view bright_white = "\033[97m";
};

// ─────────────────────────────────────────────────────────────────────────────
// Cell - individual table cell
// ─────────────────────────────────────────────────────────────────────────────

/// A single table cell with content and formatting.
struct Cell {
  std::string content;
  Alignment alignment = Alignment::left;
  std::string color_code;

  /// Construct a cell with content and default left alignment.
  Cell(std::string text) : content(std::move(text)) {}

  /// Construct a cell with content and alignment.
  Cell(std::string text, Alignment align) : content(std::move(text)), alignment(align) {}

  /// Construct a cell with content, alignment, and color.
  Cell(std::string text, Alignment align, std::string_view color)
      : content(std::move(text)), alignment(align), color_code(color) {}

  /// Construct a cell with content and color (default left alignment).
  Cell(std::string text, std::string_view color) : content(std::move(text)), color_code(color) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// Table - the main table builder
// ─────────────────────────────────────────────────────────────────────────────

/// A table builder for formatted terminal output.
///
/// Usage:
///   Table table;
///   table.set_border_style(BorderStyle::unicode);
///   table.set_headers({"Name", "Age", "City"});
///   table.add_row({"Alice", "30", "NYC"});
///   table.add_row({"Bob", "25", "LA"});
///   std::string output = table.render();
class Table {
public:
  /// Construct an empty table with default settings.
  Table() = default;

  /// Construct a table with the specified border style.
  explicit Table(BorderStyle style) : border_style_(style) {}

  // ───────────────────────────────────────────────────────────────────────────
  // Configuration
  // ───────────────────────────────────────────────────────────────────────────

  /// Set the border drawing style.
  void set_border_style(BorderStyle style) noexcept { border_style_ = style; }

  /// Get the current border style.
  [[nodiscard]] BorderStyle border_style() const noexcept { return border_style_; }

  /// Enable or disable color output.
  void set_color_enabled(bool enabled) noexcept { color_enabled_ = enabled; }

  /// Check if color output is enabled.
  [[nodiscard]] bool color_enabled() const noexcept { return color_enabled_; }

  /// Set the color for the header row (when colors are enabled).
  void set_header_color(std::string_view color) { header_color_ = std::string(color); }

  /// Set the color for borders (when colors are enabled).
  void set_border_color(std::string_view color) { border_color_ = std::string(color); }

  /// Set the default alignment for all columns.
  void set_default_alignment(Alignment align) noexcept { default_alignment_ = align; }

  /// Set alignment for a specific column (0-indexed).
  void set_column_alignment(std::size_t column_index, Alignment align) {
    if (column_index >= column_alignments_.size()) {
      column_alignments_.resize(column_index + 1, default_alignment_);
    }
    column_alignments_[column_index] = align;
  }

  /// Set padding (spaces on each side of cell content).
  void set_padding(std::size_t padding) noexcept { padding_ = padding; }

  /// Get current padding.
  [[nodiscard]] std::size_t padding() const noexcept { return padding_; }

  // ───────────────────────────────────────────────────────────────────────────
  // Content
  // ───────────────────────────────────────────────────────────────────────────

  /// Set column headers.
  void set_headers(std::vector<std::string> headers) { headers_ = std::move(headers); }

  /// Set column headers from initializer list.
  void set_headers(std::initializer_list<std::string_view> headers) {
    headers_.clear();
    headers_.reserve(headers.size());
    for (auto header : headers) {
      headers_.emplace_back(header);
    }
  }

  /// Set column headers with cells (allows per-header alignment/color).
  void set_headers(std::vector<Cell> headers) { header_cells_ = std::move(headers); }

  /// Add a row of strings.
  void add_row(std::vector<std::string> row) {
    std::vector<Cell> cells;
    cells.reserve(row.size());
    for (auto& cell : row) {
      cells.emplace_back(std::move(cell));
    }
    rows_.push_back(std::move(cells));
  }

  /// Add a row from initializer list.
  void add_row(std::initializer_list<std::string_view> row) {
    std::vector<Cell> cells;
    cells.reserve(row.size());
    for (auto cell : row) {
      cells.emplace_back(std::string(cell));
    }
    rows_.push_back(std::move(cells));
  }

  /// Add a row of cells (allows per-cell formatting).
  void add_row(std::vector<Cell> row) { rows_.push_back(std::move(row)); }

  /// Clear all rows (headers remain).
  void clear_rows() noexcept { rows_.clear(); }

  /// Clear everything including headers.
  void clear() noexcept {
    headers_.clear();
    header_cells_.clear();
    rows_.clear();
    column_alignments_.clear();
  }

  /// Get the number of rows (excluding header).
  [[nodiscard]] std::size_t row_count() const noexcept { return rows_.size(); }

  /// Get the number of columns (max across all rows and headers).
  [[nodiscard]] std::size_t column_count() const noexcept {
    std::size_t max_columns = headers_.size();
    if (!header_cells_.empty()) {
      max_columns = std::max(max_columns, header_cells_.size());
    }
    for (const auto& row : rows_) {
      max_columns = std::max(max_columns, row.size());
    }
    return max_columns;
  }

  /// Check if the table is empty (no rows or headers).
  [[nodiscard]] bool empty() const noexcept {
    return rows_.empty() && headers_.empty() && header_cells_.empty();
  }

  // ───────────────────────────────────────────────────────────────────────────
  // Rendering
  // ───────────────────────────────────────────────────────────────────────────

  /// Render the table to a string.
  [[nodiscard]] std::string render() const {
    if (empty()) {
      return "";
    }

    std::vector<std::size_t> column_widths = compute_column_widths();
    std::ostringstream output;

    render_top_border(output, column_widths);
    render_header_row(output, column_widths);
    render_header_separator(output, column_widths);
    render_data_rows(output, column_widths);
    render_bottom_border(output, column_widths);

    return output.str();
  }

private:
  // ───────────────────────────────────────────────────────────────────────────
  // Border characters
  // ───────────────────────────────────────────────────────────────────────────

  struct BorderChars {
    std::string_view horizontal;
    std::string_view vertical;
    std::string_view top_left;
    std::string_view top_right;
    std::string_view bottom_left;
    std::string_view bottom_right;
    std::string_view top_tee;
    std::string_view bottom_tee;
    std::string_view left_tee;
    std::string_view right_tee;
    std::string_view cross;
  };

  [[nodiscard]] BorderChars get_border_chars() const noexcept {
    switch (border_style_) {
      case BorderStyle::ascii:
        return {"-", "|", "+", "+", "+", "+", "+", "+", "+", "+", "+"};
      case BorderStyle::unicode:
        return {"─", "│", "┌", "┐", "└", "┘", "┬", "┴", "├", "┤", "┼"};
      case BorderStyle::none:
      default:
        return {"", "", "", "", "", "", "", "", "", "", ""};
    }
  }

  // ───────────────────────────────────────────────────────────────────────────
  // Column width computation
  // ───────────────────────────────────────────────────────────────────────────

  [[nodiscard]] std::vector<std::size_t> compute_column_widths() const {
    std::size_t num_columns = column_count();
    std::vector<std::size_t> widths(num_columns, 0);

    // Check headers
    for (std::size_t index = 0; index < headers_.size(); ++index) {
      widths[index] = std::max(widths[index], display_width(headers_[index]));
    }
    for (std::size_t index = 0; index < header_cells_.size(); ++index) {
      widths[index] = std::max(widths[index], display_width(header_cells_[index].content));
    }

    // Check all rows
    for (const auto& row : rows_) {
      for (std::size_t index = 0; index < row.size(); ++index) {
        widths[index] = std::max(widths[index], display_width(row[index].content));
      }
    }

    return widths;
  }

  /// Compute display width of a string (accounts for ANSI codes having zero width).
  [[nodiscard]] static std::size_t display_width(std::string_view text) {
    std::size_t width = 0;
    bool in_escape = false;

    for (char character : text) {
      if (character == '\033') {
        in_escape = true;
      } else if (in_escape) {
        if (character == 'm') {
          in_escape = false;
        }
      } else {
        ++width;
      }
    }

    return width;
  }

  // ───────────────────────────────────────────────────────────────────────────
  // Alignment helpers
  // ───────────────────────────────────────────────────────────────────────────

  [[nodiscard]] Alignment get_column_alignment(std::size_t column_index) const noexcept {
    if (column_index < column_alignments_.size()) {
      return column_alignments_[column_index];
    }
    return default_alignment_;
  }

  [[nodiscard]] std::string align_text(std::string_view text, std::size_t width,
                                       Alignment alignment) const {
    std::size_t text_width = display_width(text);
    if (text_width >= width) {
      return std::string(text);
    }

    std::size_t total_padding = width - text_width;
    std::string result;
    result.reserve(width + padding_ * 2);

    // Add left padding
    result.append(padding_, ' ');

    switch (alignment) {
      case Alignment::left:
        result.append(text);
        result.append(total_padding, ' ');
        break;
      case Alignment::right:
        result.append(total_padding, ' ');
        result.append(text);
        break;
      case Alignment::center: {
        std::size_t left_pad = total_padding / 2;
        std::size_t right_pad = total_padding - left_pad;
        result.append(left_pad, ' ');
        result.append(text);
        result.append(right_pad, ' ');
        break;
      }
    }

    // Add right padding
    result.append(padding_, ' ');

    return result;
  }

  // ───────────────────────────────────────────────────────────────────────────
  // Border rendering
  // ───────────────────────────────────────────────────────────────────────────

  void render_horizontal_line(std::ostringstream& output,
                              const std::vector<std::size_t>& column_widths,
                              std::string_view left_corner, std::string_view tee,
                              std::string_view right_corner) const {
    if (border_style_ == BorderStyle::none) {
      return;
    }

    BorderChars chars = get_border_chars();

    if (color_enabled_ && !border_color_.empty()) {
      output << border_color_;
    }

    output << left_corner;

    for (std::size_t index = 0; index < column_widths.size(); ++index) {
      std::size_t cell_width = column_widths[index] + padding_ * 2;
      for (std::size_t count = 0; count < cell_width; ++count) {
        output << chars.horizontal;
      }
      if (index < column_widths.size() - 1) {
        output << tee;
      }
    }

    output << right_corner;

    if (color_enabled_ && !border_color_.empty()) {
      output << Color::reset;
    }

    output << '\n';
  }

  void render_top_border(std::ostringstream& output,
                         const std::vector<std::size_t>& column_widths) const {
    BorderChars chars = get_border_chars();
    render_horizontal_line(output, column_widths, chars.top_left, chars.top_tee, chars.top_right);
  }

  void render_bottom_border(std::ostringstream& output,
                            const std::vector<std::size_t>& column_widths) const {
    BorderChars chars = get_border_chars();
    render_horizontal_line(output, column_widths, chars.bottom_left, chars.bottom_tee,
                           chars.bottom_right);
  }

  void render_header_separator(std::ostringstream& output,
                               const std::vector<std::size_t>& column_widths) const {
    if (!has_headers()) {
      return;
    }
    BorderChars chars = get_border_chars();
    render_horizontal_line(output, column_widths, chars.left_tee, chars.cross, chars.right_tee);
  }

  // ───────────────────────────────────────────────────────────────────────────
  // Row rendering
  // ───────────────────────────────────────────────────────────────────────────

  [[nodiscard]] bool has_headers() const noexcept {
    return !headers_.empty() || !header_cells_.empty();
  }

  void render_header_row(std::ostringstream& output,
                         const std::vector<std::size_t>& column_widths) const {
    if (!has_headers()) {
      return;
    }

    BorderChars chars = get_border_chars();
    std::size_t num_columns = column_widths.size();

    // Build the header cells
    std::vector<Cell> effective_headers;
    effective_headers.reserve(num_columns);

    if (!header_cells_.empty()) {
      effective_headers = header_cells_;
    } else {
      for (const auto& header : headers_) {
        effective_headers.emplace_back(header);
      }
    }

    // Render the row
    if (border_style_ != BorderStyle::none) {
      if (color_enabled_ && !border_color_.empty()) {
        output << border_color_;
      }
      output << chars.vertical;
      if (color_enabled_ && !border_color_.empty()) {
        output << Color::reset;
      }
    }

    for (std::size_t index = 0; index < num_columns; ++index) {
      std::string_view content;
      Alignment alignment = get_column_alignment(index);
      std::string_view cell_color;

      if (index < effective_headers.size()) {
        content = effective_headers[index].content;
        if (effective_headers[index].alignment != Alignment::left ||
            index < column_alignments_.size()) {
          // Use cell alignment if explicitly set, otherwise column alignment
          alignment = index < column_alignments_.size() ? column_alignments_[index]
                                                        : effective_headers[index].alignment;
        }
        cell_color = effective_headers[index].color_code;
      }

      // Apply header color
      if (color_enabled_) {
        if (!cell_color.empty()) {
          output << cell_color;
        } else if (!header_color_.empty()) {
          output << header_color_;
        }
      }

      output << align_text(content, column_widths[index], alignment);

      if (color_enabled_ && (!cell_color.empty() || !header_color_.empty())) {
        output << Color::reset;
      }

      if (border_style_ != BorderStyle::none) {
        if (color_enabled_ && !border_color_.empty()) {
          output << border_color_;
        }
        output << chars.vertical;
        if (color_enabled_ && !border_color_.empty()) {
          output << Color::reset;
        }
      }
    }

    output << '\n';
  }

  void render_data_rows(std::ostringstream& output,
                        const std::vector<std::size_t>& column_widths) const {
    BorderChars chars = get_border_chars();
    std::size_t num_columns = column_widths.size();

    for (const auto& row : rows_) {
      if (border_style_ != BorderStyle::none) {
        if (color_enabled_ && !border_color_.empty()) {
          output << border_color_;
        }
        output << chars.vertical;
        if (color_enabled_ && !border_color_.empty()) {
          output << Color::reset;
        }
      }

      for (std::size_t index = 0; index < num_columns; ++index) {
        std::string_view content;
        Alignment alignment = get_column_alignment(index);
        std::string_view cell_color;

        if (index < row.size()) {
          content = row[index].content;
          // Cell alignment overrides column alignment if explicitly set
          if (row[index].alignment != Alignment::left) {
            alignment = row[index].alignment;
          }
          cell_color = row[index].color_code;
        }

        if (color_enabled_ && !cell_color.empty()) {
          output << cell_color;
        }

        output << align_text(content, column_widths[index], alignment);

        if (color_enabled_ && !cell_color.empty()) {
          output << Color::reset;
        }

        if (border_style_ != BorderStyle::none) {
          if (color_enabled_ && !border_color_.empty()) {
            output << border_color_;
          }
          output << chars.vertical;
          if (color_enabled_ && !border_color_.empty()) {
            output << Color::reset;
          }
        }
      }

      output << '\n';
    }
  }

  // ───────────────────────────────────────────────────────────────────────────
  // Member data
  // ───────────────────────────────────────────────────────────────────────────

  BorderStyle border_style_ = BorderStyle::none;
  bool color_enabled_ = false;
  std::string header_color_;
  std::string border_color_;
  Alignment default_alignment_ = Alignment::left;
  std::vector<Alignment> column_alignments_;
  std::size_t padding_ = 1;

  std::vector<std::string> headers_;
  std::vector<Cell> header_cells_;
  std::vector<std::vector<Cell>> rows_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Factory functions
// ─────────────────────────────────────────────────────────────────────────────

/// Create a table with the specified border style.
[[nodiscard]] inline Table make_table(BorderStyle style = BorderStyle::none) {
  return Table(style);
}

/// Create a table with headers.
[[nodiscard]] inline Table make_table(std::initializer_list<std::string_view> headers,
                                      BorderStyle style = BorderStyle::none) {
  Table table(style);
  table.set_headers(headers);
  return table;
}

} // namespace straylight::nix::text
