#include "nix/expr/value-to-xml.h"

#include <cstdlib>

#include "nix/expr/eval-inline.h"
#include "nix/util/signals.h"
#include "nix/util/string-ostream.h"
#include "nix/util/xml-writer.h"

namespace nix {

static xml_attrs_t singleton_attrs(const std::string& name, std::string_view value) {
  xml_attrs_t attrs;
  attrs[name] = value;
  return attrs;
}

static void print_value_as_xml(eval_state_t& state, bool strict, bool location, value_t& v,
                               xml_writer_t& doc, NixStringContext& context, path_set_t& drvs_seen,
                               const pos_idx_t pos);

static void pos_to_xml(eval_state_t& state, xml_attrs_t& xml_attrs, const pos_t& pos) {
  if (auto path = std::get_if<source_path_t>(&pos.origin)) {
    xml_attrs["path"] = path->path.abs();
  }
  xml_attrs["line"] = fmt("%1%", pos.line);
  xml_attrs["column"] = fmt("%1%", pos.column);
}

static void show_attrs(eval_state_t& state, bool strict, bool location, const bindings_t& attrs,
                       xml_writer_t& doc, NixStringContext& context, path_set_t& drvs_seen) {
  string_set_t names;

  for (auto& a : attrs.lexicographicOrder(state.symbols)) {
    xml_attrs_t xml_attrs;
    xml_attrs["name"] = state.symbols[a->name];
    if (location && a->pos) {
      pos_to_xml(state, xml_attrs, state.positions[a->pos]);
    }

    xml_open_element_t _(doc, "attr", xml_attrs);
    print_value_as_xml(state, strict, location, *a->value, doc, context, drvs_seen, a->pos);
  }
}

static void print_value_as_xml(eval_state_t& state, bool strict, bool location, value_t& v,
                               xml_writer_t& doc, NixStringContext& context, path_set_t& drvs_seen,
                               const pos_idx_t pos) {
  check_interrupt();

  if (strict) {
    state.forceValue(v, pos);
  }

  switch (v.type()) {
    case nInt:
      doc.write_empty_element("int", singleton_attrs("value", fmt("%1%", v.integer())));
      break;

    case nBool:
      doc.write_empty_element("bool", singleton_attrs("value", v.boolean() ? "true" : "false"));
      break;

    case nString:
      /* !!! show the context? */
      copy_context(v, context);
      doc.write_empty_element("string", singleton_attrs("value", v.string_view()));
      break;

    case nPath:
      doc.write_empty_element("path", singleton_attrs("value", v.path().to_string()));
      break;

    case nNull:
      doc.write_empty_element("null");
      break;

    case nAttrs:
      if (state.is_derivation(v)) {
        xml_attrs_t xml_attrs;

        Path drv_path;
        if (auto a = v.attrs()->get(state.s.drv_path)) {
          if (strict) {
            state.forceValue(*a->value, a->pos);
          }
          if (a->value->type() == nString) {
            xml_attrs["drvPath"] = drv_path = a->value->string_view();
          }
        }

        if (auto a = v.attrs()->get(state.s.out_path)) {
          if (strict) {
            state.forceValue(*a->value, a->pos);
          }
          if (a->value->type() == nString) {
            xml_attrs["outPath"] = a->value->string_view();
          }
        }

        xml_open_element_t _(doc, "derivation", xml_attrs);

        if (drv_path != "" && drvs_seen.insert(drv_path).second) {
          show_attrs(state, strict, location, *v.attrs(), doc, context, drvs_seen);
        } else {
          doc.write_empty_element("repeated");
        }
      }

      else {
        xml_open_element_t _(doc, "attrs");
        show_attrs(state, strict, location, *v.attrs(), doc, context, drvs_seen);
      }

      break;

    case nList: {
      xml_open_element_t _(doc, "list");
      for (auto v2 : v.list_view()) {
        print_value_as_xml(state, strict, location, *v2, doc, context, drvs_seen, pos);
      }
      break;
    }

    case nFunction: {
      if (!v.isLambda()) {
        // FIXME: Serialize primops and primopapps
        doc.write_empty_element("unevaluated");
        break;
      }
      xml_attrs_t xml_attrs;
      if (location) {
        pos_to_xml(state, xml_attrs, state.positions[v.lambda().fun->pos]);
      }
      xml_open_element_t _(doc, "function", xml_attrs);

      if (auto formals = v.lambda().fun->getFormals()) {
        xml_attrs_t attrs;
        if (v.lambda().fun->arg) {
          attrs["name"] = state.symbols[v.lambda().fun->arg];
        }
        if (formals->ellipsis) {
          attrs["ellipsis"] = "1";
        }
        xml_open_element_t _(doc, "attrspat", attrs);
        for (auto& i : formals->lexicographicOrder(state.symbols)) {
          doc.write_empty_element("attr", singleton_attrs("name", state.symbols[i.name]));
        }
      } else {
        doc.write_empty_element("varpat",
                                singleton_attrs("name", state.symbols[v.lambda().fun->arg]));
      }

      break;
    }

    case nExternal:
      v.external()->print_value_as_xml(state, strict, location, doc, context, drvs_seen, pos);
      break;

    case nFloat:
      doc.write_empty_element("float", singleton_attrs("value", fmt("%1%", v.fpoint())));
      break;

    case nThunk:
      doc.write_empty_element("unevaluated");
      break;

    case nFailed:
      doc.write_empty_element("failed");
      break;
  }
}

void ExternalValueBase::print_value_as_xml(eval_state_t& state, bool strict, bool location,
                                           xml_writer_t& doc, NixStringContext& context,
                                           path_set_t& drvs_seen, const pos_idx_t pos) const {
  doc.write_empty_element("unevaluated");
}

void print_value_as_xml(eval_state_t& state, bool strict, bool location, value_t& v,
                        std::ostream& out, NixStringContext& context, const pos_idx_t pos) {
  xml_writer_t doc(true, out);
  xml_open_element_t root(doc, "expr");
  path_set_t drvs_seen;
  print_value_as_xml(state, strict, location, v, doc, context, drvs_seen, pos);
}

std::string print_value_as_xml_string(eval_state_t& state, bool strict, bool location, value_t& v,
                                      NixStringContext& context, const pos_idx_t pos) {
  string_ostream_t out;
  print_value_as_xml(state, strict, location, v, out, context, pos);
  return out.take();
}

} // namespace nix
