#pragma once
///@file

#include <iostream>
#include <list>
#include <map>
#include <string>

namespace nix {

typedef std::map<std::string, std::string, std::less<>> xml_attrs_t;

class xml_writer_t {
private:
  std::ostream& output;

  bool indent;
  bool closed;

  std::list<std::string> pendingElems;

public:
  xml_writer_t(bool indent, std::ostream& output);
  ~xml_writer_t();

  void close();

  void openElement(std::string_view name, const xml_attrs_t& attrs = xml_attrs_t());
  void closeElement();

  void writeEmptyElement(std::string_view name, const xml_attrs_t& attrs = xml_attrs_t());

private:
  void writeAttrs(const xml_attrs_t& attrs);

  void indent_(size_t depth);
};

class xml_open_element_t {
private:
  xml_writer_t& writer;

public:
  xml_open_element_t(xml_writer_t& writer, std::string_view name, const xml_attrs_t& attrs = xml_attrs_t())
      : writer(writer) {
    writer.openElement(name, attrs);
  }

  ~xml_open_element_t() { writer.closeElement(); }
};

} // namespace nix
