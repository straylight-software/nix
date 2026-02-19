#ifdef __linux__

namespace nix {

struct chroot_derivation_builder_t : virtual derivation_builder_impl_t
{
    chroot_derivation_builder_t(
        LocalStore & store, std::unique_ptr<DerivationBuilderCallbacks> misc_methods, DerivationBuilderParams params)
        : derivation_builder_impl_t{store, std::move(misc_methods), std::move(params)}
    {
    }

    /**
     * The root of the chroot environment.
     */
    Path chroot_root_dir;

    /**
     * RAII object to delete the chroot directory.
     */
    std::shared_ptr<auto_delete_t> auto_del_chroot;

    PathsInChroot paths_in_chroot;

    bool needs_hash_rewrite() override
    {
        return false;
    }

    void set_build_tmp_dir() override
    {
        /* If sandboxing is enabled, put the actual TMPDIR underneath
           an inaccessible root-owned directory, to prevent outside
           access.

           On macOS, we don't use an actual chroot, so this isn't
           possible. Any mitigation along these lines would have to be
           done directly in the sandbox profile. */
        tmp_dir = topTmpDir + "/build";
        create_dir(tmp_dir, 0700);
    }

    Path tmp_dir_in_sandbox() override
    {
        /* In a sandbox, for determinism, always use the same temporary
           directory. */
        return settings.sandboxBuildDir;
    }

    virtual gid_t sandbox_gid()
    {
        return buildUser->getGID();
    }

    void prepare_sandbox() override
    {
        /* Create a temporary directory in which we set up the chroot
           environment using bind-mounts.  We put it in the Nix store
           so that the build outputs can be moved efficiently from the
           chroot to their final location. */
        auto chroot_parent_dir = store.toRealPath(drv_path) + ".chroot";
        delete_path(chroot_parent_dir);

        /* Clean up the chroot directory automatically. */
        auto_del_chroot = std::make_shared<auto_delete_t>(chroot_parent_dir);

        printMsg(lvl_chatty, "setting up chroot environment in '%1%'", chroot_parent_dir);

        if (mkdir(chroot_parent_dir.c_str(), 0700) == -1)
            throw sys_error_t("cannot create '%s'", chroot_root_dir);

        chroot_root_dir = chroot_parent_dir + "/root";

        if (mkdir(chroot_root_dir.c_str(), buildUser && buildUser->getUIDCount() != 1 ? 0755 : 0750) == -1)
            throw sys_error_t("cannot create '%1%'", chroot_root_dir);

        if (buildUser
            && chown(
                   chroot_root_dir.c_str(), buildUser->getUIDCount() != 1 ? buildUser->getUID() : 0, buildUser->getGID())
                   == -1)
            throw sys_error_t("cannot change ownership of '%1%'", chroot_root_dir);

        /* Create a writable /tmp in the chroot.  Many builders need
           this.  (Of course they should really respect $TMPDIR
           instead.) */
        Path chroot_tmp_dir = chroot_root_dir + "/tmp";
        create_dirs(chroot_tmp_dir);
        chmod_(chroot_tmp_dir, 01777);

        /* Create a /etc/passwd with entries for the build user and the
           nobody account.  The latter is kind of a hack to support
           Samba-in-QEMU. */
        create_dirs(chroot_root_dir + "/etc");
        if (drv_options.useUidRange(drv))
            chown_to_builder(chroot_root_dir + "/etc");

        if (drv_options.useUidRange(drv) && (!buildUser || buildUser->getUIDCount() < 65536))
            throw Error("feature 'uid-range' requires the setting '%s' to be enabled", settings.autoAllocateUids.name);

        /* Declare the build user's group so that programs get a consistent
           view of the system (e.g., "id -gn"). */
        write_file(
            chroot_root_dir + "/etc/group",
            fmt("root:x:0:\n"
                "nixbld:!:%1%:\n"
                "nogroup:x:65534:\n",
                sandbox_gid()));

        /* Create /etc/hosts with localhost entry. */
        if (derivation_type.isSandboxed())
            write_file(chroot_root_dir + "/etc/hosts", "127.0.0.1 localhost\n::1 localhost\n");

        /* Make the closure of the inputs available in the chroot,
           rather than the whole Nix store.  This prevents any access
           to undeclared dependencies.  Directories are bind-mounted,
           while other inputs are hard-linked (since only directories
           can be bind-mounted).  !!! As an extra security
           precaution, make the fake Nix store only writable by the
           build user. */
        Path chroot_store_dir = chroot_root_dir + store.store_dir;
        create_dirs(chroot_store_dir);
        chmod_(chroot_store_dir, 01775);

        if (buildUser && chown(chroot_store_dir.c_str(), 0, buildUser->getGID()) == -1)
            throw sys_error_t("cannot change ownership of '%1%'", chroot_store_dir);

        paths_in_chroot = get_paths_in_sandbox();

        for (auto & i : inputPaths) {
            auto p = store.printStorePath(i);
            paths_in_chroot.insert_or_assign(p, ChrootPath{.source = store.toRealPath(p)});
        }

        /* If we're repairing, checking or rebuilding part of a
           multiple-outputs derivation, it's possible that we're
           rebuilding a path that is in settings.sandbox-paths
           (typically the dependencies of /bin/sh).  Throw them
           out. */
        for (auto & i : drv.outputsAndOptPaths(store)) {
            /* If the name isn't known a priori (i.e. floating
               content-addressing derivation), the temporary location we use
               should be fresh.  Freshness means it is impossible that the path
               is already in the sandbox, so we don't need to worry about
               removing it.  */
            if (i.second.second)
                paths_in_chroot.erase(store.printStorePath(*i.second.second));
        }
    }

    strings_t get_pre_build_hook_args() override
    {
        assert(!chroot_root_dir.empty());
        return strings_t({store.printStorePath(drv_path), chroot_root_dir});
    }

    Path real_path_in_host(const Path & p) override
    {
        // FIXME: why the needsHashRewrite() conditional?
        return !needs_hash_rewrite() ? chroot_root_dir + p : store.toRealPath(p);
    }

    void cleanup_build(bool force) override
    {
        derivation_builder_impl_t::cleanup_build(force);

        /* Move paths out of the chroot for easier debugging of
           build failures. */
        if (!force && build_mode == bmNormal)
            for (auto & [_, status] : initialOutputs) {
                if (!status.known)
                    continue;
                if (build_mode != bmCheck && status.known->isValid())
                    continue;
                auto p = store.toRealPath(status.known->path);
                if (path_exists(chroot_root_dir + p))
                    std::filesystem::rename((chroot_root_dir + p), p);
            }

        auto_del_chroot.reset(); /* this runs the destructor */
    }

    std::pair<Path, Path> add_dependency_prep(const StorePath & path)
    {
        derivation_builder_impl_t::add_dependency_impl(path);

        debug("materialising '%s' in the sandbox", store.printStorePath(path));

        Path source = store.toRealPath(path);
        Path target = chroot_root_dir + store.printStorePath(path);

        if (path_exists(target)) {
            // There is a similar debug message in doBind, so only run it in this block to not have double messages.
            debug("bind-mounting %s -> %s", target, source);
            throw Error("store path '%s' already exists in the sandbox", store.printStorePath(path));
        }

        return {source, target};
    }
};

} // namespace nix

#endif
