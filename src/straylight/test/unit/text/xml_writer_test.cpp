// straylight::nix::text::xml_writer tests
//
// Unit tests for the XML writer primitive.
// Uses StringXmlWriter to avoid sstream overhead.

#include <string>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include <straylight/nix/text/xml_writer.h>
namespace xml = straylight::nix::text;

// ─────────────────────────────────────────────────────────────────────────────
// XML Declaration Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("xml declaration default", "[xml_writer][declaration]") {
  xml::StringXmlWriter writer;
  writer.writeDeclaration();

  REQUIRE(writer.str() == "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
}

TEST_CASE("xml declaration custom version and encoding", "[xml_writer][declaration]") {
  xml::StringXmlWriter writer;
  writer.writeDeclaration("1.1", "ISO-8859-1");

  REQUIRE(writer.str() == "<?xml version=\"1.1\" encoding=\"ISO-8859-1\"?>\n");
}

TEST_CASE("xml declaration with standalone", "[xml_writer][declaration]") {
  xml::StringXmlWriter writer;
  writer.writeDeclaration("1.0", "UTF-8", "yes");

  REQUIRE(writer.str() == "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n");
}

TEST_CASE("xml declaration no indent", "[xml_writer][declaration]") {
  xml::StringXmlWriter writer(false);
  writer.writeDeclaration();

  REQUIRE(writer.str() == "<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
}

// ─────────────────────────────────────────────────────────────────────────────
// Element Tests (manual open/close)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("simple element open/close", "[xml_writer][element]") {
  xml::StringXmlWriter writer;

  writer.openElement("root");
  writer.closeElement();

  REQUIRE(writer.str() == "<root>\n</root>\n");
}

TEST_CASE("nested elements", "[xml_writer][element]") {
  xml::StringXmlWriter writer;

  writer.openElement("root");
  writer.openElement("child");
  writer.closeElement();
  writer.closeElement();

  REQUIRE(writer.str() == "<root>\n  <child>\n  </child>\n</root>\n");
}

TEST_CASE("element with attributes", "[xml_writer][element][attrs]") {
  xml::StringXmlWriter writer;

  xml::XmlAttrs attrs = {{"name", "value"}, {"foo", "bar"}};
  writer.openElement("element", attrs);
  writer.closeElement();

  // Note: std::map is sorted, so "foo" comes before "name"
  REQUIRE(writer.str() == "<element foo=\"bar\" name=\"value\">\n</element>\n");
}

TEST_CASE("element with no indent", "[xml_writer][element]") {
  xml::StringXmlWriter writer(false);

  writer.openElement("root");
  writer.openElement("child");
  writer.closeElement();
  writer.closeElement();

  REQUIRE(writer.str() == "<root><child></child></root>");
}

