// straylight::nix::text::table tests
//
// Tests for table formatting: Table, Cell, Alignment, BorderStyle, Color

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "straylight/nix/text/table.h"

namespace tbl = straylight::nix::text;

// ─────────────────────────────────────────────────────────────────────────────
// Table - empty and basic construction
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Empty table renders empty string", "[table]") {
  tbl::Table table;
  REQUIRE(table.empty());
  REQUIRE(table.render().empty());
}

TEST_CASE("Table can be constructed with border style", "[table]") {
  tbl::Table table(tbl::BorderStyle::unicode);
  REQUIRE(table.border_style() == tbl::BorderStyle::unicode);
}

TEST_CASE("Table default border style is none", "[table]") {
  tbl::Table table;
  REQUIRE(table.border_style() == tbl::BorderStyle::none);
}

TEST_CASE("Table row_count returns correct value", "[table]") {
  tbl::Table table;
  REQUIRE(table.row_count() == 0);
  table.add_row({"a", "b"});
  REQUIRE(table.row_count() == 1);
  table.add_row({"c", "d"});
  REQUIRE(table.row_count() == 2);
}

TEST_CASE("Table column_count returns max columns", "[table]") {
  tbl::Table table;
  REQUIRE(table.column_count() == 0);

  table.set_headers({"A", "B", "C"});
  REQUIRE(table.column_count() == 3);

  table.add_row({"1", "2", "3", "4", "5"});
  REQUIRE(table.column_count() == 5);
}

