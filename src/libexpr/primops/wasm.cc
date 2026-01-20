#include "nix/expr/primops.hh"
#include "nix/expr/eval-inline.hh"

#include <wasmtime.hh>
#include <wasi.h>

using namespace wasmtime;

namespace nix {

// FIXME
SourcePath realisePath(
    EvalState & state,
    const PosIdx pos,
    Value & v,
    std::optional<SymlinkResolution> resolveSymlinks = SymlinkResolution::Full);

using ValueId = uint32_t;

template<typename T, typename E = Error>
T unwrap(Result<T, E> && res)
{
    if (res)
        return res.ok();
    throw Error(res.err().message());
}

static Engine & getEngine()
{
    static Engine engine = []() {
        wasmtime::Config config;
        config.pooling_allocation_strategy(PoolAllocationConfig());
        config.memory_init_cow(true);
        return Engine(std::move(config));
    }();
    return engine;
}

static std::span<uint8_t> string2span(std::string_view s)
{
    return std::span<uint8_t>((uint8_t *) s.data(), s.size());
}

static std::string_view span2string(std::span<uint8_t> s)
{
    return std::string_view((char *) s.data(), s.size());
}

template<typename T>
static std::span<T> subspan(std::span<uint8_t> s, size_t len)
{
    assert(s.size() >= len * sizeof(T));
    return std::span((T *) s.data(), len);
}

struct NixWasmInstance;

template<typename R, typename... Args>
static void regFun(Linker & linker, std::string_view name, R (NixWasmInstance::*f)(Args...))
{
    unwrap(linker.func_wrap("env", name, [f](Caller caller, Args... args) -> Result<R, Trap> {
        try {
            auto instance = std::any_cast<NixWasmInstance *>(caller.context().get_data());
            return (*instance.*f)(args...);
        } catch (Error & e) {
            return Trap(e.what());
        }
    }));
}

// Pre-compiled module with linker (no WASI yet - that's per-instance)
struct NixWasmModule
{
    Engine & engine;
    SourcePath wasmPath;
    Module module;

    NixWasmModule(SourcePath _wasmPath)
        : engine(getEngine())
        , wasmPath(_wasmPath)
        , module(unwrap(Module::compile(engine, string2span(wasmPath.readFile()))))
    {
    }
};

struct NixWasmInstance
{
    EvalState & state;
    ref<NixWasmModule> mod;
    wasmtime::Store wasmStore;
    wasmtime::Store::Context wasmCtx;
    std::optional<Instance> instance;
    std::optional<Memory> memory_;

    ValueVector values;
    std::exception_ptr ex;

    std::optional<std::string> functionName;

    NixWasmInstance(EvalState & _state, ref<NixWasmModule> _mod)
        : state(_state)
        , mod(_mod)
        , wasmStore(mod->engine)
        , wasmCtx(wasmStore)
    {
        // Set instance pointer BEFORE instantiation so FFI callbacks can find us
        wasmCtx.set_data(this);

        // Create linker for this instance
        Linker linker(mod->engine);

        // Set up WASI for GHC runtime support
        wasi_config_t * wasi_config = wasi_config_new();
        wasi_config_inherit_stdout(wasi_config);
        wasi_config_inherit_stderr(wasi_config);

        auto * error = wasmtime_context_set_wasi(wasmCtx.capi(), wasi_config);
        if (error != nullptr) {
            auto msg = wasmtime::Error(error);
            throw nix::Error("failed to set WASI config: %s", msg.message());
        }

        // Link WASI functions
        error = wasmtime_linker_define_wasi(linker.capi());
        if (error != nullptr) {
            auto msg = wasmtime::Error(error);
            throw nix::Error("failed to define WASI: %s", msg.message());
        }

        // Register Nix FFI functions
        regFun(linker, "panic", &NixWasmInstance::panic);
        regFun(linker, "warn", &NixWasmInstance::warn);
        regFun(linker, "get_type", &NixWasmInstance::get_type);
        regFun(linker, "make_int", &NixWasmInstance::make_int);
        regFun(linker, "get_int", &NixWasmInstance::get_int);
        regFun(linker, "make_float", &NixWasmInstance::make_float);
        regFun(linker, "get_float", &NixWasmInstance::get_float);
        regFun(linker, "make_string", &NixWasmInstance::make_string);
        regFun(linker, "copy_string", &NixWasmInstance::copy_string);
        regFun(linker, "get_string_len", &NixWasmInstance::get_string_len);
        regFun(linker, "make_bool", &NixWasmInstance::make_bool);
        regFun(linker, "get_bool", &NixWasmInstance::get_bool);
        regFun(linker, "make_null", &NixWasmInstance::make_null);
        regFun(linker, "make_path", &NixWasmInstance::make_path);
        regFun(linker, "copy_path", &NixWasmInstance::copy_path);
        regFun(linker, "make_list", &NixWasmInstance::make_list);
        regFun(linker, "copy_list", &NixWasmInstance::copy_list);
        regFun(linker, "get_list_len", &NixWasmInstance::get_list_len);
        regFun(linker, "get_list_elem", &NixWasmInstance::get_list_elem);
        regFun(linker, "make_attrset", &NixWasmInstance::make_attrset);
        regFun(linker, "copy_attrset", &NixWasmInstance::copy_attrset);
        regFun(linker, "copy_attrname", &NixWasmInstance::copy_attrname);
        regFun(linker, "get_attrs_len", &NixWasmInstance::get_attrs_len);
        regFun(linker, "has_attr", &NixWasmInstance::has_attr);
        regFun(linker, "get_attr", &NixWasmInstance::get_attr);
        regFun(linker, "call_function", &NixWasmInstance::call_function);

        // Instantiate the module (this may call _initialize which needs FFI)
        instance = unwrap(linker.instantiate(wasmCtx, mod->module));
        memory_ = std::get<Memory>(*instance->get(wasmCtx, "memory"));
    }

