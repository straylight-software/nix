// straylight::nix::primitives::xml_writer
//
// Modern XML writer for generating well-formed XML output.
//
// Features:
// - Stream-based output to std::ostream (XmlWriter)
// - String-based output without sstream (StringXmlWriter)
// - Manual control via openElement()/closeElement()
// - RAII Element class for automatic closing
// - Self-closing empty elements
// - Text content with proper escaping
// - CDATA sections and comments
// - Configurable indentation
// - XML declaration generation
//
// Usage (StringXmlWriter - preferred, no sstream):
//   StringXmlWriter writer;
//   writer.writeDeclaration();
//   {
//     auto root = writer.element("root", {{"version", "1.0"}});
//     writer.writeText("Hello, world!");
//     {
//       auto child = writer.element("child");
//       writer.writeEmptyElement("empty", {{"attr", "value"}});
//     }
//   }
//   std::string result = writer.str();
//
// Usage (XmlWriter - for stream output):
//   XmlWriter writer(some_ostream);
//   writer.writeDeclaration();
//   // ...

#pragma once

#include <cstddef>
#include <map>
#include <ostream>
#include <stack>
#include <string>
#include <string_view>

namespace straylight::nix::text {

// ─────────────────────────────────────────────────────────────────────────────
// XmlAttrs type alias (matches nix/util/xml-writer.h)
// ─────────────────────────────────────────────────────────────────────────────

/// Attribute map for XML elements.
/// Uses std::less<> for heterogeneous lookup.
using XmlAttrs = std::map<std::string, std::string, std::less<>>;

// ─────────────────────────────────────────────────────────────────────────────
// XML escaping utilities
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

/// Escape special XML characters in text content.
/// Escapes: & < > " '
[[nodiscard]] inline std::string escape_xml(std::string_view text) {
  std::string result;
  result.reserve(text.size() * 1.1); // Slight overallocation for escapes

  for (char c : text) {
    switch (c) {
      case '&':
        result += "&amp;";
        break;
      case '<':
        result += "&lt;";
        break;
      case '>':
        result += "&gt;";
        break;
      case '"':
        result += "&quot;";
        break;
      case '\'':
        result += "&apos;";
        break;
      default:
        result += c;
        break;
    }
  }

  return result;
}

/// Escape text for use in attribute values.
/// Same as escape_xml but included for clarity.
[[nodiscard]] inline std::string escape_attr(std::string_view text) {
  return escape_xml(text);
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations
// ─────────────────────────────────────────────────────────────────────────────

class XmlWriter;
class StringXmlWriter;

// ─────────────────────────────────────────────────────────────────────────────
// Element (RAII wrapper for XmlWriter)
// ─────────────────────────────────────────────────────────────────────────────

/// RAII wrapper for automatic element closing.
/// When destroyed, closes the element that was opened on construction.
class Element {
public:
  Element(Element const&) = delete;
  Element& operator=(Element const&) = delete;

  Element(Element&& other) noexcept : writer_(other.writer_), active_(other.active_) {
    other.active_ = false;
  }

  Element& operator=(Element&& other) noexcept {
    if (this != &other) {
      close();
      writer_ = other.writer_;
      active_ = other.active_;
      other.active_ = false;
    }
    return *this;
  }

  ~Element() { close(); }

  /// Explicitly close the element (idempotent).
  void close();

private:
  friend class XmlWriter;

  explicit Element(XmlWriter& writer) : writer_(&writer), active_(true) {}

  XmlWriter* writer_;
  bool active_;
};

// ─────────────────────────────────────────────────────────────────────────────
// StringElement (RAII wrapper for StringXmlWriter)
// ─────────────────────────────────────────────────────────────────────────────

/// RAII wrapper for automatic element closing (StringXmlWriter version).
class StringElement {
public:
  StringElement(StringElement const&) = delete;
  StringElement& operator=(StringElement const&) = delete;

  StringElement(StringElement&& other) noexcept : writer_(other.writer_), active_(other.active_) {
    other.active_ = false;
  }

  StringElement& operator=(StringElement&& other) noexcept {
    if (this != &other) {
      close();
      writer_ = other.writer_;
      active_ = other.active_;
      other.active_ = false;
    }
    return *this;
  }

  ~StringElement() { close(); }

  /// Explicitly close the element (idempotent).
  void close();

private:
  friend class StringXmlWriter;

  explicit StringElement(StringXmlWriter& writer) : writer_(&writer), active_(true) {}

  StringXmlWriter* writer_;
  bool active_;
};

// ─────────────────────────────────────────────────────────────────────────────
// XmlWriter class
// ─────────────────────────────────────────────────────────────────────────────

/// XML writer for generating well-formed XML output.
class XmlWriter {
public:
  /// Construct a writer that outputs to the given stream.
  /// @param output The output stream to write to.
  /// @param indent Whether to indent nested elements (default: true).
  /// @param indent_str The string to use for each indentation level (default: 2 spaces).
  explicit XmlWriter(std::ostream& output, bool indent = true, std::string_view indent_str = "  ")
      : output_(output), indent_(indent), indent_str_(indent_str) {}

  // Non-copyable
  XmlWriter(XmlWriter const&) = delete;
  XmlWriter& operator=(XmlWriter const&) = delete;

  // Non-movable (owns reference to stream)
  XmlWriter(XmlWriter&&) = delete;
  XmlWriter& operator=(XmlWriter&&) = delete;

  ~XmlWriter() = default;

  // ─────────────────────────────────────────────────────────────────────────
  // XML Declaration
  // ─────────────────────────────────────────────────────────────────────────

  /// Write the XML declaration.
  /// @param version XML version (default: "1.0").
  /// @param encoding Character encoding (default: "UTF-8").
  /// @param standalone Standalone declaration (optional).
  void writeDeclaration(std::string_view version = "1.0", std::string_view encoding = "UTF-8",
                        std::string_view standalone = "") {
    output_ << "<?xml version=\"" << version << "\" encoding=\"" << encoding << "\"";
    if (!standalone.empty()) {
      output_ << " standalone=\"" << standalone << "\"";
    }
    output_ << "?>";
    if (indent_) {
      output_ << '\n';
    }
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Element management
  // ─────────────────────────────────────────────────────────────────────────

  /// Open an element manually.
  /// Must be paired with a corresponding closeElement() call.
  void openElement(std::string_view name, XmlAttrs const& attrs = {}) {
    writeIndent();
    output_ << '<' << name;
    writeAttrs(attrs);
    output_ << '>';
    if (indent_) {
      output_ << '\n';
    }
    element_stack_.push(std::string(name));
  }

  /// Close the most recently opened element.
  void closeElement() {
    if (element_stack_.empty()) {
      return;
    }
    std::string name = std::move(element_stack_.top());
    element_stack_.pop();
    writeIndent();
    output_ << "</" << name << '>';
    if (indent_) {
      output_ << '\n';
    }
  }

  /// Create an RAII element that closes automatically.
  /// @return An Element object that closes when destroyed.
  [[nodiscard]] Element element(std::string_view name, XmlAttrs const& attrs = {}) {
    openElement(name, attrs);
    return Element(*this);
  }

  /// Write a self-closing empty element.
  void writeEmptyElement(std::string_view name, XmlAttrs const& attrs = {}) {
    writeIndent();
    output_ << '<' << name;
    writeAttrs(attrs);
    output_ << " />";
    if (indent_) {
      output_ << '\n';
    }
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Content writing
  // ─────────────────────────────────────────────────────────────────────────

  /// Write escaped text content.
  void writeText(std::string_view content) {
    writeIndent();
    output_ << detail::escape_xml(content);
    if (indent_) {
      output_ << '\n';
    }
  }

  /// Write raw text content (no escaping, no indentation).
  /// Use with caution - caller is responsible for proper escaping.
  void writeRaw(std::string_view content) { output_ << content; }

  /// Write a CDATA section.
  /// Note: content must not contain "]]>" sequence.
  void writeCdata(std::string_view content) {
    writeIndent();
    output_ << "<![CDATA[" << content << "]]>";
    if (indent_) {
      output_ << '\n';
    }
  }

  /// Write an XML comment.
  /// Note: content must not contain "--" sequence.
  void writeComment(std::string_view content) {
    writeIndent();
    output_ << "<!-- " << content << " -->";
    if (indent_) {
      output_ << '\n';
    }
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Accessors
  // ─────────────────────────────────────────────────────────────────────────

  /// Get the current nesting depth.
  [[nodiscard]] std::size_t depth() const { return element_stack_.size(); }

  /// Check if indentation is enabled.
  [[nodiscard]] bool indenting() const { return indent_; }

  /// Get the underlying output stream.
  [[nodiscard]] std::ostream& stream() { return output_; }

private:
  void writeAttrs(XmlAttrs const& attrs) {
    for (auto const& [name, value] : attrs) {
      output_ << ' ' << name << "=\"" << detail::escape_attr(value) << '"';
    }
  }

  void writeIndent() {
    if (!indent_) {
      return;
    }
    for (std::size_t i = 0; i < element_stack_.size(); ++i) {
      output_ << indent_str_;
    }
  }

  std::ostream& output_;
  bool indent_;
  std::string indent_str_;
  std::stack<std::string> element_stack_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Element implementation (must come after XmlWriter definition)
// ─────────────────────────────────────────────────────────────────────────────

inline void Element::close() {
  if (active_ && writer_ != nullptr) {
    writer_->closeElement();
    active_ = false;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// StringXmlWriter class (sstream-free alternative)
// ─────────────────────────────────────────────────────────────────────────────

/// XML writer that outputs directly to a std::string.
/// Use this instead of XmlWriter + std::ostringstream to avoid sstream overhead.
class StringXmlWriter {
public:
  /// Construct a writer that outputs to an internal string.
  /// @param indent Whether to indent nested elements (default: true).
  /// @param indent_str The string to use for each indentation level (default: 2 spaces).
  explicit StringXmlWriter(bool indent = true, std::string_view indent_str = "  ")
      : indent_(indent), indent_str_(indent_str) {}

  // Non-copyable
  StringXmlWriter(StringXmlWriter const&) = delete;
  StringXmlWriter& operator=(StringXmlWriter const&) = delete;

  // Movable
  StringXmlWriter(StringXmlWriter&&) = default;
  StringXmlWriter& operator=(StringXmlWriter&&) = default;

  ~StringXmlWriter() = default;

  // ─────────────────────────────────────────────────────────────────────────
  // XML Declaration
  // ─────────────────────────────────────────────────────────────────────────

  /// Write the XML declaration.
  void writeDeclaration(std::string_view version = "1.0", std::string_view encoding = "UTF-8",
                        std::string_view standalone = "") {
    output_ += "<?xml version=\"";
    output_ += version;
    output_ += "\" encoding=\"";
    output_ += encoding;
    output_ += "\"";
    if (!standalone.empty()) {
      output_ += " standalone=\"";
      output_ += standalone;
      output_ += "\"";
    }
    output_ += "?>";
    if (indent_) {
      output_ += '\n';
    }
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Element management
  // ─────────────────────────────────────────────────────────────────────────

  /// Open an element manually.
  void openElement(std::string_view name, XmlAttrs const& attrs = {}) {
    writeIndent();
    output_ += '<';
    output_ += name;
    writeAttrs(attrs);
    output_ += '>';
    if (indent_) {
      output_ += '\n';
    }
    element_stack_.push(std::string(name));
  }

  /// Close the most recently opened element.
  void closeElement() {
    if (element_stack_.empty()) {
      return;
    }
    std::string name = std::move(element_stack_.top());
    element_stack_.pop();
    writeIndent();
    output_ += "</";
    output_ += name;
    output_ += '>';
    if (indent_) {
      output_ += '\n';
    }
  }

  /// Create an RAII element that closes automatically.
  [[nodiscard]] StringElement element(std::string_view name, XmlAttrs const& attrs = {}) {
    openElement(name, attrs);
    return StringElement(*this);
  }

  /// Write a self-closing empty element.
  void writeEmptyElement(std::string_view name, XmlAttrs const& attrs = {}) {
    writeIndent();
    output_ += '<';
    output_ += name;
    writeAttrs(attrs);
    output_ += " />";
    if (indent_) {
      output_ += '\n';
    }
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Content writing
  // ─────────────────────────────────────────────────────────────────────────

  /// Write escaped text content.
  void writeText(std::string_view content) {
    writeIndent();
    output_ += detail::escape_xml(content);
    if (indent_) {
      output_ += '\n';
    }
  }

  /// Write raw text content (no escaping, no indentation).
  void writeRaw(std::string_view content) { output_ += content; }

  /// Write a CDATA section.
  void writeCdata(std::string_view content) {
    writeIndent();
    output_ += "<![CDATA[";
    output_ += content;
    output_ += "]]>";
    if (indent_) {
      output_ += '\n';
    }
  }

  /// Write an XML comment.
  void writeComment(std::string_view content) {
    writeIndent();
    output_ += "<!-- ";
    output_ += content;
    output_ += " -->";
    if (indent_) {
      output_ += '\n';
    }
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Accessors
  // ─────────────────────────────────────────────────────────────────────────

  /// Get the current nesting depth.
  [[nodiscard]] auto depth() const -> std::size_t { return element_stack_.size(); }

  /// Check if indentation is enabled.
  [[nodiscard]] auto indenting() const -> bool { return indent_; }

  /// Get the output string.
  [[nodiscard]] auto str() const& -> std::string const& { return output_; }

  /// Move the output string out.
  [[nodiscard]] auto str() && -> std::string { return std::move(output_); }

  /// Direct access to output string for appending.
  [[nodiscard]] auto output() -> std::string& { return output_; }

private:
  void writeAttrs(XmlAttrs const& attrs) {
    for (auto const& [name, value] : attrs) {
      output_ += ' ';
      output_ += name;
      output_ += "=\"";
      output_ += detail::escape_attr(value);
      output_ += '"';
    }
  }

  void writeIndent() {
    if (!indent_) {
      return;
    }
    for (std::size_t i = 0; i < element_stack_.size(); ++i) {
      output_ += indent_str_;
    }
  }

  std::string output_;
  bool indent_;
  std::string indent_str_;
  std::stack<std::string> element_stack_;
};

// ─────────────────────────────────────────────────────────────────────────────
// StringElement implementation
// ─────────────────────────────────────────────────────────────────────────────

inline void StringElement::close() {
  if (active_ && writer_ != nullptr) {
    writer_->closeElement();
    active_ = false;
  }
}

} // namespace straylight::nix::text
