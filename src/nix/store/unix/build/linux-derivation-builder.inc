#ifdef __linux__

#  include "nix/store/personality.h"
#  include "nix/util/cgroup.h"
#  include "nix/util/linux-namespaces.h"
#  include "nix/util/logging.h"
#  include "nix/store/linux/fchmodat2-compat.h"

#  include <sys/ioctl.h>
#  include <net/if.h>
#  include <netinet/ip.h>
#  include <sys/mman.h>
#  include <sched.h>
#  include <sys/param.h>
#  include <sys/mount.h>
#  include <sys/syscall.h>

#  if HAVE_SECCOMP
#    include <seccomp.h>
#  endif

#  define pivot_root(new_root, put_old) (syscall(SYS_pivot_root, new_root, put_old))

namespace nix {

static void setup_seccomp()
{
    if (!settings.filterSyscalls)
        return;

#  if HAVE_SECCOMP
    scmp_filter_ctx ctx;

    if (!(ctx = seccomp_init(SCMP_ACT_ALLOW)))
        throw sys_error_t("unable to initialize seccomp mode 2");

    finally_t cleanup([&]() { seccomp_release(ctx); });

    constexpr std::string_view nativeSystem = NIX_LOCAL_SYSTEM;

    if (nativeSystem == "x86_64-linux" && seccomp_arch_add(ctx, SCMP_ARCH_X86) != 0)
        throw sys_error_t("unable to add 32-bit seccomp architecture");

    if (nativeSystem == "x86_64-linux" && seccomp_arch_add(ctx, SCMP_ARCH_X32) != 0)
        throw sys_error_t("unable to add X32 seccomp architecture");

    if (nativeSystem == "aarch64-linux" && seccomp_arch_add(ctx, SCMP_ARCH_ARM) != 0)
        printError(
            "unable to add ARM seccomp architecture; this may result in spurious build failures if running 32-bit ARM processes");

    if (nativeSystem == "mips64-linux" && seccomp_arch_add(ctx, SCMP_ARCH_MIPS) != 0)
        printError("unable to add mips seccomp architecture");

    if (nativeSystem == "mips64-linux" && seccomp_arch_add(ctx, SCMP_ARCH_MIPS64N32) != 0)
        printError("unable to add mips64-*abin32 seccomp architecture");

    if (nativeSystem == "mips64el-linux" && seccomp_arch_add(ctx, SCMP_ARCH_MIPSEL) != 0)
        printError("unable to add mipsel seccomp architecture");

    if (nativeSystem == "mips64el-linux" && seccomp_arch_add(ctx, SCMP_ARCH_MIPSEL64N32) != 0)
        printError("unable to add mips64el-*abin32 seccomp architecture");

    /* Prevent builders from creating setuid/setgid binaries. */
    for (int perm : {S_ISUID, S_ISGID}) {
        if (seccomp_rule_add(
                ctx,
                SCMP_ACT_ERRNO(EPERM),
                SCMP_SYS(chmod),
                1,
                SCMP_A1(SCMP_CMP_MASKED_EQ, (scmp_datum_t) perm, (scmp_datum_t) perm))
            != 0)
            throw sys_error_t("unable to add seccomp rule");

        if (seccomp_rule_add(
                ctx,
                SCMP_ACT_ERRNO(EPERM),
                SCMP_SYS(fchmod),
                1,
                SCMP_A1(SCMP_CMP_MASKED_EQ, (scmp_datum_t) perm, (scmp_datum_t) perm))
            != 0)
            throw sys_error_t("unable to add seccomp rule");

        if (seccomp_rule_add(
                ctx,
                SCMP_ACT_ERRNO(EPERM),
                SCMP_SYS(fchmodat),
                1,
                SCMP_A2(SCMP_CMP_MASKED_EQ, (scmp_datum_t) perm, (scmp_datum_t) perm))
            != 0)
            throw sys_error_t("unable to add seccomp rule");

        if (seccomp_rule_add(
                ctx,
                SCMP_ACT_ERRNO(EPERM),
                NIX_SYSCALL_FCHMODAT2,
                1,
                SCMP_A2(SCMP_CMP_MASKED_EQ, (scmp_datum_t) perm, (scmp_datum_t) perm))
            != 0)
            throw sys_error_t("unable to add seccomp rule");
    }

    /* Prevent builders from using EAs or ACLs. Not all filesystems
       support these, and they're not allowed in the Nix store because
       they're not representable in the NAR serialisation. */
    if (seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOTSUP), SCMP_SYS(getxattr), 0) != 0
        || seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOTSUP), SCMP_SYS(lgetxattr), 0) != 0
        || seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOTSUP), SCMP_SYS(fgetxattr), 0) != 0
        || seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOTSUP), SCMP_SYS(setxattr), 0) != 0
        || seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOTSUP), SCMP_SYS(lsetxattr), 0) != 0
        || seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOTSUP), SCMP_SYS(fsetxattr), 0) != 0)
        throw sys_error_t("unable to add seccomp rule");

    if (seccomp_attr_set(ctx, SCMP_FLTATR_CTL_NNP, settings.allowNewPrivileges ? 0 : 1) != 0)
        throw sys_error_t("unable to set 'no new privileges' seccomp attribute");

    if (seccomp_load(ctx) != 0)
        throw sys_error_t("unable to load seccomp BPF program");