    ValueId addValue(Value * v)
    {
        auto id = values.size();
        values.emplace_back(v);
        return id;
    }

    std::pair<ValueId, Value &> allocValue()
    {
        auto v = state.allocValue();
        auto id = addValue(v);
        return {id, *v};
    }

    Func getFunction(std::string_view name)
    {
        auto ext = instance->get(wasmCtx, name);
        if (!ext)
            throw Error("WASM module '%s' does not export function '%s'", mod->wasmPath, name);
        auto fun = std::get_if<Func>(&*ext);
        if (!fun)
            throw Error("export '%s' of WASM module '%s' is not a function", name, mod->wasmPath);
        return *fun;
    }

    std::vector<Val> runFunction(std::string_view name, const std::vector<Val> & args)
    {
        functionName = name;
        return unwrap(getFunction(name).call(wasmCtx, args));
    }

    auto memory()
    {
        return memory_->data(wasmCtx);
    }

    std::monostate panic(uint32_t ptr, uint32_t len)
    {
        throw Error("WASM panic: %s", Uncolored(span2string(memory().subspan(ptr, len))));
    }

    std::monostate warn(uint32_t ptr, uint32_t len)
    {
        nix::warn(
            "'%s' function '%s': %s",
            mod->wasmPath,
            functionName.value_or("<unknown>"),
            span2string(memory().subspan(ptr, len)));
        return {};
    }

    uint32_t get_type(ValueId valueId)
    {
        auto & value = *values.at(valueId);
        state.forceValue(value, noPos);
        auto t = value.type();
        return t == nInt        ? 1
               : t == nFloat    ? 2
               : t == nBool     ? 3
               : t == nString   ? 4
               : t == nPath     ? 5
               : t == nNull     ? 6
               : t == nAttrs    ? 7
               : t == nList     ? 8
               : t == nFunction ? 9
                                : []() -> int { throw Error("unsupported type"); }();
    }

    ValueId make_int(int64_t n)
    {
        auto [valueId, value] = allocValue();
        value.mkInt(n);
        return valueId;
    }

    int64_t get_int(ValueId valueId)
    {
        return state.forceInt(*values.at(valueId), noPos, "while evaluating a value from WASM").value;
    }

    ValueId make_float(double x)
    {
        auto [valueId, value] = allocValue();
        value.mkFloat(x);
        return valueId;
    }

    double get_float(ValueId valueId)
    {
        return state.forceFloat(*values.at(valueId), noPos, "while evaluating a value from WASM");
    }

    ValueId make_string(uint32_t ptr, uint32_t len)
    {
        auto [valueId, value] = allocValue();
        value.mkString(span2string(memory().subspan(ptr, len)), state.mem);
        return valueId;
    }

    uint32_t copy_string(ValueId valueId, uint32_t ptr, uint32_t maxLen)
    {
        auto s = state.forceString(*values.at(valueId), noPos, "while evaluating a value from WASM");
        if (s.size() <= maxLen) {
            auto buf = memory().subspan(ptr, maxLen);
            memcpy(buf.data(), s.data(), s.size());
        }
        return s.size();
    }

