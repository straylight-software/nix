namespace nix {

struct external_derivation_builder_t : derivation_builder_impl_t
{
    ExternalBuilder external_builder;

    external_derivation_builder_t(
        LocalStore & store,
        std::unique_ptr<DerivationBuilderCallbacks> misc_methods,
        DerivationBuilderParams params,
        ExternalBuilder external_builder)
        : derivation_builder_impl_t(store, std::move(misc_methods), std::move(params))
        , external_builder(std::move(external_builder))
    {
        experimental_feature_settings.require(xp_t::external_builders);
    }

    Path tmp_dir_in_sandbox() override
    {
        /* In a sandbox, for determinism, always use the same temporary
           directory. */
        return "/build";
    }

    void set_build_tmp_dir() override
    {
        tmp_dir = topTmpDir + "/build";
        create_dir(tmp_dir, 0700);
    }

    void start_child() override
    {
        if (drv_options.getRequiredSystemFeatures(drv).count("recursive-nix"))
            throw Error("'recursive-nix' is not supported yet by external derivation builders");

        auto json = nlohmann::json::object();

        json.emplace("version", 1);
        json.emplace("builder", drv.builder);
        {
            auto l = nlohmann::json::array();
            for (auto & i : drv.args)
                l.push_back(rewrite_strings(i, input_rewrites));
            json.emplace("args", std::move(l));
        }
        {
            auto j = nlohmann::json::object();
            for (auto & [name, value] : env)
                j.emplace(name, rewrite_strings(value, input_rewrites));
            json.emplace("env", std::move(j));
        }
        json.emplace("topTmpDir", topTmpDir);
        json.emplace("tmpDir", tmp_dir);
        json.emplace("tmpDirInSandbox", tmp_dir_in_sandbox());
        json.emplace("storeDir", store.store_dir);
        json.emplace("realStoreDir", store.config->real_store_dir.get());
        json.emplace("system", drv.platform);
        {
            auto l = nlohmann::json::array();
            for (auto & i : inputPaths)
                l.push_back(store.printStorePath(i));
            json.emplace("inputPaths", std::move(l));
        }
        {
            auto l = nlohmann::json::object();
            for (auto & i : scratchOutputs)
                l.emplace(i.first, store.printStorePath(i.second));
            json.emplace("outputs", std::move(l));
        }

        // TODO(cole-h): writing this to stdin is too much effort right now, if we want to revisit
        // that, see this comment by Eelco about how to make it not suck:
        // https://github.com/DeterminateSystems/nix-src/pull/141#discussion_r2205493257
        auto json_file = std::filesystem::path{topTmpDir} / "build.json";
        write_file(json_file, json.dump());

        pid = start_process([&]() {
            open_slave();
            try {
                common_child_init();

                strings_t args = {external_builder.program};

                if (!external_builder.args.empty()) {
                    args.insert(args.end(), external_builder.args.begin(), external_builder.args.end());
                }

                args.insert(args.end(), json_file);

                if (chdir(tmp_dir.c_str()) == -1)
                    throw sys_error_t("changing into '%1%'", tmp_dir);

                chown_to_builder(topTmpDir);

                set_user();

                debug("executing external builder: %s", concat_strings_sep(" ", args));
                execv(external_builder.program.c_str(), strings_to_char_ptrs(args).data());

                throw sys_error_t("executing '%s'", external_builder.program);
            } catch (...) {
                handle_child_exception(true);
                _exit(1);
            }
        });
    }
};

std::unique_ptr<DerivationBuilder> make_external_derivation_builder(
    LocalStore & store,
    std::unique_ptr<DerivationBuilderCallbacks> misc_methods,
    DerivationBuilderParams params,
    const ExternalBuilder & handler)
{
    return std::make_unique<external_derivation_builder_t>(store, std::move(misc_methods), std::move(params), handler);
}

} // namespace nix
