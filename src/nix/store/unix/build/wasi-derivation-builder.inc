// wasi-derivation-builder.inc
// TODO[b7r6]: WASI builder disabled - needs wasmtime dependency

#ifndef NIX_WITH_WASI
// Stub class when WASI is disabled
struct WasiDerivationBuilder : nix::DerivationBuilderImpl
{
    WasiDerivationBuilder(
        nix::LocalStore & store,
        std::unique_ptr<nix::DerivationBuilderCallbacks> miscMethods,
        nix::DerivationBuilderParams params)
        : nix::DerivationBuilderImpl(store, std::move(miscMethods), std::move(params))
    {
        throw nix::Error("WASI builds are not supported in this build of Nix (compiled without wasmtime)");
    }
};
#endif