TEST_CASE("element with custom indent", "[xml_writer][element]") {
  xml::StringXmlWriter writer(true, "\t");

  writer.openElement("root");
  writer.openElement("child");
  writer.closeElement();
  writer.closeElement();

  REQUIRE(writer.str() == "<root>\n\t<child>\n\t</child>\n</root>\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// RAII Element Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("RAII element auto close", "[xml_writer][element][raii]") {
  xml::StringXmlWriter writer;

  {
    auto root = writer.element("root");
    // root automatically closed here
  }

  REQUIRE(writer.str() == "<root>\n</root>\n");
}

TEST_CASE("RAII nested elements", "[xml_writer][element][raii]") {
  xml::StringXmlWriter writer;

  {
    auto root = writer.element("root");
    {
      auto child = writer.element("child");
    }
  }

  REQUIRE(writer.str() == "<root>\n  <child>\n  </child>\n</root>\n");
}

TEST_CASE("RAII element with attrs", "[xml_writer][element][raii][attrs]") {
  xml::StringXmlWriter writer;

  {
    auto root = writer.element("root", {{"version", "1.0"}});
  }

  REQUIRE(writer.str() == "<root version=\"1.0\">\n</root>\n");
}

TEST_CASE("RAII element explicit close", "[xml_writer][element][raii]") {
  xml::StringXmlWriter writer;

  auto root = writer.element("root");
  root.close();
  // Second close should be no-op
  root.close();

  REQUIRE(writer.str() == "<root>\n</root>\n");
}

TEST_CASE("RAII element move semantics", "[xml_writer][element][raii]") {
  xml::StringXmlWriter writer;

  {
    auto root = writer.element("root");
    auto moved = std::move(root);
    // moved should close, root should not
  }

  REQUIRE(writer.str() == "<root>\n</root>\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Empty Element Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("empty element", "[xml_writer][element][empty]") {
  xml::StringXmlWriter writer;

  writer.writeEmptyElement("empty");

  REQUIRE(writer.str() == "<empty />\n");
}

TEST_CASE("empty element with attrs", "[xml_writer][element][empty][attrs]") {
  xml::StringXmlWriter writer;

  writer.writeEmptyElement("br", {{"class", "clear"}});

  REQUIRE(writer.str() == "<br class=\"clear\" />\n");
}

TEST_CASE("empty element no indent", "[xml_writer][element][empty]") {
  xml::StringXmlWriter writer(false);

  writer.writeEmptyElement("empty");

  REQUIRE(writer.str() == "<empty />");
}

TEST_CASE("empty element nested", "[xml_writer][element][empty]") {
  xml::StringXmlWriter writer;

  {
    auto root = writer.element("root");
    writer.writeEmptyElement("item", {{"id", "1"}});
    writer.writeEmptyElement("item", {{"id", "2"}});
  }

  REQUIRE(writer.str() == "<root>\n  <item id=\"1\" />\n  <item id=\"2\" />\n</root>\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Text Content Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("text content", "[xml_writer][text]") {
  xml::StringXmlWriter writer;

  {
    auto root = writer.element("root");
    writer.writeText("Hello, world!");
  }

  REQUIRE(writer.str() == "<root>\n  Hello, world!\n</root>\n");
}

TEST_CASE("text content escaping", "[xml_writer][text][escape]") {
  xml::StringXmlWriter writer;

  {
    auto root = writer.element("root");
    writer.writeText("<script>alert('XSS');</script>");
  }

  REQUIRE(writer.str() ==
          "<root>\n  &lt;script&gt;alert(&apos;XSS&apos;);&lt;/script&gt;\n</root>\n");
}

TEST_CASE("text content ampersand escaping", "[xml_writer][text][escape]") {
  xml::StringXmlWriter writer;

  {
    auto root = writer.element("root");
    writer.writeText("AT&T");
  }

  REQUIRE(writer.str() == "<root>\n  AT&amp;T\n</root>\n");
}

TEST_CASE("text content quote escaping", "[xml_writer][text][escape]") {
  xml::StringXmlWriter writer;

  {
    auto root = writer.element("root");
    writer.writeText("He said \"hello\" and 'goodbye'");
  }

  REQUIRE(writer.str() == "<root>\n  He said &quot;hello&quot; and &apos;goodbye&apos;\n</root>\n");
}

TEST_CASE("text content all special chars", "[xml_writer][text][escape]") {
  xml::StringXmlWriter writer;

  writer.writeText("<>&\"'");

  REQUIRE(writer.str() == "&lt;&gt;&amp;&quot;&apos;\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Attribute Escaping Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("attribute escaping", "[xml_writer][attrs][escape]") {
  xml::StringXmlWriter writer;

  writer.writeEmptyElement("element", {{"data", "a<b>c&d\"e'f"}});

  REQUIRE(writer.str() == "<element data=\"a&lt;b&gt;c&amp;d&quot;e&apos;f\" />\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// CDATA Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("cdata section", "[xml_writer][cdata]") {
  xml::StringXmlWriter writer;

  {
    auto root = writer.element("root");
    writer.writeCdata("function() { return x < y && y > z; }");
  }

  REQUIRE(writer.str() == "<root>\n  <![CDATA[function() { return x < y && y > z; }]]>\n</root>\n");
}

TEST_CASE("cdata no escaping", "[xml_writer][cdata]") {
  xml::StringXmlWriter writer;

  writer.writeCdata("<>&\"'");

  // CDATA content is NOT escaped
  REQUIRE(writer.str() == "<![CDATA[<>&\"']]>\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Comment Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("comment", "[xml_writer][comment]") {
  xml::StringXmlWriter writer;

  writer.writeComment("This is a comment");

  REQUIRE(writer.str() == "<!-- This is a comment -->\n");
}

TEST_CASE("comment nested", "[xml_writer][comment]") {
  xml::StringXmlWriter writer;

  {
    auto root = writer.element("root");
    writer.writeComment("Inner comment");
  }

  REQUIRE(writer.str() == "<root>\n  <!-- Inner comment -->\n</root>\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Raw Content Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("raw content", "[xml_writer][raw]") {
  xml::StringXmlWriter writer;

  writer.writeRaw("<?processing instruction?>");

  REQUIRE(writer.str() == "<?processing instruction?>");
}

// ─────────────────────────────────────────────────────────────────────────────
// Depth and State Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("depth tracking", "[xml_writer][depth]") {
  xml::StringXmlWriter writer;

  REQUIRE(writer.depth() == 0);

  writer.openElement("level1");
  REQUIRE(writer.depth() == 1);

  writer.openElement("level2");
  REQUIRE(writer.depth() == 2);

  writer.openElement("level3");
  REQUIRE(writer.depth() == 3);

  writer.closeElement();
  REQUIRE(writer.depth() == 2);

  writer.closeElement();
  REQUIRE(writer.depth() == 1);

  writer.closeElement();
  REQUIRE(writer.depth() == 0);
}

TEST_CASE("indenting accessor", "[xml_writer]") {
  xml::StringXmlWriter writer1(true);
  REQUIRE(writer1.indenting() == true);

  xml::StringXmlWriter writer2(false);
  REQUIRE(writer2.indenting() == false);
}

TEST_CASE("output accessor", "[xml_writer]") {
  xml::StringXmlWriter writer;

  writer.output() += "direct output";

  REQUIRE(writer.str() == "direct output");
}

// ─────────────────────────────────────────────────────────────────────────────
// Complex Document Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("complete xml document", "[xml_writer][integration]") {
  xml::StringXmlWriter writer;

  writer.writeDeclaration();
  writer.writeComment("Sample document");
  {
    auto root = writer.element("catalog", {{"version", "1.0"}});
    {
      auto item = writer.element("item", {{"id", "1"}});
      writer.writeText("First item");
    }
    {
      auto item = writer.element("item", {{"id", "2"}});
      writer.writeText("Second item");
    }
    writer.writeEmptyElement("separator");
    {
      auto script = writer.element("script");
      writer.writeCdata("if (a < b) { return true; }");
    }
  }

  std::string expected = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                         "<!-- Sample document -->\n"
                         "<catalog version=\"1.0\">\n"
                         "  <item id=\"1\">\n"
                         "    First item\n"
                         "  </item>\n"
                         "  <item id=\"2\">\n"
                         "    Second item\n"
                         "  </item>\n"
                         "  <separator />\n"
                         "  <script>\n"
                         "    <![CDATA[if (a < b) { return true; }]]>\n"
                         "  </script>\n"
                         "</catalog>\n";

  REQUIRE(writer.str() == expected);
}

TEST_CASE("deeply nested structure", "[xml_writer][integration]") {
  xml::StringXmlWriter writer;

  {
    auto a = writer.element("a");
    {
      auto b = writer.element("b");
      {
        auto c = writer.element("c");
        {
          auto d = writer.element("d");
          writer.writeText("deep");
        }
      }
    }
  }

  std::string expected = "<a>\n"
                         "  <b>\n"
                         "    <c>\n"
                         "      <d>\n"
                         "        deep\n"
                         "      </d>\n"
                         "    </c>\n"
                         "  </b>\n"
                         "</a>\n";

  REQUIRE(writer.str() == expected);
}

TEST_CASE("mixed content", "[xml_writer][integration]") {
  xml::StringXmlWriter writer(false); // No indent for mixed content

  writer.openElement("p");
  writer.writeRaw("Text with ");
  writer.openElement("em");
  writer.writeRaw("emphasis");
  writer.closeElement();
  writer.writeRaw(" and ");
  writer.openElement("strong");
  writer.writeRaw("strong");
  writer.closeElement();
  writer.writeRaw(" words.");
  writer.closeElement();

  REQUIRE(writer.str() == "<p>Text with <em>emphasis</em> and <strong>strong</strong> words.</p>");
}

// ─────────────────────────────────────────────────────────────────────────────
// Edge Cases
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("close element when empty", "[xml_writer][edge]") {
  xml::StringXmlWriter writer;

  // Should not crash or output anything
  writer.closeElement();
  writer.closeElement();

  REQUIRE(writer.str().empty());
}

TEST_CASE("empty text", "[xml_writer][edge]") {
  xml::StringXmlWriter writer;

  writer.writeText("");

  REQUIRE(writer.str() == "\n");
}

TEST_CASE("empty cdata", "[xml_writer][edge]") {
  xml::StringXmlWriter writer;

  writer.writeCdata("");

  REQUIRE(writer.str() == "<![CDATA[]]>\n");
}

TEST_CASE("empty comment", "[xml_writer][edge]") {
  xml::StringXmlWriter writer;

  writer.writeComment("");

  REQUIRE(writer.str() == "<!--  -->\n");
}

TEST_CASE("element with empty name", "[xml_writer][edge]") {
  xml::StringXmlWriter writer;

  // XML with empty element name is technically invalid but we don't validate
  writer.openElement("");
  writer.closeElement();

  REQUIRE(writer.str() == "<>\n</>\n");
}

TEST_CASE("unicode content", "[xml_writer][edge]") {
  xml::StringXmlWriter writer;

  {
    auto root = writer.element("root");
    writer.writeText("Hello, World!"); // Chinese characters
  }

  // UTF-8 should pass through unchanged
  REQUIRE(writer.str() == "<root>\n  Hello, World!\n</root>\n");
}

TEST_CASE("many siblings", "[xml_writer][edge]") {
  xml::StringXmlWriter writer(false);

  writer.openElement("root");
  for (int i = 0; i < 100; ++i) {
    writer.writeEmptyElement("item");
  }
  writer.closeElement();

  // Just verify it compiles and runs without crashing
  REQUIRE(writer.depth() == 0);
  REQUIRE(writer.str().starts_with("<root>"));
  REQUIRE(writer.str().ends_with("</root>"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Escaping detail function tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("escape_xml function", "[xml_writer][escape][detail]") {
  using xml::detail::escape_xml;

  REQUIRE(escape_xml("") == "");
  REQUIRE(escape_xml("hello") == "hello");
  REQUIRE(escape_xml("<") == "&lt;");
  REQUIRE(escape_xml(">") == "&gt;");
  REQUIRE(escape_xml("&") == "&amp;");
  REQUIRE(escape_xml("\"") == "&quot;");
  REQUIRE(escape_xml("'") == "&apos;");
  REQUIRE(escape_xml("<>&\"'") == "&lt;&gt;&amp;&quot;&apos;");
  REQUIRE(escape_xml("a<b>c&d\"e'f") == "a&lt;b&gt;c&amp;d&quot;e&apos;f");
  REQUIRE(escape_xml("no special chars here") == "no special chars here");
}
