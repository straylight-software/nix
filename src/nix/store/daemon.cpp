#include "nix/store/daemon.h"

#include "nix/store/active-builds.h"
#include "nix/store/build-result.h"
#include "nix/store/derivations.h"
#include "nix/store/gc-store.h"
#include "nix/store/globals.h"
#include "nix/store/indirect-root-store.h"
#include "nix/store/log-store.h"
#include "nix/store/path-with-outputs.h"
#include "nix/store/store-api.h"
#include "nix/store/store-cast.h"
#include "nix/store/worker-protocol-connection.h"
#include "nix/store/worker-protocol-impl.h"
#include "nix/store/worker-protocol.h"
#include "nix/util/archive.h"
#include "nix/util/args.h"
#include "nix/util/finally.h"
#include "nix/util/git.h"
#include "nix/util/logging.h"
#include "nix/util/signals.h"

#ifndef _WIN32 // TODO need graceful async exit support on Windows?
#  include "nix/util/monitor-fd.h"
#endif

#include <sstream>

namespace nix::daemon {

sink_t& operator<<(sink_t& sink, const logger_t::fields_t& fields) {
  sink << fields.size();
  for (auto& f : fields) {
    sink << f.type;
    if (f.type == logger_t::field_t::t_int)
      sink << f.i;
    else if (f.type == logger_t::field_t::t_string)
      sink << f.s;
    else
      unreachable();
  }
  return sink;
}

/* logger_t that forwards log messages to the client, *if* we're in a
   state where the protocol allows it (i.e., when can_send_stderr is
   true). */
struct tunnel_logger_t : public logger_t {
  fd_sink_t& to;

  struct State {
    bool can_send_stderr = false;
    std::vector<std::string> pending_msgs;
  };

  sync_t<State> state_;

  WorkerProto::Version client_version;

  tunnel_logger_t(fd_sink_t& to, WorkerProto::Version client_version)
      : to(to), client_version(client_version) {}

  void enqueue_msg(const std::string& s) {
    auto state(state_.lock());

    if (state->can_send_stderr) {
      assert(state->pending_msgs.empty());
      try {
        to(s);
        to.flush();
      } catch (...) {
        /* Write failed; that means that the other side is
           gone. */
        state->can_send_stderr = false;
        throw;
      }
    } else
      state->pending_msgs.push_back(s);
  }

  void log(verbosity_t lvl, std::string_view s) override {
    if (lvl > verbosity)
      return;

    string_sink_t buf;
    buf << STDERR_NEXT << (s + "\n");
    enqueue_msg(buf.str());
  }

  void log_ei(const error_info_t& ei) override {
    if (ei.level > verbosity)
      return;

    std::ostringstream oss;
    show_error_info(oss, ei, false);

    string_sink_t buf;
    buf << STDERR_NEXT << oss.view();
    enqueue_msg(buf.str());
  }

  /* start_work() means that we're starting an operation for which we
     want to send out stderr to the client. */
  void start_work() {
    auto state(state_.lock());
    state->can_send_stderr = true;

    for (auto& msg : state->pending_msgs)
      to(msg);

    state->pending_msgs.clear();

    to.flush();
  }

  /* stop_work() means that we're done; stop sending stderr to the
     client. */
  void stop_work(const Error* ex = nullptr) {
    auto state(state_.lock());

    state->can_send_stderr = false;

    if (!ex)
      to << STDERR_LAST;
    else {
      if (GET_PROTOCOL_MINOR(client_version) >= 26) {
        to << STDERR_ERROR << *ex;
      } else {
        to << STDERR_ERROR << ex->what() << ex->info().status;
      }
    }
  }

  void start_activity(activity_id_t act, verbosity_t lvl, activity_type_t type,
                      const std::string& s, const fields_t& fields, activity_id_t parent) override {
    if (GET_PROTOCOL_MINOR(client_version) < 20) {
      if (!s.empty())
        log(lvl, s + "...");
      return;
    }

    string_sink_t buf;
    buf << STDERR_START_ACTIVITY << act << static_cast<uint64_t>(lvl) << type << s << fields
        << parent;
    enqueue_msg(buf.str());
  }

  void stop_activity(activity_id_t act) override {
    if (GET_PROTOCOL_MINOR(client_version) < 20)
      return;
    string_sink_t buf;
    buf << STDERR_STOP_ACTIVITY << act;
    enqueue_msg(buf.str());
  }

  void result(activity_id_t act, result_type_t type, const fields_t& fields) override {
    if (GET_PROTOCOL_MINOR(client_version) < 20)
      return;
    string_sink_t buf;
    buf << STDERR_RESULT << act << type << fields;
    enqueue_msg(buf.str());
  }
};

struct tunnel_sink_t : sink_t {
  sink_t& to;