#  else
    throw Error(
        "seccomp is not supported on this platform; "
        "you can bypass this error by setting the option 'filter-syscalls' to false, but note that untrusted builds can then create setuid binaries!");
#  endif
}

static void do_bind(const Path & source, const Path & target, bool optional = false)
{
    debug("bind mounting '%1%' to '%2%'", source, target);

    auto bind_mount = [&]() {
        if (mount(source.c_str(), target.c_str(), "", MS_BIND | MS_REC, 0) == -1)
            throw sys_error_t("bind mount from '%1%' to '%2%' failed", source, target);
    };

    auto maybe_st = maybe_lstat(source);
    if (!maybe_st) {
        if (optional)
            return;
        else
            throw sys_error_t("getting attributes of path '%1%'", source);
    }
    auto st = *maybe_st;

    if (S_ISDIR(st.st_mode)) {
        create_dirs(target);
        bind_mount();
    } else if (S_ISLNK(st.st_mode)) {
        // Symlinks can (apparently) not be bind-mounted, so just copy it
        create_dirs(dir_of(target));
        copy_file(std::filesystem::path(source), std::filesystem::path(target), false);
    } else {
        create_dirs(dir_of(target));
        write_file(target, "");
        bind_mount();
    }
}

struct linux_derivation_builder_t : virtual derivation_builder_impl_t
{
    using derivation_builder_impl_t::derivation_builder_impl_t;

    void enter_chroot() override
    {
        setup_seccomp();

        linux::setPersonality(drv.platform);
    }
};

struct chroot_linux_derivation_builder_t : chroot_derivation_builder_t, linux_derivation_builder_t
{
    /**
     * pipe_t for synchronising updates to the builder namespaces.
     */
    pipe_t user_namespace_sync;

    /**
     * The mount namespace and user namespace of the builder, used to add additional
     * paths to the sandbox as a result of recursive Nix calls.
     */
    auto_close_fd_t sandbox_mount_namespace;
    auto_close_fd_t sandbox_user_namespace;

    /**
     * On Linux, whether we're doing the build in its own user
     * namespace.
     */
    bool using_user_namespace = true;

    /**
     * The cgroup of the builder, if any.
     */
    std::optional<Path> cgroup;

    chroot_linux_derivation_builder_t(
        LocalStore & store, std::unique_ptr<DerivationBuilderCallbacks> misc_methods, DerivationBuilderParams params)
        : derivation_builder_impl_t{store, std::move(misc_methods), std::move(params)}
        , chroot_derivation_builder_t{store, std::move(misc_methods), std::move(params)}
        , linux_derivation_builder_t{store, std::move(misc_methods), std::move(params)}
    {
    }

    uid_t sandbox_uid()
    {
        return using_user_namespace ? (!buildUser || buildUser->getUIDCount() == 1 ? 1000 : 0) : buildUser->getUID();
    }

    gid_t sandbox_gid() override
    {
        return using_user_namespace ? (!buildUser || buildUser->getUIDCount() == 1 ? 100 : 0)
                                  : chroot_derivation_builder_t::sandbox_gid();
    }

