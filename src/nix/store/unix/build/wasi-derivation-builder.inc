// wasi-derivation-builder.inc
// TODO[b7r6]: WASI builder disabled - needs wasmtime dependency

#ifndef NIX_WITH_WASI
// Stub class when WASI is disabled
struct wasi_derivation_builder_t : nix::derivation_builder_impl_t
{
    wasi_derivation_builder_t(
        nix::LocalStore & store,
        std::unique_ptr<nix::DerivationBuilderCallbacks> misc_methods,
        nix::DerivationBuilderParams params)
        : nix::derivation_builder_impl_t(store, std::move(misc_methods), std::move(params))
    {
        throw nix::Error("WASI builds are not supported in this build of Nix (compiled without wasmtime)");
    }
};
#endif
