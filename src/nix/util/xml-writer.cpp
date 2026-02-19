#include "nix/util/xml-writer.h"

#include <cassert>

namespace nix {

xml_writer_t::xml_writer_t(bool indent, std::ostream& output) : output(output), indent(indent) {
  output << "<?xml version='1.0' encoding='utf-8'?>" << std::endl;
  closed = false;
}

xml_writer_t::~xml_writer_t() {
  close();
}

void xml_writer_t::close() {
  if (closed)
    return;
  while (!pendingElems.empty())
    closeElement();
  closed = true;
}

void xml_writer_t::indent_(size_t depth) {
  if (!indent)
    return;
  output << std::string(depth * 2, ' ');
}

void xml_writer_t::openElement(std::string_view name, const xml_attrs_t& attrs) {
  assert(!closed);
  indent_(pendingElems.size());
  output << "<" << name;
  writeAttrs(attrs);
  output << ">";
  if (indent)
    output << std::endl;
  pendingElems.push_back(std::string(name));
}

void xml_writer_t::closeElement() {
  assert(!pendingElems.empty());
  indent_(pendingElems.size() - 1);
  output << "</" << pendingElems.back() << ">";
  if (indent)
    output << std::endl;
  pendingElems.pop_back();
  if (pendingElems.empty())
    closed = true;
}

void xml_writer_t::writeEmptyElement(std::string_view name, const xml_attrs_t& attrs) {
  assert(!closed);
  indent_(pendingElems.size());
  output << "<" << name;
  writeAttrs(attrs);
  output << " />";
  if (indent)
    output << std::endl;
}

void xml_writer_t::writeAttrs(const xml_attrs_t& attrs) {
  for (auto& i : attrs) {
    output << " " << i.first << "=\"";
    for (size_t j = 0; j < i.second.size(); ++j) {
      char c = i.second[j];
      if (c == '"')
        output << "&quot;";
      else if (c == '<')
        output << "&lt;";
      else if (c == '>')
        output << "&gt;";
      else if (c == '&')
        output << "&amp;";
      /* Escape newlines to prevent attribute normalisation (see
         XML spec, section 3.3.3. */
      else if (c == '\n')
        output << "&#xA;";
      else
        output << c;
    }
    output << "\"";
  }
}

} // namespace nix