    std::unique_ptr<UserLock> get_build_user() override
    {
        return acquire_user_lock(drv_options.useUidRange(drv) ? 65536 : 1, true);
    }

    void prepare_user() override
    {
        if ((buildUser && buildUser->getUIDCount() != 1) || settings.useCgroups) {
            experimental_feature_settings.require(xp_t::cgroups);

            /* If we're running from the daemon, then this will return the
               root cgroup of the service. Otherwise, it will return the
               current cgroup. */
            auto root_cgroup = get_root_cgroup();
            auto cgroup_fs = get_cgroup_fs();
            if (!cgroup_fs)
                throw Error("cannot determine the cgroups file system");
            auto root_cgroup_path = canon_path(*cgroup_fs + "/" + root_cgroup);
            if (!path_exists(root_cgroup_path))
                throw Error("expected cgroup directory '%s'", root_cgroup_path);

            static std::atomic<unsigned int> counter{0};

            cgroup = buildUser ? fmt("%s/nix-build-uid-%d", root_cgroup_path, buildUser->getUID())
                               : fmt("%s/nix-build-pid-%d-%d", root_cgroup_path, getpid(), counter++);

            debug("using cgroup '%s'", *cgroup);

            /* When using a build user, record the cgroup we used for that
               user so that if we got interrupted previously, we can kill
               any left-over cgroup first. */
            if (buildUser) {
                auto cgroups_dir = settings.nixStateDir + "/cgroups";
                create_dirs(cgroups_dir);

                auto cgroup_file = fmt("%s/%d", cgroups_dir, buildUser->getUID());

                if (path_exists(cgroup_file)) {
                    auto prev_cgroup = read_file(cgroup_file);
                    destroy_cgroup(prev_cgroup);
                }

                write_file(cgroup_file, *cgroup);
            }
        }

        // Kill any processes left in the cgroup or build user.
        derivation_builder_impl_t::prepare_user();
    }

    void prepare_sandbox() override
    {
        chroot_derivation_builder_t::prepare_sandbox();

        if (cgroup) {
            if (mkdir(cgroup->c_str(), 0755) != 0)
                throw sys_error_t("creating cgroup '%s'", *cgroup);
            chown_to_builder(*cgroup);
            chown_to_builder(*cgroup + "/cgroup.procs");
            chown_to_builder(*cgroup + "/cgroup.threads");
            // chownToBuilder(*cgroup + "/cgroup.subtree_control");
        }
    }

