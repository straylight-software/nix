#include "nix/expr/json-to-value.h"

#include <limits>
#include <variant>

#include <nlohmann/json.hpp>

#include "nix/expr/eval.h"
#include "nix/expr/value.h"

using json = nlohmann::json;

namespace nix {

// for more information, refer to
// https://github.com/nlohmann/json/blob/master/include/nlohmann/detail/input/json_sax.hpp
struct json_sax_t : nlohmann::json_sax<json> {
  struct json_state_t {
    std::unique_ptr<json_state_t> parent;
    RootValue v;

    virtual std::unique_ptr<json_state_t> resolve(eval_state_t&) {
      throw std::logic_error("tried to close toplevel json parser state");
    }

    explicit json_state_t(std::unique_ptr<json_state_t>&& p) : parent(std::move(p)) {}

    explicit json_state_t(value_t* v) : v(alloc_root_value(v)) {}

    json_state_t(json_state_t& p) = delete;

    value_t& value(eval_state_t& state) {
      if (!v)
        v = alloc_root_value(state.allocValue());
      return **v;
    }

    virtual ~json_state_t() {}

    virtual void add() {}
  };

  struct json_object_state_t : public json_state_t {
    using json_state_t::json_state_t;
    ValueMap attrs;

    std::unique_ptr<json_state_t> resolve(eval_state_t& state) override {
      auto attrs2 = state.buildBindings(attrs.size());
      for (auto& i : attrs)
        attrs2.insert(i.first, i.second);
      parent->value(state).mkAttrs(attrs2);
      return std::move(parent);
    }

    void add() override { v = nullptr; }

    void key(string_t& name, eval_state_t& state) {
      force_no_null_byte(name);
      attrs.insert_or_assign(state.symbols.create(name), &value(state));
    }
  };

  struct json_list_state_t : public json_state_t {
    ValueVector values;

    std::unique_ptr<json_state_t> resolve(eval_state_t& state) override {
      auto list = state.buildList(values.size());
      for (const auto& [n, v2] : enumerate(list))
        v2 = values[n];
      parent->value(state).mkList(list);
      return std::move(parent);
    }

    void add() override {
      values.push_back(*v);
      v = nullptr;
    }

    json_list_state_t(std::unique_ptr<json_state_t>&& p, std::size_t reserve)
        : json_state_t(std::move(p)) {
      values.reserve(reserve);
    }
  };

  eval_state_t& state;
  std::unique_ptr<json_state_t> rs;

  json_sax_t(eval_state_t& state, value_t& v) : state(state), rs(new json_state_t(&v)) {};

  bool null() override {
    rs->value(state).mkNull();
    rs->add();
    return true;
  }

  bool boolean(bool val) override {
    rs->value(state).mkBool(val);
    rs->add();
    return true;
  }

  bool number_integer(number_integer_t val) override {
    rs->value(state).mkInt(val);
    rs->add();
    return true;
  }

  bool number_unsigned(number_unsigned_t val_) override {
    if (val_ > std::numeric_limits<NixInt::Inner>::max()) {
      throw Error("unsigned json number %1% outside of Nix integer range", val_);
    }
    NixInt::Inner val = val_;
    rs->value(state).mkInt(val);
    rs->add();
    return true;
  }

  bool number_float(number_float_t val, const string_t& s) override {
    rs->value(state).mkFloat(val);
    rs->add();
    return true;
  }

  bool string(string_t& val) override {
    force_no_null_byte(val);
    rs->value(state).mk_string(val, state.mem);
    rs->add();
    return true;
  }

#if NLOHMANN_JSON_VERSION_MAJOR >= 3 && NLOHMANN_JSON_VERSION_MINOR >= 8
  bool binary(binary_t&) override {
    // This function ought to be unreachable
    assert(false);
    return true;
  }
#endif

  bool start_object(std::size_t len) override {
    rs = std::make_unique<json_object_state_t>(std::move(rs));
    return true;
  }

  bool key(string_t& name) override {
    dynamic_cast<json_object_state_t*>(rs.get())->key(name, state);
    return true;
  }

  bool end_object() override {
    rs = rs->resolve(state);
    rs->add();
    return true;
  }

  bool end_array() override { return end_object(); }

  bool start_array(size_t len) override {
    rs = std::make_unique<json_list_state_t>(std::move(rs),
                                             len != std::numeric_limits<size_t>::max() ? len : 128);
    return true;
  }

  bool parse_error(std::size_t, const std::string&,
                   const nlohmann::detail::exception& ex) override {
    throw JSONParseError("%s", ex.what());
  }
};

void parse_json(eval_state_t& state, const std::string_view& s_, value_t& v) {
  json_sax_t parser(state, v);
  bool res = json::sax_parse(s_, &parser);
  if (!res)
    throw JSONParseError("Invalid JSON value_t");
}

} // namespace nix