TEST_CASE("Table empty() reflects state", "[table]") {
  tbl::Table table;
  REQUIRE(table.empty());

  table.set_headers({"H"});
  REQUIRE_FALSE(table.empty());

  table.clear();
  REQUIRE(table.empty());

  table.add_row({"R"});
  REQUIRE_FALSE(table.empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// Table - headers
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Table can set headers from vector", "[table]") {
  tbl::Table table;
  std::vector<std::string> headers = {"Name", "Age"};
  table.set_headers(headers);
  table.set_border_style(tbl::BorderStyle::ascii);

  std::string output = table.render();
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("Name"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("Age"));
}

TEST_CASE("Table can set headers from initializer list", "[table]") {
  tbl::Table table;
  table.set_headers({"Col1", "Col2", "Col3"});
  table.set_border_style(tbl::BorderStyle::ascii);

  std::string output = table.render();
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("Col1"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("Col2"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("Col3"));
}

TEST_CASE("Table can set headers as cells with alignment", "[table]") {
  tbl::Table table;
  std::vector<tbl::Cell> headers = {
      tbl::Cell("Left", tbl::Alignment::left),
      tbl::Cell("Right", tbl::Alignment::right),
      tbl::Cell("Center", tbl::Alignment::center),
  };
  table.set_headers(headers);
  table.set_border_style(tbl::BorderStyle::ascii);

  std::string output = table.render();
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("Left"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("Right"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("Center"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Table - rows
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Table can add rows from vector", "[table]") {
  tbl::Table table;
  table.set_border_style(tbl::BorderStyle::ascii);

  std::vector<std::string> row = {"Alice", "30"};
  table.add_row(row);

  std::string output = table.render();
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("Alice"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("30"));
}

TEST_CASE("Table can add rows from initializer list", "[table]") {
  tbl::Table table;
  table.set_border_style(tbl::BorderStyle::ascii);
  table.add_row({"Bob", "25", "NYC"});

  std::string output = table.render();
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("Bob"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("25"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("NYC"));
}

TEST_CASE("Table can add rows as cells", "[table]") {
  tbl::Table table;
  table.set_border_style(tbl::BorderStyle::ascii);

  std::vector<tbl::Cell> row = {
      tbl::Cell("Value1"),
      tbl::Cell("Value2", tbl::Alignment::right),
  };
  table.add_row(row);

  std::string output = table.render();
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("Value1"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("Value2"));
}

TEST_CASE("Table clear_rows() keeps headers", "[table]") {
  tbl::Table table;
  table.set_headers({"Header"});
  table.add_row({"Row1"});
  table.add_row({"Row2"});

  REQUIRE(table.row_count() == 2);
  table.clear_rows();
  REQUIRE(table.row_count() == 0);

  std::string output = table.render();
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("Header"));
}

TEST_CASE("Table clear() removes everything", "[table]") {
  tbl::Table table;
  table.set_headers({"Header"});
  table.add_row({"Row"});

  table.clear();
  REQUIRE(table.empty());
  REQUIRE(table.row_count() == 0);
  REQUIRE(table.column_count() == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Table - border styles
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Table with no border style renders without borders", "[table][border]") {
  tbl::Table table;
  table.set_border_style(tbl::BorderStyle::none);
  table.set_headers({"A", "B"});
  table.add_row({"1", "2"});

  std::string output = table.render();
  REQUIRE_THAT(output, !Catch::Matchers::ContainsSubstring("|"));
  REQUIRE_THAT(output, !Catch::Matchers::ContainsSubstring("+"));
  REQUIRE_THAT(output, !Catch::Matchers::ContainsSubstring("-"));
}

TEST_CASE("Table with ASCII border style uses ASCII chars", "[table][border]") {
  tbl::Table table;
  table.set_border_style(tbl::BorderStyle::ascii);
  table.set_headers({"A"});
  table.add_row({"1"});

  std::string output = table.render();
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("+"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("-"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("|"));
}

TEST_CASE("Table with Unicode border style uses box drawing", "[table][border]") {
  tbl::Table table;
  table.set_border_style(tbl::BorderStyle::unicode);
  table.set_headers({"A"});
  table.add_row({"1"});

  std::string output = table.render();
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("┌"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("─"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("│"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("└"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("┐"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("┘"));
}

TEST_CASE("Table Unicode borders include cross and tee chars", "[table][border]") {
  tbl::Table table;
  table.set_border_style(tbl::BorderStyle::unicode);
  table.set_headers({"A", "B"});
  table.add_row({"1", "2"});

  std::string output = table.render();
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("┬")); // top tee
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("┼")); // cross
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("┴")); // bottom tee
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("├")); // left tee
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("┤")); // right tee
}

// ─────────────────────────────────────────────────────────────────────────────
// Table - alignment
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Table default alignment is left", "[table][alignment]") {
  tbl::Table table;
  table.set_border_style(tbl::BorderStyle::ascii);
  table.add_row({"X"});
  table.add_row({"Long text"});

  std::string output = table.render();
  // X should be left-aligned, so followed by spaces
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("| X"));
}

TEST_CASE("Table set_default_alignment affects all columns", "[table][alignment]") {
  tbl::Table table;
  table.set_border_style(tbl::BorderStyle::ascii);
  table.set_default_alignment(tbl::Alignment::right);
  table.add_row({"X"});
  table.add_row({"Long"});

  std::string output = table.render();
  // X should be right-aligned with padding before it
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("X |"));
}

TEST_CASE("Table set_column_alignment overrides default", "[table][alignment]") {
  tbl::Table table;
  table.set_border_style(tbl::BorderStyle::ascii);
  table.set_default_alignment(tbl::Alignment::left);
  table.set_column_alignment(1, tbl::Alignment::right);

  table.add_row({"Left", "Right"});
  table.add_row({"L", "R"});

  std::string output = table.render();
  // Second column should be right-aligned
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("Right |"));
}

TEST_CASE("Table center alignment centers text", "[table][alignment]") {
  tbl::Table table;
  table.set_border_style(tbl::BorderStyle::ascii);
  table.set_column_alignment(0, tbl::Alignment::center);

  table.add_row({"X"});
  table.add_row({"Long text"});

  std::string output = table.render();
  // X should have roughly equal padding on both sides
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("X"));
}

TEST_CASE("Cell alignment overrides column alignment", "[table][alignment]") {
  tbl::Table table;
  table.set_border_style(tbl::BorderStyle::ascii);
  table.set_default_alignment(tbl::Alignment::left);

  std::vector<tbl::Cell> row = {
      tbl::Cell("Centered", tbl::Alignment::center),
  };
  table.add_row(row);
  table.add_row({"Normal left"});

  // The cell with explicit center alignment should be centered
  std::string output = table.render();
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("Centered"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Table - padding
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Table default padding is 1", "[table][padding]") {
  tbl::Table table;
  REQUIRE(table.padding() == 1);
}

TEST_CASE("Table set_padding changes padding", "[table][padding]") {
  tbl::Table table;
  table.set_padding(3);
  REQUIRE(table.padding() == 3);
}

TEST_CASE("Table with zero padding has minimal spacing", "[table][padding]") {
  tbl::Table table;
  table.set_border_style(tbl::BorderStyle::ascii);
  table.set_padding(0);
  table.add_row({"A"});

  std::string output = table.render();
  // With zero padding, cell content should be directly adjacent to borders
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("|A|"));
}

TEST_CASE("Table with larger padding has more spacing", "[table][padding]") {
  tbl::Table table;
  table.set_border_style(tbl::BorderStyle::ascii);
  table.set_padding(3);
  table.add_row({"A"});

  std::string output = table.render();
  // With padding 3, expect "   A   " pattern
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("|   A   |"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Table - color support
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Table color is disabled by default", "[table][color]") {
  tbl::Table table;
  REQUIRE_FALSE(table.color_enabled());
}

TEST_CASE("Table set_color_enabled toggles color", "[table][color]") {
  tbl::Table table;
  table.set_color_enabled(true);
  REQUIRE(table.color_enabled());
  table.set_color_enabled(false);
  REQUIRE_FALSE(table.color_enabled());
}

TEST_CASE("Table with disabled color does not include ANSI codes", "[table][color]") {
  tbl::Table table;
  table.set_border_style(tbl::BorderStyle::ascii);
  table.set_color_enabled(false);
  table.set_header_color(tbl::Color::bold);
  table.set_headers({"Header"});
  table.add_row({"Value"});

  std::string output = table.render();
  REQUIRE_THAT(output, !Catch::Matchers::ContainsSubstring("\033["));
}

TEST_CASE("Table with header color includes ANSI codes", "[table][color]") {
  tbl::Table table;
  table.set_border_style(tbl::BorderStyle::ascii);
  table.set_color_enabled(true);
  table.set_header_color(tbl::Color::bold);
  table.set_headers({"Header"});
  table.add_row({"Value"});

  std::string output = table.render();
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("\033[1m")); // bold
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("\033[0m")); // reset
}