    ValueId make_bool(int32_t b)
    {
        return addValue(state.getBool(b));
    }

    int32_t get_bool(ValueId valueId)
    {
        return state.forceBool(*values.at(valueId), noPos, "while evaluating a value from WASM");
    }

    ValueId make_null()
    {
        return addValue(&Value::vNull);
    }

    ValueId make_list(uint32_t ptr, uint32_t len)
    {
        auto vs = subspan<ValueId>(memory().subspan(ptr), len);

        auto [valueId, value] = allocValue();

        auto list = state.buildList(len);
        for (const auto & [n, v] : enumerate(list))
            v = values.at(vs[n]); // FIXME: endianness
        value.mkList(list);

        return valueId;
    }

    uint32_t copy_list(ValueId valueId, uint32_t ptr, uint32_t maxLen)
    {
        auto & value = *values.at(valueId);
        state.forceList(value, noPos, "while getting a list from WASM");

        if (value.listSize() <= maxLen) {
            auto out = subspan<ValueId>(memory().subspan(ptr), value.listSize());

            for (const auto & [n, elem] : enumerate(value.listView()))
                out[n] = addValue(elem);
        }

        return value.listSize();
    }

    ValueId make_attrset(uint32_t ptr, uint32_t len)
    {
        auto mem = memory();

        struct Attr
        {
            // FIXME: endianness
            uint32_t attrNamePtr;
            uint32_t attrNameLen;
            ValueId value;
        };

        auto attrs = subspan<Attr>(mem.subspan(ptr), len);

        auto [valueId, value] = allocValue();
        auto builder = state.buildBindings(len);
        for (auto & attr : attrs)
            builder.insert(
                state.symbols.create(span2string(mem.subspan(attr.attrNamePtr, attr.attrNameLen))),
                values.at(attr.value));
        value.mkAttrs(builder);

        return valueId;
    }

    uint32_t copy_attrset(ValueId valueId, uint32_t ptr, uint32_t maxLen)
    {
        auto & value = *values.at(valueId);
        state.forceAttrs(value, noPos, "while copying an attrset into WASM");

        if (value.attrs()->size() <= maxLen) {
            // FIXME: endianness.
            struct Attr
            {
                ValueId value;
                uint32_t nameLen;
            };

            auto buf = subspan<Attr>(memory().subspan(ptr), maxLen);

            // FIXME: for determinism, we should return attributes in lexicographically sorted order.
            for (const auto & [n, attr] : enumerate(*value.attrs())) {
                buf[n].value = addValue(attr.value);
                buf[n].nameLen = state.symbols[attr.name].size();
            }
        }

        return value.attrs()->size();
    }

    std::monostate copy_attrname(ValueId valueId, uint32_t attrIdx, uint32_t ptr, uint32_t len)
    {
        auto & value = *values.at(valueId);
        state.forceAttrs(value, noPos, "while copying an attr name into WASM");

        auto & attrs = *value.attrs();

        assert((size_t) attrIdx < attrs.size());

        std::string_view name = state.symbols[attrs[attrIdx].name];

        assert((size_t) len == name.size());

        memcpy(memory().subspan(ptr, len).data(), name.data(), name.size());

        return {};
    }

    ValueId call_function(ValueId funId, uint32_t ptr, uint32_t len)
    {
        auto & fun = *values.at(funId);
        state.forceFunction(fun, noPos, "while calling a function from WASM");

        ValueVector args;
        for (auto argId : subspan<ValueId>(memory().subspan(ptr), len))
            args.push_back(values.at(argId));

        auto [valueId, value] = allocValue();

        state.callFunction(fun, args, value, noPos);

        return valueId;
    }

    // Get the length of a list without copying
    uint32_t get_list_len(ValueId valueId)
    {
        auto & value = *values.at(valueId);
        state.forceList(value, noPos, "while getting list length from WASM");
        return value.listSize();
    }

    // Get a single list element by index
    ValueId get_list_elem(ValueId valueId, uint32_t idx)
    {
        auto & value = *values.at(valueId);
        state.forceList(value, noPos, "while getting list element from WASM");
        if (idx >= value.listSize())
            throw Error("list index %d out of bounds (size %d)", idx, value.listSize());
        auto view = value.listView();
        return addValue(view[idx]);
    }