  tunnel_sink_t(sink_t& to) : to(to) {}

  void operator()(std::string_view data) override {
    to << STDERR_WRITE;
    write_string(data, to);
  }
};

struct tunnel_source_t : buffered_source_t {
  source_t& from;
  buffered_sink_t& to;

  tunnel_source_t(source_t& from, buffered_sink_t& to) : from(from), to(to) {}

  size_t read_unbuffered(char* data, size_t len) override {
    to << STDERR_READ << len;
    to.flush();
    size_t n = read_string(data, len, from);
    if (n == 0)
      throw EndOfFile("unexpected end-of-file");
    return n;
  }
};

struct client_settings_t {
  bool keep_failed;
  bool keep_going;
  bool try_fallback;
  verbosity_t verbosity;
  unsigned int max_build_jobs;
  time_t max_silent_time;
  bool verbose_build;
  unsigned int build_cores;
  bool use_substitutes;
  string_map_t overrides;

  void apply(TrustedFlag trusted) {
    settings.keep_failed = keep_failed;
    settings.keep_going = keep_going;
    settings.try_fallback = try_fallback;
    nix::verbosity = verbosity;
    settings.max_build_jobs.assign(max_build_jobs);
    settings.max_silent_time = max_silent_time;
    settings.verbose_build = verbose_build;
    settings.build_cores = build_cores;
    settings.use_substitutes = use_substitutes;

    for (auto& i : overrides) {
      auto& name(i.first);
      auto& value(i.second);

      auto setSubstituters = [&](setting_t<strings_t>& res) {
        if (name != res.name && res.aliases.count(name) == 0)
          return false;
        string_set_t trusted = settings.trustedSubstituters;
        for (auto& s : settings.substituters.get())
          trusted.insert(s);
        strings_t subs;
        auto ss = tokenize_string<strings_t>(value);
        for (auto& s : ss)
          if (trusted.count(s))
            subs.push_back(s);
          else if (!has_suffix(s, "/") && trusted.count(s + "/"))
            subs.push_back(s + "/");
          else
            warn("ignoring untrusted substituter '%s', you are not a trusted user.\n"
                 "Run `man nix.conf` for more information on the `substituters` configuration "
                 "option.",
                 s);
        res = subs;
        return true;
      };

      try {
        if (name == "ssh-auth-sock") // obsolete
          ;
        else if (name == experimental_feature_settings.experimental_features.name) {
          // We don’t want to forward the experimental features to
          // the daemon, as that could cause some pretty weird stuff
          if (parse_features(tokenize_string<string_set_t>(value)) !=
              experimental_feature_settings.experimental_features.get())
            debug("Ignoring the client-specified experimental features");
        } else if (name == "plugin-files") {
          warn("Ignoring the client-specified plugin-files.\n"
               "The client specifying plugins to the daemon never made sense, and was removed in "
               "Nix >=2.14.");
        } else if (trusted || name == settings.buildTimeout.name ||
                   name == settings.max_silent_time.name || name == settings.pollInterval.name ||
                   name == "connect-timeout" || (name == "builders" && value == ""))
          settings.set(name, value);
        else if (setSubstituters(settings.substituters))
          ;
        else
          warn("ignoring the client-specified setting '%s', because it is a restricted setting and "
               "you are not a trusted user",
               name);
      } catch (UsageError& e) {
        warn(e.what());
      }
    }
  }
};

static void perform_op(tunnel_logger_t* logger, ref<store_t> store, TrustedFlag trusted,
                       RecursiveFlag recursive, WorkerProto::BasicServerConnection& conn,
                       WorkerProto::Op op) {
  WorkerProto::ReadConn rconn(conn);
  WorkerProto::WriteConn wconn(conn);

  switch (op) {
    case WorkerProto::Op::IsValidPath: {
      auto path = WorkerProto::Serialise<store_path_t>::read(*store, rconn);
      logger->start_work();
      bool result = store->isValidPath(path);
      logger->stop_work();
      conn.to << result;
      break;
    }

    case WorkerProto::Op::QueryValidPaths: {
      auto paths = WorkerProto::Serialise<store_path_set_t>::read(*store, rconn);

      SubstituteFlag substitute = NoSubstitute;
      if (GET_PROTOCOL_MINOR(conn.protoVersion) >= 27) {
        substitute = read_int(conn.from) ? Substitute : NoSubstitute;
      }

      logger->start_work();
      if (substitute) {
        store->substitutePaths(paths);
      }
      auto res = store->queryValidPaths(paths, substitute);
      logger->stop_work();
      WorkerProto::write(*store, wconn, res);
      break;
    }

    case WorkerProto::Op::HasSubstitutes: {
      auto path = WorkerProto::Serialise<store_path_t>::read(*store, rconn);
      logger->start_work();
      store_path_set_t paths; // FIXME
      paths.insert(path);
      auto res = store->querySubstitutablePaths(paths);
      logger->stop_work();
      conn.to << (res.count(path) != 0);
      break;
    }

    case WorkerProto::Op::QuerySubstitutablePaths: {
      auto paths = WorkerProto::Serialise<store_path_set_t>::read(*store, rconn);
      logger->start_work();
      auto res = store->querySubstitutablePaths(paths);
      logger->stop_work();
      WorkerProto::write(*store, wconn, res);
      break;
    }

    case WorkerProto::Op::QueryPathHash: {
      auto path = WorkerProto::Serialise<store_path_t>::read(*store, rconn);
      logger->start_work();
      auto hash = store->queryPathInfo(path)->nar_hash;
      logger->stop_work();
      conn.to << hash.to_string(hash_format_t::base16, false);
      break;
    }

    case WorkerProto::Op::QueryReferences:
    case WorkerProto::Op::QueryReferrers:
    case WorkerProto::Op::QueryValidDerivers:
    case WorkerProto::Op::QueryDerivationOutputs: {
      auto path = WorkerProto::Serialise<store_path_t>::read(*store, rconn);
      logger->start_work();
      store_path_set_t paths;
      if (op == WorkerProto::Op::QueryReferences)
        for (auto& i : store->queryPathInfo(path)->references)
          paths.insert(i);
      else if (op == WorkerProto::Op::QueryReferrers)
        store->query_referrers(path, paths);
      else if (op == WorkerProto::Op::QueryValidDerivers)
        paths = store->queryValidDerivers(path);
      else
        paths = store->queryDerivationOutputs(path);
      logger->stop_work();
      WorkerProto::write(*store, wconn, paths);
      break;
    }

    case WorkerProto::Op::QueryDerivationOutputNames: {
      auto path = WorkerProto::Serialise<store_path_t>::read(*store, rconn);
      logger->start_work();
      auto names = store->read_derivation(path).outputNames();
      logger->stop_work();
      conn.to << names;
      break;
    }

    case WorkerProto::Op::QueryDerivationOutputMap: {
      auto path = WorkerProto::Serialise<store_path_t>::read(*store, rconn);
      logger->start_work();
      auto outputs = store->queryPartialDerivationOutputMap(path);
      logger->stop_work();
      WorkerProto::write(*store, wconn, outputs);
      break;
    }

    case WorkerProto::Op::QueryDeriver: {
      auto path = WorkerProto::Serialise<store_path_t>::read(*store, rconn);
      logger->start_work();
      auto info = store->queryPathInfo(path);
      logger->stop_work();
      WorkerProto::write(*store, conn, info->deriver);
      break;
    }

    case WorkerProto::Op::QueryPathFromHashPart: {
      auto hash_part = read_string(conn.from);
      logger->start_work();
      auto path = store->queryPathFromHashPart(hash_part);
      logger->stop_work();
      WorkerProto::write(*store, conn, path);
      break;
    }

    case WorkerProto::Op::AddToStore: {
      if (GET_PROTOCOL_MINOR(conn.protoVersion) >= 25) {
        auto name = read_string(conn.from);
        auto cam_str = read_string(conn.from);
        auto refs = WorkerProto::Serialise<store_path_set_t>::read(*store, rconn);
        bool repair_bool;
        conn.from >> repair_bool;
        auto repair = RepairFlag{repair_bool};

        logger->start_work();
        auto path_info = [&]() {
          // NB: FramedSource must be out of scope before logger->stopWork();
          // FIXME: this means that if there is an error
          // half-way through, the client will keep sending
          // data, since we haven't sent it the error yet.
          auto [content_address_method, hash_algo] =
              content_address_method_t::parseWithAlgo(cam_str);
          framed_source_t source(conn.from);
          file_serialisation_method_t dump_method;
          switch (content_address_method.getFileIngestionMethod()) {
            case file_ingestion_method_t::flat:
              dump_method = file_serialisation_method_t::flat;
              break;
            case file_ingestion_method_t::nix_archive:
              dump_method = file_serialisation_method_t::nix_archive;
              break;
            case file_ingestion_method_t::git:
              // Use NAR; Git is not a serialization method
              dump_method = file_serialisation_method_t::nix_archive;
              break;
            default:
              assert(false);
          }
          // TODO these two steps are essentially RemoteStore::addCAToStore. Move it up to store_t.
          auto path = store->add_to_store_from_dump(
              source, name, dump_method, content_address_method, hash_algo, refs, repair);
          return store->queryPathInfo(path);
        }();
        logger->stop_work();

        WorkerProto::Serialise<valid_path_info_t>::write(*store, wconn, *path_info);
      } else {
        hash_algorithm_t hash_algo;
        std::string base_name;
        content_address_method_t method;
        {
          bool fixed;
          uint8_t recursive;
          std::string hash_algo_raw;
          conn.from >> base_name >> fixed /* obsolete */ >> recursive >> hash_algo_raw;
          if (recursive > true)
            throw Error("unsupported FileIngestionMethod with value of %i; you may need to upgrade "
                        "nix-daemon",
                        recursive);
          method = recursive ? content_address_method_t::raw_t::nix_archive
                             : content_address_method_t::raw_t::flat;
          /* Compatibility hack. */
          if (!fixed) {
            hash_algo_raw = "sha256";
            method = content_address_method_t::raw_t::nix_archive;
          }
          hash_algo = parse_hash_algo(hash_algo_raw);
        }

        // Old protocol always sends NAR, regardless of hashing method
        auto dump_source = sink_to_source([&](sink_t& saved) {
          /* We parse the NAR dump through into `saved` unmodified,
             so why all this extra work? We still parse the NAR so
             that we aren't sending arbitrary data to `saved`
             unwittingly`, and we know when the NAR ends so we don't
             consume the rest of `conn.from` and can't parse another
             command. (We don't trust `add_to_store_from_dump` to not
             eagerly consume the entire stream it's given, past the
             length of the Nar. */
          tee_source_t saved_nar_source(conn.from, saved);
          null_file_system_object_sink_t sink; /* just parse the NAR */
          parse_dump(sink, saved_nar_source);
        });
        logger->start_work();
        auto path = store->add_to_store_from_dump(
            *dump_source, base_name, file_serialisation_method_t::nix_archive, method, hash_algo);
        logger->stop_work();

        WorkerProto::write(*store, wconn, path);
      }
      break;
    }

    case WorkerProto::Op::AddMultipleToStore: {
      bool repair, dont_check_sigs;
      conn.from >> repair >> dont_check_sigs;
      if (!trusted && dont_check_sigs)
        dont_check_sigs = false;

      logger->start_work();
      {
        framed_source_t source(conn.from);
        store->addMultipleToStore(source, RepairFlag{repair},
                                  dont_check_sigs ? NoCheckSigs : CheckSigs);
      }
      logger->stop_work();
      break;
    }

    case WorkerProto::Op::AddTextToStore: {
      std::string suffix = read_string(conn.from);
      std::string s = read_string(conn.from);
      auto refs = WorkerProto::Serialise<store_path_set_t>::read(*store, rconn);
      logger->start_work();
      auto path = ({
        string_source_t source{s};
        store->add_to_store_from_dump(source, suffix, file_serialisation_method_t::flat,
                                      content_address_method_t::raw_t::Text,
                                      hash_algorithm_t::SHA256, refs, NoRepair);
      });
      logger->stop_work();
      WorkerProto::write(*store, wconn, path);
      break;
    }

    case WorkerProto::Op::BuildPaths: {
      auto drvs = WorkerProto::Serialise<DerivedPaths>::read(*store, rconn);
      BuildMode mode = bmNormal;
      mode = WorkerProto::Serialise<BuildMode>::read(*store, rconn);

      /* Repairing is not atomic, so disallowed for "untrusted"
         clients.

         FIXME: layer violation in this message: the daemon code (i.e.
         this file) knows whether a client/connection is trusted, but it
         does not how how the client was authenticated. The mechanism
         need not be getting the UID of the other end of a Unix Domain
         socket_t.
        */
      if (mode == bmRepair && !trusted)
        throw Error("repairing is not allowed because you are not in 'trusted-users'");
      logger->start_work();
      store->build_paths(drvs, mode);
      logger->stop_work();
      conn.to << 1;
      break;
    }

    case WorkerProto::Op::BuildPathsWithResults: {
      auto drvs = WorkerProto::Serialise<DerivedPaths>::read(*store, rconn);
      BuildMode mode = bmNormal;
      mode = WorkerProto::Serialise<BuildMode>::read(*store, rconn);

      /* Repairing is not atomic, so disallowed for "untrusted"
         clients.

         FIXME: layer violation; see above. */
      if (mode == bmRepair && !trusted)
        throw Error("repairing is not allowed because you are not in 'trusted-users'");

      logger->start_work();
      auto results = store->build_paths_with_results(drvs, mode);
      logger->stop_work();

      WorkerProto::write(*store, wconn, results);

      break;
    }

    case WorkerProto::Op::BuildDerivation: {
      auto drv_path = WorkerProto::Serialise<store_path_t>::read(*store, rconn);
      basic_derivation_t drv;
      /*
       * Note: unlike wopEnsurePath, this operation reads a
       * derivation-to-be-realized from the client with
       * read_derivation(source_t,store_t) rather than reading it from
       * the local store with store_t::read_derivation().  Since the
       * derivation-to-be-realized is not registered in the store
       * it cannot be trusted that its out_path was calculated
       * correctly.
       */
      read_derivation(conn.from, *store, drv, derivation_t::nameFromPath(drv_path));
      auto build_mode = WorkerProto::Serialise<BuildMode>::read(*store, rconn);
      logger->start_work();

      auto drv_type = drv.type();

      /* Content-addressing derivations are trustless because their output paths
         are verified by their content alone, so any derivation is free to
         try to produce such a path.

         input_t-addressed derivation output paths, however, are calculated
         from the derivation closure that produced them---even knowing the
         root derivation is not enough. That the output data actually came
         from those derivations is fundamentally unverifiable, but the daemon
         trusts itself on that matter. The question instead is whether the
         submitted plan has rights to the output paths it wants to fill, and
         at least the derivation closure proves that.

         It would have been nice if input-address algorithm merely depended
         on the build time closure, rather than depending on the derivation
         closure. That would mean input-addressed paths used at build time
         would just be trusted and not need their own evidence. This is in
         fact fine as the same guarantees would hold *inductively*: either
         the remote builder has those paths and already trusts them, or it
         needs to build them too and thus their evidence must be provided in
         turn.  The advantage of this variant algorithm is that the evidence
         for input-addressed paths which the remote builder already has
         doesn't need to be sent again.

         That said, now that we have floating CA derivations, it is better
         that people just migrate to those which also solve this problem, and
         others. It's the same migration difficulty with strictly more
         benefit.

         Lastly, do note that when we parse fixed-output content-addressed
         derivations, we throw out the precomputed output paths and just
         store the hashes, so there aren't two competing sources of truth an
         attacker could exploit. */
      if (!(drv_type.isCA() || trusted))
        throw Error("you are not privileged to build input-addressed derivations");

      /* Make sure that the non-input-addressed derivations that got this far
         are in fact content-addressed if we don't trust them. */
      assert(drv_type.isCA() || trusted);

      /* Recompute the derivation path when we cannot trust the original. */
      if (!trusted) {
        /* Recomputing the derivation path for input-address derivations
           makes it harder to audit them after the fact, since we need the
           original not-necessarily-resolved derivation to verify the drv
           derivation as adequate claim to the input-addressed output
           paths. */
        assert(drv_type.isCA());

        derivation_t drv2;
        static_cast<basic_derivation_t&>(drv2) = drv;
        drv_path = write_derivation(*store, derivation_t{drv2});
      }

      auto res = store->buildDerivation(drv_path, drv, build_mode);
      logger->stop_work();
      WorkerProto::write(*store, wconn, res);
      break;
    }

    case WorkerProto::Op::EnsurePath: {
      auto path = WorkerProto::Serialise<store_path_t>::read(*store, rconn);
      logger->start_work();
      store->ensure_path(path);
      logger->stop_work();
      conn.to << 1;
      break;
    }

    case WorkerProto::Op::AddTempRoot: {
      auto path = WorkerProto::Serialise<store_path_t>::read(*store, rconn);
      logger->start_work();
      store->addTempRoot(path);
      logger->stop_work();
      conn.to << 1;
      break;
    }

    case WorkerProto::Op::AddPermRoot: {
      if (!trusted)
        throw Error("you are not privileged to create perm roots\n\n"
                    "hint: you can just do this client-side without special privileges, and "
                    "probably want to do that instead.");
      auto store_path = WorkerProto::Serialise<store_path_t>::read(*store, rconn);
      Path gc_root = abs_path(read_string(conn.from));
      logger->start_work();
      auto& lfs_store = require<local_fs_store>(*store);
      lfs_store.addPermRoot(store_path, gc_root);
      logger->stop_work();
      conn.to << gc_root;
      break;
    }

    case WorkerProto::Op::AddIndirectRoot: {
      Path path = abs_path(read_string(conn.from));

      logger->start_work();
      auto& indirect_root_store = require<IndirectRootStore>(*store);
      indirect_root_store.addIndirectRoot(path);
      logger->stop_work();

      conn.to << 1;
      break;
    }

    // Obsolete.
    case WorkerProto::Op::SyncWithGC: {
      logger->start_work();
      logger->stop_work();
      conn.to << 1;
      break;
    }

    case WorkerProto::Op::FindRoots: {
      logger->start_work();
      auto& gc_store = require<GcStore>(*store);
      Roots roots = gc_store.findRoots(!trusted);
      logger->stop_work();

      size_t size = 0;
      for (auto& i : roots)
        size += i.second.size();

      conn.to << size;

      for (auto& [target, links] : roots)
        for (auto& link : links) {
          conn.to << link;
          WorkerProto::write(*store, wconn, target);
        }

      break;
    }

    case WorkerProto::Op::CollectGarbage: {
      GCOptions options;
      options.action = WorkerProto::Serialise<GCOptions::GCAction>::read(*store, rconn);
      options.pathsToDelete = WorkerProto::Serialise<store_path_set_t>::read(*store, rconn);
      conn.from >> options.ignoreLiveness >> options.maxFreed;
      options.censor = !trusted;
      // obsolete fields
      read_int(conn.from);
      read_int(conn.from);
      read_int(conn.from);

      GCResults results;

      logger->start_work();
      if (options.ignoreLiveness && !get_env("_NIX_IN_TEST").has_value())
        throw Error("you are not allowed to ignore liveness");
      auto& gc_store = require<GcStore>(*store);
      gc_store.collectGarbage(options, results);
      logger->stop_work();

      conn.to << results.paths << results.bytes_freed << 0 /* obsolete */;

      break;
    }

    case WorkerProto::Op::SetOptions: {
      client_settings_t client_settings;

      client_settings.keep_failed = read_int(conn.from);
      client_settings.keep_going = read_int(conn.from);
      client_settings.try_fallback = read_int(conn.from);
      client_settings.verbosity = (verbosity_t)read_int(conn.from);
      client_settings.max_build_jobs = read_int(conn.from);
      client_settings.max_silent_time = read_int(conn.from);
      read_int(conn.from); // obsolete useBuildHook
      client_settings.verbose_build = lvl_error == (verbosity_t)read_int(conn.from);
      read_int(conn.from); // obsolete logType
      read_int(conn.from); // obsolete printBuildTrace
      client_settings.build_cores = read_int(conn.from);
      client_settings.use_substitutes = read_int(conn.from);

      unsigned int n = read_int(conn.from);
      for (unsigned int i = 0; i < n; i++) {
        auto name = read_string(conn.from);
        auto value = read_string(conn.from);
        client_settings.overrides.emplace(name, value);
      }

      logger->start_work();

      // FIXME: use some setting in recursive mode. Will need to use
      // non-global variables.
      if (!recursive)
        client_settings.apply(trusted);

      logger->stop_work();
      break;
    }

    case WorkerProto::Op::QuerySubstitutablePathInfo: {
      auto path = WorkerProto::Serialise<store_path_t>::read(*store, rconn);
      logger->start_work();
      SubstitutablePathInfos infos;
      store->querySubstitutablePathInfos({{path, std::nullopt}}, infos);
      logger->stop_work();
      auto i = infos.find(path);
      if (i == infos.end())
        conn.to << 0;
      else {
        conn.to << 1;
        WorkerProto::write(*store, wconn, i->second.deriver);
        WorkerProto::write(*store, wconn, i->second.references);
        conn.to << i->second.downloadSize << i->second.nar_size;
      }
      break;
    }

    case WorkerProto::Op::QuerySubstitutablePathInfos: {
      SubstitutablePathInfos infos;
      StorePathCAMap paths_map = {};
      if (GET_PROTOCOL_MINOR(conn.protoVersion) < 22) {
        auto paths = WorkerProto::Serialise<store_path_set_t>::read(*store, rconn);
        for (auto& path : paths)
          paths_map.emplace(path, std::nullopt);
      } else
        paths_map = WorkerProto::Serialise<StorePathCAMap>::read(*store, rconn);
      logger->start_work();
      store->querySubstitutablePathInfos(paths_map, infos);
      logger->stop_work();
      conn.to << infos.size();
      for (auto& i : infos) {
        WorkerProto::write(*store, wconn, i.first);
        WorkerProto::write(*store, wconn, i.second.deriver);
        WorkerProto::write(*store, wconn, i.second.references);
        conn.to << i.second.downloadSize << i.second.nar_size;
      }
      break;
    }

    case WorkerProto::Op::QueryAllValidPaths: {
      logger->start_work();
      auto paths = store->query_all_valid_paths();
      logger->stop_work();
      WorkerProto::write(*store, wconn, paths);
      break;
    }

    case WorkerProto::Op::QueryPathInfo: {
      auto path = WorkerProto::Serialise<store_path_t>::read(*store, rconn);
      std::shared_ptr<const valid_path_info_t> info;
      logger->start_work();
      info = store->queryPathInfo(path);
      logger->stop_work();
      if (info) {
        conn.to << 1;
        WorkerProto::write(*store, wconn, static_cast<const UnkeyedValidPathInfo&>(*info));
      } else {
        conn.to << 0;
      }
      break;
    }

    case WorkerProto::Op::OptimiseStore:
      logger->start_work();
      store->optimiseStore();
      logger->stop_work();
      conn.to << 1;
      break;

    case WorkerProto::Op::VerifyStore: {
      bool check_contents, repair;
      conn.from >> check_contents >> repair;
      logger->start_work();
      if (repair && !trusted)
        throw Error("you are not privileged to repair paths");
      bool errors = store->verifyStore(check_contents, (RepairFlag)repair);
      logger->stop_work();
      conn.to << errors;
      break;
    }

    case WorkerProto::Op::AddSignatures: {
      auto path = WorkerProto::Serialise<store_path_t>::read(*store, rconn);
      string_set_t sigs = read_strings<string_set_t>(conn.from);
      logger->start_work();
      store->addSignatures(path, sigs);
      logger->stop_work();
      conn.to << 1;
      break;
    }

    case WorkerProto::Op::NarFromPath: {
      auto path = WorkerProto::Serialise<store_path_t>::read(*store, rconn);
      logger->start_work();
      logger->stop_work();
      store->nar_from_path(path, conn.to);
      break;
    }

    case WorkerProto::Op::AddToStoreNar: {
      bool repair, dont_check_sigs;
      auto path = WorkerProto::Serialise<store_path_t>::read(*store, rconn);
      auto deriver = WorkerProto::Serialise<std::optional<store_path_t>>::read(*store, rconn);
      auto nar_hash = Hash::parse_any(read_string(conn.from), hash_algorithm_t::SHA256);
      valid_path_info_t info{path, {*store, nar_hash}};
      info.deriver = std::move(deriver);
      info.references = WorkerProto::Serialise<store_path_set_t>::read(*store, rconn);
      conn.from >> info.registrationTime >> info.nar_size >> info.ultimate;
      info.sigs = read_strings<string_set_t>(conn.from);
      info.ca = content_address_t::parseOpt(read_string(conn.from));
      conn.from >> repair >> dont_check_sigs;
      if (!trusted && dont_check_sigs)
        dont_check_sigs = false;
      if (!trusted)
        info.ultimate = false;

      if (GET_PROTOCOL_MINOR(conn.protoVersion) >= 23) {
        logger->start_work();
        {
          framed_source_t source(conn.from);
          store->add_to_store(info, source, (RepairFlag)repair,
                              dont_check_sigs ? NoCheckSigs : CheckSigs);
        }
        logger->stop_work();
      }

      else {
        std::unique_ptr<source_t> source;
        string_sink_t saved;
        if (GET_PROTOCOL_MINOR(conn.protoVersion) >= 21)
          source = std::make_unique<tunnel_source_t>(conn.from, conn.to);
        else {
          tee_source_t tee{conn.from, saved};
          null_file_system_object_sink_t ether;
          parse_dump(ether, tee);
          source = std::make_unique<string_source_t>(saved.str());
        }

        logger->start_work();

        // FIXME: race if addToStore doesn't read source?
        store->add_to_store(info, *source, (RepairFlag)repair,
                            dont_check_sigs ? NoCheckSigs : CheckSigs);

        logger->stop_work();
      }

      break;
    }

    case WorkerProto::Op::QueryMissing: {
      auto targets = WorkerProto::Serialise<DerivedPaths>::read(*store, rconn);
      logger->start_work();
      auto missing = store->query_missing(targets);
      logger->stop_work();
      WorkerProto::write(*store, wconn, missing.willBuild);
      WorkerProto::write(*store, wconn, missing.willSubstitute);
      WorkerProto::write(*store, wconn, missing.unknown);
      conn.to << missing.downloadSize << missing.nar_size;
      break;
    }

    case WorkerProto::Op::RegisterDrvOutput: {
      logger->start_work();
      if (GET_PROTOCOL_MINOR(conn.protoVersion) < 31) {
        auto output_id = WorkerProto::Serialise<DrvOutput>::read(*store, rconn);
        auto output_path = store_path_t(read_string(conn.from));
        store->register_drv_output(realisation_t{{.out_path = output_path}, output_id});
      } else {
        auto realisation = WorkerProto::Serialise<realisation_t>::read(*store, rconn);
        store->register_drv_output(realisation);
      }
      logger->stop_work();
      break;
    }

    case WorkerProto::Op::QueryRealisation: {
      logger->start_work();
      auto output_id = WorkerProto::Serialise<DrvOutput>::read(*store, rconn);
      auto info = store->query_realisation(output_id);
      logger->stop_work();
      if (GET_PROTOCOL_MINOR(conn.protoVersion) < 31) {
        std::set<store_path_t> out_paths;
        if (info)
          out_paths.insert(info->out_path);
        WorkerProto::write(*store, wconn, out_paths);
      } else {
        std::set<realisation_t> realisations;
        if (info)
          realisations.insert({*info, output_id});
        WorkerProto::write(*store, wconn, realisations);
      }
      break;
    }

    case WorkerProto::Op::AddBuildLog: {
      store_path_t path{read_string(conn.from)};
      logger->start_work();
      if (!trusted)
        throw Error("you are not privileged to add logs");
      auto& log_store = require<LogStore>(*store);
      {
        framed_source_t source(conn.from);
        string_sink_t sink;
        source.drain_into(sink);
        log_store.addBuildLog(path, sink.str());
      }
      logger->stop_work();
      conn.to << 1;
      break;
    }

    case WorkerProto::Op::QueryFailedPaths:
    case WorkerProto::Op::ClearFailedPaths:
      throw Error("Removed operation %1%", op);

    case WorkerProto::Op::QueryActiveBuilds: {
      logger->start_work();
      auto& active_builds_store = require<QueryActiveBuildsStore>(*store);
      auto active_builds = active_builds_store.queryActiveBuilds();
      logger->stop_work();
      conn.to << nlohmann::json(active_builds).dump();
      break;
    }

    default:
      throw Error("invalid operation %1%", op);
  }
}

void process_connection(ref<store_t> store, fd_source_t&& from, fd_sink_t&& to, TrustedFlag trusted,
                        RecursiveFlag recursive) {
#ifndef _WIN32 // TODO need graceful async exit support on Windows?
  auto monitor = !recursive ? std::make_unique<monitor_fd_hup_t>(from.fd()) : nullptr;
  (void)monitor; // suppress warning
  receive_interrupts_t receive_interrupts;
#endif

  /* Exchange the greeting. */
  auto [protoVersion, features] = WorkerProto::BasicServerConnection::handshake(
      to, from, PROTOCOL_VERSION, WorkerProto::allFeatures);

  if (protoVersion < MINIMUM_PROTOCOL_VERSION)
    throw Error("the Nix client version is too old");

  WorkerProto::BasicServerConnection conn;
  conn.to = std::move(to);
  conn.from = std::move(from);
  conn.protoVersion = protoVersion;
  conn.features = features;

  auto tunnel_logger_ = std::make_unique<tunnel_logger_t>(conn.to, protoVersion);
  auto tunnel_logger = tunnel_logger_.get();
  std::unique_ptr<logger_t> prevLogger_;
  auto prev_logger = logger.get();
  // FIXME
  if (!recursive) {
    prevLogger_ = std::move(logger);
    logger = std::move(tunnel_logger_);
    apply_json_logger();
  }

  unsigned int op_count = 0;

  finally_t finally([&]() {
    set_interrupted(false);
    printMsgUsing(prev_logger, lvl_debug, "%d operations", op_count);
  });

  conn.postHandshake(
      *store, {
                  .daemonNixVersion = nix_version,
                  // We and the underlying store both need to trust the client for
                  // it to be trusted.
                  .remoteTrustsUs = trusted ? store->isTrustedClient() : std::optional{NotTrusted},
              });

  /* Send startup error messages to the client. */
  tunnel_logger->start_work();

  try {
    tunnel_logger->stop_work();
    conn.to.flush();

    /* Process client requests. */
    while (true) {
      WorkerProto::Op op;
      try {
        op = (enum WorkerProto::Op)read_int(conn.from);
      } catch (Interrupted& e) {
        break;
      } catch (EndOfFile& e) {
        break;
      }

      printMsgUsing(prev_logger, lvl_debug, "received daemon op %d", op);

      op_count++;

      debug("performing daemon worker op: %d", op);

      try {
        perform_op(tunnel_logger, store, trusted, recursive, conn, op);
      } catch (Error& e) {
        /* If we're not in a state where we can send replies, then
           something went wrong processing the input of the
           client.  This can happen especially if I/O errors occur
           during addTextToStore() / importPath().  If that
           happens, just send the error message and exit. */
        bool error_allowed = tunnel_logger->state_.lock()->can_send_stderr;
        tunnel_logger->stop_work(&e);
        if (!error_allowed)
          throw;
      } catch (std::bad_alloc& e) {
        auto ex = Error("Nix daemon out of memory");
        tunnel_logger->stop_work(&ex);
        throw;
      }

      conn.to.flush();

      assert(!tunnel_logger->state_.lock()->can_send_stderr);
    };

  } catch (Error& e) {
    tunnel_logger->stop_work(&e);
    conn.to.flush();
    return;
  } catch (std::exception& e) {
    auto ex = Error(e.what());
    tunnel_logger->stop_work(&ex);
    conn.to.flush();
    return;
  }
}

} // namespace nix::daemon