TEST_CASE("Table with border color includes ANSI codes", "[table][color]") {
  tbl::Table table;
  table.set_border_style(tbl::BorderStyle::ascii);
  table.set_color_enabled(true);
  table.set_border_color(tbl::Color::cyan);
  table.add_row({"Value"});

  std::string output = table.render();
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("\033[36m")); // cyan
}

TEST_CASE("Cell with color renders color codes", "[table][color]") {
  tbl::Table table;
  table.set_border_style(tbl::BorderStyle::ascii);
  table.set_color_enabled(true);

  std::vector<tbl::Cell> row = {
      tbl::Cell("Green", tbl::Color::green),
  };
  table.add_row(row);

  std::string output = table.render();
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("\033[32m")); // green
}

TEST_CASE("Cell with color and alignment", "[table][color]") {
  tbl::Table table;
  table.set_border_style(tbl::BorderStyle::ascii);
  table.set_color_enabled(true);

  std::vector<tbl::Cell> row = {
      tbl::Cell("Red Right", tbl::Alignment::right, tbl::Color::red),
  };
  table.add_row(row);
  table.add_row({"Normal longer text"});

  std::string output = table.render();
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("\033[31m")); // red
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("Red Right"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Color struct constants
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Color constants have expected values", "[color]") {
  REQUIRE(tbl::Color::reset == "\033[0m");
  REQUIRE(tbl::Color::bold == "\033[1m");
  REQUIRE(tbl::Color::dim == "\033[2m");
  REQUIRE(tbl::Color::black == "\033[30m");
  REQUIRE(tbl::Color::red == "\033[31m");
  REQUIRE(tbl::Color::green == "\033[32m");
  REQUIRE(tbl::Color::yellow == "\033[33m");
  REQUIRE(tbl::Color::blue == "\033[34m");
  REQUIRE(tbl::Color::magenta == "\033[35m");
  REQUIRE(tbl::Color::cyan == "\033[36m");
  REQUIRE(tbl::Color::white == "\033[37m");
}