    void start_child() override
    {
        run_child_args_t args{
#  if NIX_WITH_AWS_AUTH
            .awsCredentials = preResolveAwsCredentials(),
#  endif
        };

        /* Set up private namespaces for the build:

           - The PID namespace causes the build to start as PID 1.
             Processes outside of the chroot are not visible to those
             on the inside, but processes inside the chroot are
             visible from the outside (though with different PIDs).

           - The private mount namespace ensures that all the bind
             mounts we do will only show up in this process and its
             children, and will disappear automatically when we're
             done.

           - The private network namespace ensures that the builder
             cannot talk to the outside world (or vice versa).  It
             only has a private loopback interface. (Fixed-output
             derivations are not run in a private network namespace
             to allow functions like fetchurl to work.)

           - The IPC namespace prevents the builder from communicating
             with outside processes using SysV IPC mechanisms (shared
             memory, message queues, semaphores).  It also ensures
             that all IPC objects are destroyed when the builder
             exits.

           - The UTS namespace ensures that builders see a hostname of
             localhost rather than the actual hostname.

           We use a helper process to do the clone() to work around
           clone() being broken in multi-threaded programs due to
           at-fork handlers not being run. Note that we use
           CLONE_PARENT to ensure that the real builder is parented to
           us.
        */

        user_namespace_sync.create();

        using_user_namespace = user_namespaces_supported();

        pipe_t send_pid;
        send_pid.create();

        Pid helper = start_process([&]() {
            send_pid.read_side.close();

            /* We need to open the slave early, before
               CLONE_NEWUSER. Otherwise we get EPERM when running as
               root. */
            open_slave();

            try {
                /* Drop additional groups here because we can't do it
                   after we've created the new user namespace. */
                if (setgroups(0, 0) == -1) {
                    if (errno != EPERM)
                        throw sys_error_t("setgroups failed");
                    if (settings.requireDropSupplementaryGroups)
                        throw Error(
                            "setgroups failed. Set the require-drop-supplementary-groups option to false to skip this step.");
                }

                process_options_t options;
                options.clone_flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWIPC | CLONE_NEWUTS | CLONE_PARENT | SIGCHLD;
                if (derivation_type.isSandboxed())
                    options.clone_flags |= CLONE_NEWNET;
                if (using_user_namespace)
                    options.clone_flags |= CLONE_NEWUSER;

                pid_t child = start_process([this, args = std::move(args)]() { run_child(std::move(args)); }, options);

                write_full(send_pid.write_side.get(), fmt("%d\n", child));
                _exit(0);
            } catch (...) {
                handle_child_exception(true);
                _exit(1);
            }
        });

        send_pid.write_side.close();

        if (helper.wait() != 0) {
            process_sandbox_setup_messages();
            // Only reached if the child process didn't send an exception.
            throw Error("unable to start build process");
        }

        user_namespace_sync.read_side = -1;

        /* Make sure that we write *something* to the child in case of
           an exception. Note that merely closing
           `user_namespace_sync.write_side` doesn't work in
           multi-threaded Nix, since several child processes may have
           inherited `write_side` (and O_CLOEXEC doesn't help because
           the children may not do an execve). */
        bool user_namespace_sync_done = false;
        finally_t cleanup([&]() {
            try {
                if (!user_namespace_sync_done)
                    write_full(user_namespace_sync.write_side.get(), "0\n");
            } catch (...) {
            }
            user_namespace_sync.write_side = -1;
        });

        auto ss = tokenize_string<std::vector<std::string>>(read_line(send_pid.read_side.get()));
        assert(ss.size() == 1);
        pid = string2_int<pid_t>(ss[0]).value();

        if (using_user_namespace) {
            /* Set the UID/GID mapping of the builder's user namespace
               such that the sandbox user maps to the build user, or to
               the calling user (if build users are disabled). */
            uid_t host_uid = buildUser ? buildUser->getUID() : getuid();
            uid_t host_gid = buildUser ? buildUser->getGID() : getgid();
            uid_t nr_ids = buildUser ? buildUser->getUIDCount() : 1;

            write_file("/proc/" + std::to_string(pid) + "/uid_map", fmt("%d %d %d", sandbox_uid(), host_uid, nr_ids));

            if (!buildUser || buildUser->getUIDCount() == 1)
                write_file("/proc/" + std::to_string(pid) + "/setgroups", "deny");

            write_file("/proc/" + std::to_string(pid) + "/gid_map", fmt("%d %d %d", sandbox_gid(), host_gid, nr_ids));
        } else {
            debug("note: not using a user namespace");
            if (!buildUser)
                throw Error(
                    "cannot perform a sandboxed build because user namespaces are not enabled; check /proc/sys/user/max_user_namespaces");
        }

        /* Now that we now the sandbox uid, we can write
           /etc/passwd. */
        write_file(
            chroot_root_dir + "/etc/passwd",
            fmt("root:x:0:0:Nix build user:%3%:/noshell\n"
                "nixbld:x:%1%:%2%:Nix build user:%3%:/noshell\n"
                "nobody:x:65534:65534:Nobody:/:/noshell\n",
                sandbox_uid(),
                sandbox_gid(),
                settings.sandboxBuildDir));

        /* Save the mount- and user namespace of the child. We have to do this
         *before* the child does a chroot. */
        sandbox_mount_namespace = open(fmt("/proc/%d/ns/mnt", (pid_t) pid).c_str(), O_RDONLY);
        if (sandbox_mount_namespace.get() == -1)
            throw sys_error_t("getting sandbox mount namespace");

        if (using_user_namespace) {
            sandbox_user_namespace = open(fmt("/proc/%d/ns/user", (pid_t) pid).c_str(), O_RDONLY);
            if (sandbox_user_namespace.get() == -1)
                throw sys_error_t("getting sandbox user namespace");
        }

        /* Move the child into its own cgroup. */
        if (cgroup)
            write_file(*cgroup + "/cgroup.procs", fmt("%d", (pid_t) pid));

        /* Signal the builder that we've updated its user namespace. */
        write_full(user_namespace_sync.write_side.get(), "1\n");
        user_namespace_sync_done = true;
    }