    // Get the number of attributes in an attrset without copying
    uint32_t get_attrs_len(ValueId valueId)
    {
        auto & value = *values.at(valueId);
        state.forceAttrs(value, noPos, "while getting attrset size from WASM");
        return value.attrs()->size();
    }

    // Check if an attribute exists in an attrset
    int32_t has_attr(ValueId valueId, uint32_t namePtr, uint32_t nameLen)
    {
        auto & value = *values.at(valueId);
        state.forceAttrs(value, noPos, "while checking attr in WASM");
        auto name = state.symbols.create(span2string(memory().subspan(namePtr, nameLen)));
        return value.attrs()->get(name) != nullptr ? 1 : 0;
    }

    // Get an attribute value by name, returns 0xFFFFFFFF if not found
    uint32_t get_attr(ValueId valueId, uint32_t namePtr, uint32_t nameLen)
    {
        auto & value = *values.at(valueId);
        state.forceAttrs(value, noPos, "while getting attr from WASM");
        auto name = state.symbols.create(span2string(memory().subspan(namePtr, nameLen)));
        auto attr = value.attrs()->get(name);
        if (!attr)
            return 0xFFFFFFFF; // sentinel for "not found"
        return addValue(attr->value);
    }

    // Create a path value from a string
    ValueId make_path(uint32_t ptr, uint32_t len)
    {
        auto [valueId, value] = allocValue();
        auto pathStr = span2string(memory().subspan(ptr, len));
        value.mkPath(state.rootPath(CanonPath(pathStr)), state.mem);
        return valueId;
    }

    // Copy a path value to a buffer (returns length, copies up to maxLen bytes)
    uint32_t copy_path(ValueId valueId, uint32_t ptr, uint32_t maxLen)
    {
        auto & value = *values.at(valueId);
        NixStringContext context;
        auto path = state.coerceToPath(noPos, value, context, "while copying path from WASM");
        auto pathStr = path.path.abs();
        if (pathStr.size() <= maxLen) {
            auto buf = memory().subspan(ptr, maxLen);
            memcpy(buf.data(), pathStr.data(), pathStr.size());
        }
        return pathStr.size();
    }

    // Get string length without copying
    uint32_t get_string_len(ValueId valueId)
    {
        auto s = state.forceString(*values.at(valueId), noPos, "while getting string length from WASM");
        return s.size();
    }
};

void prim_wasm(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto wasmPath = realisePath(state, pos, *args[0]);
    std::string functionName =
        std::string(state.forceStringNoCtx(*args[1], pos, "while evaluating the second argument of `builtins.wasm`"));

    try {
        // Cache compiled modules (but not instances, since WASI state is per-instance)
        // FIXME: make thread-safe.
        // FIXME: make this a weak Boehm GC pointer so that it can be freed during GC.
        static std::unordered_map<SourcePath, ref<NixWasmModule>> modules;

        auto mod = modules.find(wasmPath);
        if (mod == modules.end())
            mod = modules.emplace(wasmPath, make_ref<NixWasmModule>(wasmPath)).first;

        debug("calling wasm module");

        NixWasmInstance instance{state, mod->second};

        // Initialize the WASM module (GHC RTS setup, etc.)
        debug("calling _initialize");
        auto initResult = instance.runFunction("_initialize", {});
        debug("_initialize returned with %d results", initResult.size());
        
        // Check if hs_init is exported and call it (GHC WASM RTS init)
        // hs_init(int *argc, char ***argv) - we pass NULL for both
        auto hsInitExt = instance.instance->get(instance.wasmCtx, "hs_init");
        if (hsInitExt) {
            auto hsInit = std::get_if<Func>(&*hsInitExt);
            if (hsInit) {
                debug("calling hs_init");
                unwrap(hsInit->call(instance.wasmCtx, {(int32_t) 0, (int32_t) 0}));
                debug("hs_init complete");
            }
        }
        
        debug("calling nix_wasm_init_v1");
        instance.runFunction("nix_wasm_init_v1", {});
        debug("initialization complete");

        v = *instance.values.at(instance.runFunction(functionName, {(int32_t) instance.addValue(args[2])}).at(0).i32());
    } catch (Error & e) {
        e.addTrace(state.positions[pos], "while executing the WASM function '%s' from '%s'", functionName, wasmPath);
        throw;
    }
}

static RegisterPrimOp primop_fromTOML(
    {.name = "wasm",
     .args = {"wasm", "entry", "arg"},
     .doc = R"(
       Call a WASM function with the specified argument.
      )",
     .fun = prim_wasm});

} // namespace nix