TEST_CASE("Color bright variants have expected values", "[color]") {
  REQUIRE(tbl::Color::bright_black == "\033[90m");
  REQUIRE(tbl::Color::bright_red == "\033[91m");
  REQUIRE(tbl::Color::bright_green == "\033[92m");
  REQUIRE(tbl::Color::bright_yellow == "\033[93m");
  REQUIRE(tbl::Color::bright_blue == "\033[94m");
  REQUIRE(tbl::Color::bright_magenta == "\033[95m");
  REQUIRE(tbl::Color::bright_cyan == "\033[96m");
  REQUIRE(tbl::Color::bright_white == "\033[97m");
}

// ─────────────────────────────────────────────────────────────────────────────
// Cell construction
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Cell default construction", "[cell]") {
  tbl::Cell cell("test");
  REQUIRE(cell.content == "test");
  REQUIRE(cell.alignment == tbl::Alignment::left);
  REQUIRE(cell.color_code.empty());
}

TEST_CASE("Cell with alignment", "[cell]") {
  tbl::Cell cell("test", tbl::Alignment::right);
  REQUIRE(cell.content == "test");
  REQUIRE(cell.alignment == tbl::Alignment::right);
  REQUIRE(cell.color_code.empty());
}

TEST_CASE("Cell with alignment and color", "[cell]") {
  tbl::Cell cell("test", tbl::Alignment::center, tbl::Color::blue);
  REQUIRE(cell.content == "test");
  REQUIRE(cell.alignment == tbl::Alignment::center);
  REQUIRE(cell.color_code == tbl::Color::blue);
}

TEST_CASE("Cell with color only (default left alignment)", "[cell]") {
  tbl::Cell cell("test", tbl::Color::red);
  REQUIRE(cell.content == "test");
  REQUIRE(cell.alignment == tbl::Alignment::left);
  REQUIRE(cell.color_code == tbl::Color::red);
}

// ─────────────────────────────────────────────────────────────────────────────
// Factory functions
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("make_table creates empty table", "[factory]") {
  auto table = tbl::make_table();
  REQUIRE(table.empty());
  REQUIRE(table.border_style() == tbl::BorderStyle::none);
}

TEST_CASE("make_table with border style", "[factory]") {
  auto table = tbl::make_table(tbl::BorderStyle::unicode);
  REQUIRE(table.border_style() == tbl::BorderStyle::unicode);
}

TEST_CASE("make_table with headers", "[factory]") {
  auto table = tbl::make_table({"A", "B", "C"});
  REQUIRE(table.column_count() == 3);
  REQUIRE_FALSE(table.empty());
}

TEST_CASE("make_table with headers and border style", "[factory]") {
  auto table = tbl::make_table({"X", "Y"}, tbl::BorderStyle::ascii);
  REQUIRE(table.column_count() == 2);
  REQUIRE(table.border_style() == tbl::BorderStyle::ascii);
}