    void enter_chroot() override
    {
        user_namespace_sync.write_side = -1;

        if (read_line(user_namespace_sync.read_side.get()) != "1")
            throw Error("user namespace initialisation failed");

        user_namespace_sync.read_side = -1;

        if (derivation_type.isSandboxed()) {

            /* Initialise the loopback interface. */
            auto_close_fd_t fd(socket(PF_INET, SOCK_DGRAM, IPPROTO_IP));
            if (!fd)
                throw sys_error_t("cannot open IP socket");

            struct ifreq ifr;
            strcpy(ifr.ifr_name, "lo");
            ifr.ifr_flags = IFF_UP | IFF_LOOPBACK | IFF_RUNNING;
            if (ioctl(fd.get(), SIOCSIFFLAGS, &ifr) == -1)
                throw sys_error_t("cannot set loopback interface flags");
        }

        /* Set the hostname etc. to fixed values. */
        char hostname[] = "localhost";
        if (sethostname(hostname, sizeof(hostname)) == -1)
            throw sys_error_t("cannot set host name");
        char domainname[] = "(none)"; // kernel default
        if (setdomainname(domainname, sizeof(domainname)) == -1)
            throw sys_error_t("cannot set domain name");

        /* Make all filesystems private.  This is necessary
           because subtrees may have been mounted as "shared"
           (MS_SHARED).  (Systemd does this, for instance.)  Even
           though we have a private mount namespace, mounting
           filesystems on top of a shared subtree still propagates
           outside of the namespace.  Making a subtree private is
           local to the namespace, though, so setting MS_PRIVATE
           does not affect the outside world. */
        if (mount(0, "/", 0, MS_PRIVATE | MS_REC, 0) == -1)
            throw sys_error_t("unable to make '/' private");

        /* Bind-mount chroot directory to itself, to treat it as a
           different filesystem from /, as needed for pivot_root. */
        if (mount(chroot_root_dir.c_str(), chroot_root_dir.c_str(), 0, MS_BIND, 0) == -1)
            throw sys_error_t("unable to bind mount '%1%'", chroot_root_dir);

        /* Bind-mount the sandbox's Nix store onto itself so that
           we can mark it as a "shared" subtree, allowing bind
           mounts made in *this* mount namespace to be propagated
           into the child namespace created by the
           unshare(CLONE_NEWNS) call below.

           Marking chroot_root_dir as MS_SHARED causes pivot_root()
           to fail with EINVAL. Don't know why. */
        Path chroot_store_dir = chroot_root_dir + store.store_dir;

        if (mount(chroot_store_dir.c_str(), chroot_store_dir.c_str(), 0, MS_BIND, 0) == -1)
            throw sys_error_t("unable to bind mount the Nix store", chroot_store_dir);

        if (mount(0, chroot_store_dir.c_str(), 0, MS_SHARED, 0) == -1)
            throw sys_error_t("unable to make '%s' shared", chroot_store_dir);

        /* Set up a nearly empty /dev, unless the user asked to
           bind-mount the host /dev. */
        strings_t ss;
        if (paths_in_chroot.find("/dev") == paths_in_chroot.end()) {
            create_dirs(chroot_root_dir + "/dev/shm");
            create_dirs(chroot_root_dir + "/dev/pts");
            ss.push_back("/dev/full");
            if (systemFeatures.count("kvm")) {
                if (path_exists("/dev/kvm")) {
                    ss.push_back("/dev/kvm");
                } else {
                    warn(
                        "KVM is enabled in system-features but /dev/kvm is not available. "
                        "QEMU builds may fall back to slow emulation. "
                        "Consider removing 'kvm' from system-features in nix.conf if KVM is not supported on this system.");
                }
            }
            ss.push_back("/dev/null");
            ss.push_back("/dev/random");
            ss.push_back("/dev/tty");
            ss.push_back("/dev/urandom");
            ss.push_back("/dev/zero");
            create_symlink("/proc/self/fd", chroot_root_dir + "/dev/fd");
            create_symlink("/proc/self/fd/0", chroot_root_dir + "/dev/stdin");
            create_symlink("/proc/self/fd/1", chroot_root_dir + "/dev/stdout");
            create_symlink("/proc/self/fd/2", chroot_root_dir + "/dev/stderr");
        }

        /* Fixed-output derivations typically need to access the
           network, so give them access to /etc/resolv.conf and so
           on. */
        if (!derivation_type.isSandboxed()) {
            // Only use nss functions to resolve hosts and
            // services. Don’t use it for anything else that may
            // be configured for this system. This limits the
            // potential impurities introduced in fixed-outputs.
            write_file(chroot_root_dir + "/etc/nsswitch.conf", "hosts: files dns\nservices: files\n");

            /* N.B. it is realistic that these paths might not exist. It
               happens when testing Nix building fixed-output derivations
               within a pure derivation. */
            for (auto & path : {"/etc/resolv.conf", "/etc/services", "/etc/hosts"})
                if (path_exists(path))
                    ss.push_back(path);

            if (settings.ca_file != "") {
                Path ca_file = settings.ca_file;
                if (path_exists(ca_file))
                    paths_in_chroot.try_emplace("/etc/ssl/certs/ca-certificates.crt", canon_path(ca_file, true), true);
            }
        }

        for (auto & i : ss) {
            // For backwards-compatibility, resolve all the symlinks in the
            // chroot paths.
            auto canonicalPath = canon_path(i, true);
            paths_in_chroot.emplace(i, canonicalPath);
        }

        /* Bind-mount all the directories from the "host"
           filesystem that we want in the chroot
           environment. */
        for (auto & i : paths_in_chroot) {
            if (i.second.source == "/proc")
                continue; // backwards compatibility

#  if HAVE_EMBEDDED_SANDBOX_SHELL
            if (i.second.source == "__embedded_sandbox_shell__") {
                static unsigned char sh[] = {
#    include "embedded-sandbox-shell.gen.h"
                };
                auto dst = chroot_root_dir + i.first;
                create_dirs(dir_of(dst));
                write_file(dst, std::string_view((const char *) sh, sizeof(sh)));
                chmod_(dst, 0555);
            } else
#  endif
            {
                do_bind(i.second.source, chroot_root_dir + i.first, i.second.optional);
            }
        }

        /* Bind a new instance of procfs on /proc. */
        create_dirs(chroot_root_dir + "/proc");
        if (mount("none", (chroot_root_dir + "/proc").c_str(), "proc", 0, 0) == -1)
            throw sys_error_t("mounting /proc");

        /* Mount sysfs on /sys. */
        if (buildUser && buildUser->getUIDCount() != 1) {
            create_dirs(chroot_root_dir + "/sys");
            if (mount("none", (chroot_root_dir + "/sys").c_str(), "sysfs", 0, 0) == -1)
                throw sys_error_t("mounting /sys");
        }

        /* Mount a new tmpfs on /dev/shm to ensure that whatever
           the builder puts in /dev/shm is cleaned up automatically. */
        if (path_exists("/dev/shm")
            && mount(
                   "none",
                   (chroot_root_dir + "/dev/shm").c_str(),
                   "tmpfs",
                   0,
                   fmt("size=%s", settings.sandboxShmSize).c_str())
                   == -1)
            throw sys_error_t("mounting /dev/shm");

        /* Mount a new devpts on /dev/pts.  Note that this
           requires the kernel to be compiled with
           CONFIG_DEVPTS_MULTIPLE_INSTANCES=y (which is the case
           if /dev/ptx/ptmx exists). */
        if (path_exists("/dev/pts/ptmx") && !path_exists(chroot_root_dir + "/dev/ptmx")
            && !paths_in_chroot.count("/dev/pts")) {
            if (mount("none", (chroot_root_dir + "/dev/pts").c_str(), "devpts", 0, "newinstance,mode=0620") == 0) {
                create_symlink("/dev/pts/ptmx", chroot_root_dir + "/dev/ptmx");

                /* Make sure /dev/pts/ptmx is world-writable.  With some
                   Linux versions, it is created with permissions 0.  */
                chmod_(chroot_root_dir + "/dev/pts/ptmx", 0666);
            } else {
                if (errno != EINVAL)
                    throw sys_error_t("mounting /dev/pts");
                do_bind("/dev/pts", chroot_root_dir + "/dev/pts");
                do_bind("/dev/ptmx", chroot_root_dir + "/dev/ptmx");
            }
        }

        /* Make /etc unwritable */
        if (!drv_options.useUidRange(drv))
            chmod_(chroot_root_dir + "/etc", 0555);

        /* Unshare this mount namespace. This is necessary because
           pivot_root() below changes the root of the mount
           namespace. This means that the call to setns() in
           addDependency() would hide the host's filesystem,
           making it impossible to bind-mount paths from the host
           Nix store into the sandbox. Therefore, we save the
           pre-pivot_root namespace in
           sandbox_mount_namespace. Since we made /nix/store a
           shared subtree above, this allows addDependency() to
           make paths appear in the sandbox. */
        if (unshare(CLONE_NEWNS) == -1)
            throw sys_error_t("unsharing mount namespace");

        /* Unshare the cgroup namespace. This means
           /proc/self/cgroup will show the child's cgroup as '/'
           rather than whatever it is in the parent. */
        if (cgroup && unshare(CLONE_NEWCGROUP) == -1)
            throw sys_error_t("unsharing cgroup namespace");

        /* Do the chroot(). */
        if (chdir(chroot_root_dir.c_str()) == -1)
            throw sys_error_t("cannot change directory to '%1%'", chroot_root_dir);

        if (mkdir("real-root", 0500) == -1)
            throw sys_error_t("cannot create real-root directory");

        if (pivot_root(".", "real-root") == -1)
            throw sys_error_t("cannot pivot old root directory onto '%1%'", (chroot_root_dir + "/real-root"));

        if (chroot(".") == -1)
            throw sys_error_t("cannot change root directory to '%1%'", chroot_root_dir);

        if (umount2("real-root", MNT_DETACH) == -1)
            throw sys_error_t("cannot unmount real root filesystem");

        if (rmdir("real-root") == -1)
            throw sys_error_t("cannot remove real-root directory");

        linux_derivation_builder_t::enter_chroot();
    }

