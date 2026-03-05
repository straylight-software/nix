/**
 * @file value.cpp
 * @brief Value inspection and construction for Nix FFI.
 */

#include <nix/store/build-result.h>

#include "internal.h"

extern "C" {

NixValueType nix_value_type(const NixValue* value) {
  if (!value || !value->value) {
    return NIX_TYPE_FAILED;
  }

  switch (value->value->type()) {
    case nix::ValueType::nThunk:
      return NIX_TYPE_THUNK;
    case nix::ValueType::nInt:
      return NIX_TYPE_INT;
    case nix::ValueType::nFloat:
      return NIX_TYPE_FLOAT;
    case nix::ValueType::nBool:
      return NIX_TYPE_BOOL;
    case nix::ValueType::nString:
      return NIX_TYPE_STRING;
    case nix::ValueType::nPath:
      return NIX_TYPE_PATH;
    case nix::ValueType::nNull:
      return NIX_TYPE_NULL;
    case nix::ValueType::nAttrs:
      return NIX_TYPE_ATTRS;
    case nix::ValueType::nList:
      return NIX_TYPE_LIST;
    case nix::ValueType::nFunction:
      return NIX_TYPE_FUNCTION;
    case nix::ValueType::nExternal:
      return NIX_TYPE_EXTERNAL;
    case nix::ValueType::nFailed:
      return NIX_TYPE_FAILED;
    default:
      return NIX_TYPE_FAILED;
  }
}

bool nix_value_is_thunk(const NixValue* value) {
  return value && value->value && value->value->type() == nix::ValueType::nThunk;
}

NixError nix_value_get_int(const NixValue* value, int64_t* out) {
  if (!value || !out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  if (value->value->type() != nix::ValueType::nInt) {
    return nix::ffi::set_error(NIX_ERR_TYPE, "expected int");
  }

  *out = static_cast<int64_t>(value->value->integer());
  return NIX_OK;
}

NixError nix_value_get_float(const NixValue* value, double* out) {
  if (!value || !out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  if (value->value->type() != nix::ValueType::nFloat) {
    return nix::ffi::set_error(NIX_ERR_TYPE, "expected float");
  }

  *out = value->value->fpoint();
  return NIX_OK;
}

NixError nix_value_get_bool(const NixValue* value, bool* out) {
  if (!value || !out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  if (value->value->type() != nix::ValueType::nBool) {
    return nix::ffi::set_error(NIX_ERR_TYPE, "expected bool");
  }

  *out = value->value->boolean();
  return NIX_OK;
}

NixError nix_value_get_string(const NixValue* value, NixString* out) {
  if (!value || !out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  if (value->value->type() != nix::ValueType::nString) {
    return nix::ffi::set_error(NIX_ERR_TYPE, "expected string");
  }

  nix::ffi::string_set(out, std::string(value->value->string_view()));
  return NIX_OK;
}

NixError nix_value_get_path(const NixValue* value, NixString* out) {
  if (!value || !out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  if (value->value->type() != nix::ValueType::nPath) {
    return nix::ffi::set_error(NIX_ERR_TYPE, "expected path");
  }

  nix::ffi::string_set(out, value->value->path().to_string());
  return NIX_OK;
}

NixError nix_value_get_list_length(const NixValue* value, size_t* out) {
  if (!value || !out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  if (value->value->type() != nix::ValueType::nList) {
    return nix::ffi::set_error(NIX_ERR_TYPE, "expected list");
  }

  *out = value->value->list_size();
  return NIX_OK;
}

NixError nix_value_get_list_elem(const NixValue* value, size_t index, NixValue** elem_out) {
  if (!value || !elem_out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  if (value->value->type() != nix::ValueType::nList) {
    return nix::ffi::set_error(NIX_ERR_TYPE, "expected list");
  }

  if (index >= value->value->list_size()) {
    return nix::ffi::set_error(NIX_ERR_OVERFLOW, "index out of bounds");
  }

  auto list = value->value->list_view();
  *elem_out = new NixValue{list[index], value->state_ref};
  return NIX_OK;
}

NixError nix_value_get_attrs_size(const NixValue* value, size_t* out) {
  if (!value || !out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  if (value->value->type() != nix::ValueType::nAttrs) {
    return nix::ffi::set_error(NIX_ERR_TYPE, "expected attrs");
  }

  *out = value->value->attrs()->size();
  return NIX_OK;
}

NixError nix_value_get_attr(NixEvalState* state, const NixValue* value, const char* name,
                            NixValue** attr_out) {
  if (!state || !value || !name || !attr_out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  if (value->value->type() != nix::ValueType::nAttrs) {
    return nix::ffi::set_error(NIX_ERR_TYPE, "expected attrs");
  }

  return nix::ffi::catch_errors([&] {
    auto sym = state->state->symbols.create(name);
    auto* attr = value->value->attrs()->get(sym);
    if (!attr) {
      return nix::ffi::set_error(NIX_ERR_NOT_FOUND, "attribute not found");
    }

    *attr_out = new NixValue{attr->value, state};
    return NIX_OK;
  });
}

NixError nix_value_has_attr(NixEvalState* state, const NixValue* value, const char* name,
                            bool* exists_out) {
  if (!state || !value || !name || !exists_out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  if (value->value->type() != nix::ValueType::nAttrs) {
    return nix::ffi::set_error(NIX_ERR_TYPE, "expected attrs");
  }

  return nix::ffi::catch_errors([&] {
    auto sym = state->state->symbols.create(name);
    *exists_out = value->value->attrs()->get(sym) != nullptr;
    return NIX_OK;
  });
}

NixError nix_value_get_attr_names(const NixValue* value, NixString** names_out, size_t* count_out) {
  if (!value || !names_out || !count_out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  if (value->value->type() != nix::ValueType::nAttrs) {
    return nix::ffi::set_error(NIX_ERR_TYPE, "expected attrs");
  }

  return nix::ffi::catch_errors([&] {
    auto* attrs = value->value->attrs();
    *count_out = attrs->size();

    if (attrs->empty()) {
      *names_out = nullptr;
      return NIX_OK;
    }

    auto* names = new NixString[attrs->size()];
    size_t i = 0;
    for (const auto& attr : *attrs) {
      nix::ffi::string_set(&names[i++], std::string(value->state_ref->state->symbols[attr.name]));
    }
    *names_out = names;
    return NIX_OK;
  });
}

NixError nix_value_iter_attrs(const NixValue* value, NixAttrIterCallback callback,
                              void* user_data) {
  if (!value || !callback) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  if (value->value->type() != nix::ValueType::nAttrs) {
    return nix::ffi::set_error(NIX_ERR_TYPE, "expected attrs");
  }

  return nix::ffi::catch_errors([&] {
    auto* attrs = value->value->attrs();
    for (const auto& attr : *attrs) {
      auto name = value->state_ref->state->symbols[attr.name];
      NixValue wrapper{attr.value, value->state_ref};
      if (!callback(name.c_str(), &wrapper, user_data)) {
        break;
      }
    }
    return NIX_OK;
  });
}

NixError nix_value_coerce_to_string(NixEvalState* state, NixValue* value, bool copy_to_store,
                                    NixString* out) {
  if (!state || !value || !out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  return nix::ffi::catch_errors([&] {
    nix::NixStringContext context;
    auto s = state->state->coerceToString(nix::no_pos, *value->value, context, "FFI coerceToString",
                                          false, copy_to_store);
    nix::ffi::string_set(out, std::move(s).to_owned());
    return NIX_OK;
  });
}

// Value construction functions

NixError nix_value_new_int(int64_t n, NixValue** value_out) {
  if (!value_out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  // Note: These standalone values need special handling for GC
  // For now, we allocate without a state reference
  auto* v = new nix::value_t();
  v->mkInt(n);
  *value_out = new NixValue{v, nullptr};
  return NIX_OK;
}

NixError nix_value_new_float(double n, NixValue** value_out) {
  if (!value_out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  auto* v = new nix::value_t();
  v->mkFloat(n);
  *value_out = new NixValue{v, nullptr};
  return NIX_OK;
}

NixError nix_value_new_bool(bool b, NixValue** value_out) {
  if (!value_out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  auto* v = new nix::value_t();
  v->mkBool(b);
  *value_out = new NixValue{v, nullptr};
  return NIX_OK;
}

NixError nix_value_new_null(NixValue** value_out) {
  if (!value_out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  auto* v = new nix::value_t();
  v->mkNull();
  *value_out = new NixValue{v, nullptr};
  return NIX_OK;
}

// Value constructors that require eval state memory allocation

NixError nix_value_new_string(NixEvalState* state, const char* s, NixValue** value_out) {
  if (!state || !s || !value_out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  return nix::ffi::catch_errors([&] {
    auto* v = state->state->allocValue();
    v->mk_string(s, state->state->mem);
    *value_out = new NixValue{v, state};
    return NIX_OK;
  });
}

NixError nix_value_new_string_with_context(NixEvalState* state, const char* s,
                                           const char* const* context, size_t context_count,
                                           NixValue** value_out) {
  if (!state || !s || !value_out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  return nix::ffi::catch_errors([&] {
    nix::NixStringContext ctx;
    if (context && context_count > 0) {
      for (size_t i = 0; i < context_count; ++i) {
        if (context[i]) {
          // Parse context string - format depends on Nix context encoding
          // For now, treat as opaque store paths
          ctx.insert(nix::NixStringContextElem::parse(context[i]));
        }
      }
    }

    auto* v = state->state->allocValue();
    v->mk_string(s, ctx, state->state->mem);
    *value_out = new NixValue{v, state};
    return NIX_OK;
  });
}

NixError nix_value_new_path(NixEvalState* state, const char* path, NixValue** value_out) {
  if (!state || !path || !value_out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  return nix::ffi::catch_errors([&] {
    auto* v = state->state->allocValue();
    auto source_path = state->state->root_path(path);
    v->mkPath(source_path, state->state->mem);
    *value_out = new NixValue{v, state};
    return NIX_OK;
  });
}

NixError nix_value_new_list(NixEvalState* state, NixValue* const* elems, size_t count,
                            NixValue** value_out) {
  if (!state || !value_out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }
  if (count > 0 && !elems) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null elements array");
  }

  return nix::ffi::catch_errors([&] {
    auto builder = state->state->buildList(count);
    for (size_t i = 0; i < count; ++i) {
      if (!elems[i] || !elems[i]->value) {
        return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null list element");
      }
      builder[i] = elems[i]->value;
    }

    auto* v = state->state->allocValue();
    v->mkList(builder);
    *value_out = new NixValue{v, state};
    return NIX_OK;
  });
}

NixError nix_value_new_attrs(NixEvalState* state, const char* const* names, NixValue* const* values,
                             size_t count, NixValue** value_out) {
  if (!state || !value_out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }
  if (count > 0 && (!names || !values)) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null names or values array");
  }

  return nix::ffi::catch_errors([&] {
    auto builder = state->state->buildBindings(count);
    for (size_t i = 0; i < count; ++i) {
      if (!names[i]) {
        return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null attribute name");
      }
      if (!values[i] || !values[i]->value) {
        return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null attribute value");
      }
      auto sym = state->state->symbols.create(names[i]);
      builder.insert(sym, values[i]->value);
    }

    auto* v = state->state->allocValue();
    v->mkAttrs(builder.finish());
    *value_out = new NixValue{v, state};
    return NIX_OK;
  });
}

NixError nix_value_call(NixEvalState* state, NixValue* fn, NixValue* arg, NixValue** result_out) {
  if (!state || !fn || !arg || !result_out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  return nix::ffi::catch_errors([&] {
    auto* result = state->state->allocValue();
    state->state->callFunction(*fn->value, *arg->value, *result, nix::no_pos);
    *result_out = new NixValue{result, state};
    return NIX_OK;
  });
}

NixError nix_value_call_many(NixEvalState* state, NixValue* fn, NixValue* const* args,
                             size_t args_count, NixValue** result_out) {
  if (!state || !fn || !result_out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }
  if (args_count > 0 && !args) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null arguments array");
  }

  return nix::ffi::catch_errors([&] {
    nix::value_t* current = fn->value;

    for (size_t i = 0; i < args_count; ++i) {
      if (!args[i] || !args[i]->value) {
        return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument in array");
      }
      auto* next = state->state->allocValue();
      state->state->callFunction(*current, *args[i]->value, *next, nix::no_pos);
      current = next;
    }

    *result_out = new NixValue{current, state};
    return NIX_OK;
  });
}

NixError nix_value_get_string_context(const NixValue* value, NixString** contexts_out,
                                      size_t* count_out) {
  if (!value || !contexts_out || !count_out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  if (value->value->type() != nix::ValueType::nString) {
    return nix::ffi::set_error(NIX_ERR_TYPE, "expected string");
  }

  return nix::ffi::catch_errors([&] {
    auto* ctx = value->value->context();
    if (!ctx || ctx->size() == 0) {
      *contexts_out = nullptr;
      *count_out = 0;
      return NIX_OK;
    }

    auto* strings = new NixString[ctx->size()];
    size_t i = 0;
    for (const auto& elem : *ctx) {
      // elem is a const StringData*, containing the context element string
      nix::ffi::string_set(&strings[i++], std::string(elem->c_str()));
    }
    *contexts_out = strings;
    *count_out = ctx->size();
    return NIX_OK;
  });
}

NixError nix_value_coerce_to_path(NixEvalState* state, NixStore* store, NixValue* value,
                                  NixStorePath** path_out) {
  if (!state || !store || !value || !path_out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  return nix::ffi::catch_errors([&] {
    nix::NixStringContext context;
    auto path = state->state->coerceToStorePath(nix::no_pos, *value->value, context, "FFI coerce");
    *path_out = new NixStorePath{path};
    return NIX_OK;
  });
}

NixError nix_value_coerce_to_derivation(NixEvalState* state, NixStore* store, NixValue* value,
                                        NixDerivation** drv_out) {
  if (!state || !store || !value || !drv_out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  return nix::ffi::catch_errors([&] {
    // First, check if this is a derivation attrset
    if (!state->state->is_derivation(*value->value)) {
      return nix::ffi::set_error(NIX_ERR_TYPE, "value is not a derivation");
    }

    // Get the drvPath attribute to find the derivation file
    state->state->forceValue(*value->value, nix::no_pos);
    auto* attrs = value->value->attrs();
    auto drvPathSym = state->state->symbols.create("drvPath");
    auto* drvPathAttr = attrs->get(drvPathSym);

    if (!drvPathAttr) {
      return nix::ffi::set_error(NIX_ERR_EVAL, "derivation has no drvPath attribute");
    }

    state->state->forceValue(*drvPathAttr->value, nix::no_pos);
    nix::NixStringContext context;
    auto drvPath =
        state->state->coerceToStorePath(nix::no_pos, *drvPathAttr->value, context, "FFI drvPath");

    // Read the derivation
    auto drv = store->store->read_derivation(drvPath);
    *drv_out = new NixDerivation{std::move(drv), std::string(drvPath.name())};
    return NIX_OK;
  });
}

// Symbol operations
NixError nix_symbol_new(NixEvalState* state, const char* name, NixSymbol** sym_out) {
  if (!state || !name || !sym_out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  return nix::ffi::catch_errors([&] {
    auto sym = state->state->symbols.create(name);
    *sym_out = new NixSymbol{sym};
    return NIX_OK;
  });
}

NixError nix_symbol_str(const NixSymbol* sym, NixString* out) {
  if (!sym || !out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  // Note: Symbol strings are stored in the symbol table, which requires
  // access to the state. For standalone symbols, we store the symbol_t
  // which doesn't have direct string access without the table.
  // This is a design limitation - consider storing state ref in NixSymbol.
  return nix::ffi::set_error(NIX_ERR_INTERNAL,
                             "nix_symbol_str requires eval state - use nix_value_get_attr_names");
}

void nix_symbol_free(NixSymbol* sym) {
  delete sym;
}

// Position operations
NixError nix_expr_pos(const NixExpr* expr, NixPos** pos_out) {
  if (!expr || !pos_out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  return nix::ffi::catch_errors([&] {
    auto pos_idx = expr->expr->getPos();
    if (pos_idx == nix::no_pos) {
      *pos_out = nullptr;
      return NIX_OK;
    }
    auto pos = expr->state_ref->state->positions[pos_idx];
    *pos_out = new NixPos{pos};
    return NIX_OK;
  });
}

void nix_pos_free(NixPos* pos) {
  delete pos;
}

NixError nix_pos_info(const NixPos* pos, NixString* file_out, uint32_t* line_out,
                      uint32_t* column_out) {
  if (!pos) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  return nix::ffi::catch_errors([&] {
    if (file_out) {
      // Get file path from position
      auto& origin = pos->pos.origin;
      if (auto* path = std::get_if<nix::source_path_t>(&origin)) {
        nix::ffi::string_set(file_out, path->to_string());
      } else if (auto* str = std::get_if<nix::pos_t::String>(&origin)) {
        nix::ffi::string_set(file_out, *str->source);
      } else if (auto* stdin = std::get_if<nix::pos_t::Stdin>(&origin)) {
        nix::ffi::string_set_static(file_out, "<stdin>");
      } else {
        nix::ffi::string_set_static(file_out, "<unknown>");
      }
    }
    if (line_out) {
      *line_out = pos->pos.line;
    }
    if (column_out) {
      *column_out = pos->pos.column;
    }
    return NIX_OK;
  });
}

// Derivation operations
NixError nix_derivation_read(NixStore* store, const NixStorePath* drv_path,
                             NixDerivation** drv_out) {
  if (!store || !drv_path || !drv_out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  return nix::ffi::catch_errors([&] {
    auto drv = store->store->read_derivation(drv_path->path);
    *drv_out = new NixDerivation{std::move(drv), std::string(drv_path->path.name())};
    return NIX_OK;
  });
}

NixError nix_derivation_parse(NixStore* store, const char* drv_content, const char* name,
                              NixDerivation** drv_out) {
  if (!store || !drv_content || !name || !drv_out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  return nix::ffi::catch_errors([&] {
    auto drv = nix::parse_derivation(*store->store, std::string(drv_content), name);
    *drv_out = new NixDerivation{std::move(drv), std::string(name)};
    return NIX_OK;
  });
}

void nix_derivation_free(NixDerivation* drv) {
  delete drv;
}

NixError nix_derivation_name(const NixDerivation* drv, NixString* out) {
  if (!drv || !out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  nix::ffi::string_set(out, drv->name);
  return NIX_OK;
}

NixError nix_derivation_builder(const NixDerivation* drv, NixString* out) {
  if (!drv || !out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  nix::ffi::string_set(out, drv->drv.builder);
  return NIX_OK;
}

NixError nix_derivation_system(const NixDerivation* drv, NixString* out) {
  if (!drv || !out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  nix::ffi::string_set(out, drv->drv.platform);
  return NIX_OK;
}

NixError nix_derivation_args(const NixDerivation* drv, NixString** args_out, size_t* count_out) {
  if (!drv || !args_out || !count_out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  return nix::ffi::catch_errors([&] {
    const auto& args = drv->drv.args;
    *count_out = args.size();

    if (args.empty()) {
      *args_out = nullptr;
      return NIX_OK;
    }

    auto* strings = new NixString[args.size()];
    size_t i = 0;
    for (const auto& arg : args) {
      nix::ffi::string_set(&strings[i++], arg);
    }
    *args_out = strings;
    return NIX_OK;
  });
}

NixError nix_derivation_env(const NixDerivation* drv, NixString** names_out, NixString** values_out,
                            size_t* count_out) {
  if (!drv || !names_out || !values_out || !count_out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  return nix::ffi::catch_errors([&] {
    const auto& env = drv->drv.env;
    *count_out = env.size();

    if (env.empty()) {
      *names_out = nullptr;
      *values_out = nullptr;
      return NIX_OK;
    }

    auto* names = new NixString[env.size()];
    auto* values = new NixString[env.size()];
    size_t i = 0;
    for (const auto& [key, val] : env) {
      nix::ffi::string_set(&names[i], key);
      nix::ffi::string_set(&values[i], val);
      ++i;
    }
    *names_out = names;
    *values_out = values;
    return NIX_OK;
  });
}

NixError nix_derivation_outputs(const NixDerivation* drv, NixString** names_out,
                                size_t* count_out) {
  if (!drv || !names_out || !count_out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  return nix::ffi::catch_errors([&] {
    const auto& outputs = drv->drv.outputs;
    *count_out = outputs.size();

    if (outputs.empty()) {
      *names_out = nullptr;
      return NIX_OK;
    }

    auto* names = new NixString[outputs.size()];
    size_t i = 0;
    for (const auto& [name, _] : outputs) {
      nix::ffi::string_set(&names[i++], name);
    }
    *names_out = names;
    return NIX_OK;
  });
}

NixError nix_derivation_output_path(NixStore* store, const NixDerivation* drv,
                                    const char* output_name, NixStorePath** path_out) {
  if (!store || !drv || !output_name || !path_out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  return nix::ffi::catch_errors([&] {
    const auto& outputs = drv->drv.outputs;
    auto it = outputs.find(output_name);
    if (it == outputs.end()) {
      return nix::ffi::set_error(NIX_ERR_NOT_FOUND, "output not found");
    }

    // Get the output path - this depends on the output type
    auto paths = drv->drv.outputsAndOptPaths(*store->store);
    auto path_it = paths.find(output_name);
    if (path_it == paths.end() || !path_it->second.second) {
      // Floating CA output - path not yet known
      *path_out = nullptr;
      return NIX_OK;
    }

    *path_out = new NixStorePath{*path_it->second.second};
    return NIX_OK;
  });
}

NixError nix_derivation_build(NixStore* store, const NixDerivation* drv,
                              NixStorePath*** outputs_out, size_t* outputs_count_out) {
  if (!store || !drv || !outputs_out || !outputs_count_out) {
    return nix::ffi::set_error(NIX_ERR_NULL_POINTER, "null argument");
  }

  return nix::ffi::catch_errors([&] {
    // Build the derivation
    // First, write the derivation to the store to get its path
    auto drv_path = nix::write_derivation(*store->store, drv->drv, nix::NoRepair, false);

    // Build using the store's build mechanism
    auto result = store->store->buildDerivation(drv_path, drv->drv);

    if (auto* failure = result.tryGetFailure()) {
      std::string msg =
          failure->errorMsg.empty()
              ? std::string(nix::build_result_t::Failure::status_to_string(failure->status))
              : failure->errorMsg;
      return nix::ffi::set_error(NIX_ERR_BUILD, "build failed: " + msg);
    }

    // Collect output paths
    auto output_paths = drv->drv.outputsAndOptPaths(*store->store);
    std::vector<nix::store_path_t> built_paths;

    for (const auto& [name, output] : output_paths) {
      if (output.second) {
        built_paths.push_back(*output.second);
      }
    }

    *outputs_count_out = built_paths.size();
    if (built_paths.empty()) {
      *outputs_out = nullptr;
      return NIX_OK;
    }

    auto** paths = new NixStorePath*[built_paths.size()];
    for (size_t i = 0; i < built_paths.size(); ++i) {
      paths[i] = new NixStorePath{built_paths[i]};
    }
    *outputs_out = paths;
    return NIX_OK;
  });
}

} // extern "C"