// ─────────────────────────────────────────────────────────────────────────────
// Complete table rendering
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Complete ASCII table renders correctly", "[table][render]") {
  tbl::Table table;
  table.set_border_style(tbl::BorderStyle::ascii);
  table.set_headers({"Name", "Age", "City"});
  table.add_row({"Alice", "30", "NYC"});
  table.add_row({"Bob", "25", "LA"});

  std::string output = table.render();

  // Check structure
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("+"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("|"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("-"));

  // Check content
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("Name"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("Age"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("City"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("Alice"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("Bob"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("NYC"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("LA"));
}

TEST_CASE("Complete Unicode table renders correctly", "[table][render]") {
  tbl::Table table;
  table.set_border_style(tbl::BorderStyle::unicode);
  table.set_headers({"Item", "Price"});
  table.add_row({"Apple", "$1.00"});
  table.add_row({"Banana", "$0.50"});

  std::string output = table.render();

  // Check Unicode borders
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("┌"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("┐"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("└"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("┘"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("│"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("─"));

  // Check content
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("Apple"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("$1.00"));
}

TEST_CASE("Table without headers renders correctly", "[table][render]") {
  tbl::Table table;
  table.set_border_style(tbl::BorderStyle::ascii);
  table.add_row({"A", "B"});
  table.add_row({"C", "D"});

  std::string output = table.render();

  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("A"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("B"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("C"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("D"));
}

TEST_CASE("Table with only headers renders correctly", "[table][render]") {
  tbl::Table table;
  table.set_border_style(tbl::BorderStyle::ascii);
  table.set_headers({"Col1", "Col2"});

  std::string output = table.render();

  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("Col1"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("Col2"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Edge cases
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Table handles empty strings", "[table][edge]") {
  tbl::Table table;
  table.set_border_style(tbl::BorderStyle::ascii);
  table.add_row({"", "Value", ""});

  std::string output = table.render();
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("Value"));
}

TEST_CASE("Table handles uneven row lengths", "[table][edge]") {
  tbl::Table table;
  table.set_border_style(tbl::BorderStyle::ascii);
  table.set_headers({"A", "B", "C", "D"});
  table.add_row({"1"});
  table.add_row({"2", "3", "4", "5"});

  std::string output = table.render();
  // Should still render without crashing
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("A"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("1"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("5"));
}

TEST_CASE("Table handles special characters", "[table][edge]") {
  tbl::Table table;
  table.set_border_style(tbl::BorderStyle::ascii);
  table.add_row({"Hello\tWorld", "Line1\nLine2"});

  std::string output = table.render();
  // Tab and newline are preserved (though may not display well)
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("Hello"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("World"));
}

TEST_CASE("Table handles Unicode content", "[table][edge]") {
  tbl::Table table;
  table.set_border_style(tbl::BorderStyle::unicode);
  table.set_headers({"Emoji", "Text"});
  table.add_row({"Hello", "World"});

  std::string output = table.render();
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("Hello"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("World"));
}

TEST_CASE("Table handles long content", "[table][edge]") {
  tbl::Table table;
  table.set_border_style(tbl::BorderStyle::ascii);
  std::string long_string(100, 'x');
  table.add_row({long_string, "short"});

  std::string output = table.render();
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring(long_string));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("short"));
}

TEST_CASE("Table handles single cell", "[table][edge]") {
  tbl::Table table;
  table.set_border_style(tbl::BorderStyle::ascii);
  table.add_row({"Single"});

  std::string output = table.render();
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("Single"));
}

TEST_CASE("Table handles many columns", "[table][edge]") {
  tbl::Table table;
  table.set_border_style(tbl::BorderStyle::ascii);

  std::vector<std::string> row;
  for (int index = 0; index < 20; ++index) {
    row.push_back(std::to_string(index));
  }
  table.add_row(row);

  std::string output = table.render();
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("0"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("19"));
}

TEST_CASE("Table handles many rows", "[table][edge]") {
  tbl::Table table;
  table.set_border_style(tbl::BorderStyle::ascii);

  for (int index = 0; index < 50; ++index) {
    table.add_row({std::to_string(index)});
  }

  std::string output = table.render();
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("0"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("49"));
  REQUIRE(table.row_count() == 50);
}

// ─────────────────────────────────────────────────────────────────────────────
// Display width calculation (ANSI escape codes)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Table correctly calculates width with ANSI codes", "[table][width]") {
  tbl::Table table;
  table.set_border_style(tbl::BorderStyle::ascii);
  table.set_color_enabled(true);

  // Add a row with embedded ANSI codes
  std::string colored = std::string(tbl::Color::red) + "Red" + std::string(tbl::Color::reset);
  std::vector<tbl::Cell> row = {tbl::Cell(colored)};
  table.add_row(row);
  table.add_row({"Normal"});

  std::string output = table.render();
  // The column should be sized based on "Normal" (6 chars), not the ANSI codes
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("Red"));
  REQUIRE_THAT(output, Catch::Matchers::ContainsSubstring("Normal"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Rendering output structure verification
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Table output ends with newline", "[table][render]") {
  tbl::Table table;
  table.set_border_style(tbl::BorderStyle::ascii);
  table.add_row({"Test"});

  std::string output = table.render();
  REQUIRE(!output.empty());
  REQUIRE(output.back() == '\n');
}

TEST_CASE("Table with no borders still ends with newline", "[table][render]") {
  tbl::Table table;
  table.set_border_style(tbl::BorderStyle::none);
  table.add_row({"Test"});

  std::string output = table.render();
  REQUIRE(!output.empty());
  REQUIRE(output.back() == '\n');
}

TEST_CASE("Table render is idempotent", "[table][render]") {
  tbl::Table table;
  table.set_border_style(tbl::BorderStyle::unicode);
  table.set_headers({"A", "B"});
  table.add_row({"1", "2"});

  std::string output1 = table.render();
  std::string output2 = table.render();

  REQUIRE(output1 == output2);
}