    void set_user() override
    {
        /* Switch to the sandbox uid/gid in the user namespace,
           which corresponds to the build user or calling user in
           the parent namespace. */
        if (setgid(sandbox_gid()) == -1)
            throw sys_error_t("setgid failed");
        if (setuid(sandbox_uid()) == -1)
            throw sys_error_t("setuid failed");
    }

    SingleDrvOutputs unprepare_build() override
    {
        sandbox_mount_namespace = -1;
        sandbox_user_namespace = -1;

        return derivation_builder_impl_t::unprepare_build();
    }

    void kill_sandbox(bool get_stats) override
    {
        if (cgroup) {
            auto stats = destroy_cgroup(*cgroup);
            if (get_stats) {
                buildResult.cpu_user = stats.cpu_user;
                buildResult.cpu_system = stats.cpu_system;
            }
            return;
        }

        derivation_builder_impl_t::kill_sandbox(get_stats);
    }

    void add_dependency_impl(const StorePath & path) override
    {
        if (is_allowed(path))
            return;

        auto [source, target] = chroot_derivation_builder_t::add_dependency_prep(path);

        /* Bind-mount the path into the sandbox. This requires
           entering its mount namespace, which is not possible
           in multithreaded programs. So we do this in a
           child process.*/
        Pid child(start_process([&]() {
            if (using_user_namespace && (setns(sandbox_user_namespace.get(), 0) == -1))
                throw sys_error_t("entering sandbox user namespace");

            if (setns(sandbox_mount_namespace.get(), 0) == -1)
                throw sys_error_t("entering sandbox mount namespace");

            do_bind(source, target);

            _exit(0);
        }));

        int status = child.wait();
        if (status != 0)
            throw Error("could not add path '%s' to sandbox", store.printStorePath(path));
    }

    ActiveBuild get_active_build() override
    {
        auto build = derivation_builder_impl_t::get_active_build();
        build.cgroup = cgroup;
        return build;
    }
};

} // namespace nix

#endif
