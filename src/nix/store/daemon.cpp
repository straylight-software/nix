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

Sink& operator<<(Sink& sink, const Logger::fields_t& fields) {
  sink << fields.size();
  for (auto& f : fields) {
    sink << f.type;
    if (f.type == Logger::field_t::tInt)
      sink << f.i;
    else if (f.type == Logger::field_t::tString)
      sink << f.s;
    else
      unreachable();
  }
  return sink;
}

/* Logger that forwards log messages to the client, *if* we're in a
   state where the protocol allows it (i.e., when canSendStderr is
   true). */
struct tunnel_logger_t : public Logger {
  fd_sink_t& to;

  struct State {
    bool canSendStderr = false;
    std::vector<std::string> pendingMsgs;
  };

  sync_t<State> state_;

  WorkerProto::Version clientVersion;

  tunnel_logger_t(fd_sink_t& to, WorkerProto::Version clientVersion)
      : to(to), clientVersion(clientVersion) {}

  void enqueueMsg(const std::string& s) {
    auto state(state_.lock());

    if (state->canSendStderr) {
      assert(state->pendingMsgs.empty());
      try {
        to(s);
        to.flush();
      } catch (...) {
        /* Write failed; that means that the other side is
           gone. */
        state->canSendStderr = false;
        throw;
      }
    } else
      state->pendingMsgs.push_back(s);
  }

  void log(verbosity_t lvl, std::string_view s) override {
    if (lvl > verbosity)
      return;

    string_sink_t buf;
    buf << STDERR_NEXT << (s + "\n");
    enqueueMsg(buf.s);
  }

  void logEI(const error_info_t& ei) override {
    if (ei.level > verbosity)
      return;

    std::ostringstream oss;
    showErrorInfo(oss, ei, false);

    string_sink_t buf;
    buf << STDERR_NEXT << oss.view();
    enqueueMsg(buf.s);
  }

  /* startWork() means that we're starting an operation for which we
     want to send out stderr to the client. */
  void startWork() {
    auto state(state_.lock());
    state->canSendStderr = true;

    for (auto& msg : state->pendingMsgs)
      to(msg);

    state->pendingMsgs.clear();

    to.flush();
  }

  /* stopWork() means that we're done; stop sending stderr to the
     client. */
  void stopWork(const Error* ex = nullptr) {
    auto state(state_.lock());

    state->canSendStderr = false;

    if (!ex)
      to << STDERR_LAST;
    else {
      if (GET_PROTOCOL_MINOR(clientVersion) >= 26) {
        to << STDERR_ERROR << *ex;
      } else {
        to << STDERR_ERROR << ex->what() << ex->info().status;
      }
    }
  }

  void startActivity(activity_id_t act, verbosity_t lvl, activity_type_t type, const std::string& s,
                     const fields_t& fields, activity_id_t parent) override {
    if (GET_PROTOCOL_MINOR(clientVersion) < 20) {
      if (!s.empty())
        log(lvl, s + "...");
      return;
    }

    string_sink_t buf;
    buf << STDERR_START_ACTIVITY << act << lvl << type << s << fields << parent;
    enqueueMsg(buf.s);
  }

  void stopActivity(activity_id_t act) override {
    if (GET_PROTOCOL_MINOR(clientVersion) < 20)
      return;
    string_sink_t buf;
    buf << STDERR_STOP_ACTIVITY << act;
    enqueueMsg(buf.s);
  }

  void result(activity_id_t act, result_type_t type, const fields_t& fields) override {
    if (GET_PROTOCOL_MINOR(clientVersion) < 20)
      return;
    string_sink_t buf;
    buf << STDERR_RESULT << act << type << fields;
    enqueueMsg(buf.s);
  }
};

struct tunnel_sink_t : Sink {
  Sink& to;

  tunnel_sink_t(Sink& to) : to(to) {}

  void operator()(std::string_view data) override {
    to << STDERR_WRITE;
    writeString(data, to);
  }
};

struct tunnel_source_t : buffered_source_t {
  Source& from;
  buffered_sink_t& to;

  tunnel_source_t(Source& from, buffered_sink_t& to) : from(from), to(to) {}

  size_t readUnbuffered(char* data, size_t len) override {
    to << STDERR_READ << len;
    to.flush();
    size_t n = readString(data, len, from);
    if (n == 0)
      throw EndOfFile("unexpected end-of-file");
    return n;
  }
};

struct client_settings_t {
  bool keepFailed;
  bool keepGoing;
  bool tryFallback;
  verbosity_t verbosity;
  unsigned int maxBuildJobs;
  time_t maxSilentTime;
  bool verboseBuild;
  unsigned int buildCores;
  bool useSubstitutes;
  string_map_t overrides;

  void apply(TrustedFlag trusted) {
    settings.keepFailed = keepFailed;
    settings.keepGoing = keepGoing;
    settings.tryFallback = tryFallback;
    nix::verbosity = verbosity;
    settings.maxBuildJobs.assign(maxBuildJobs);
    settings.maxSilentTime = maxSilentTime;
    settings.verboseBuild = verboseBuild;
    settings.buildCores = buildCores;
    settings.useSubstitutes = useSubstitutes;

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
        auto ss = tokenizeString<strings_t>(value);
        for (auto& s : ss)
          if (trusted.count(s))
            subs.push_back(s);
          else if (!hasSuffix(s, "/") && trusted.count(s + "/"))
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
        else if (name == experimentalFeatureSettings.experimentalFeatures.name) {
          // We don’t want to forward the experimental features to
          // the daemon, as that could cause some pretty weird stuff
          if (parseFeatures(tokenizeString<string_set_t>(value)) !=
              experimentalFeatureSettings.experimentalFeatures.get())
            debug("Ignoring the client-specified experimental features");
        } else if (name == "plugin-files") {
          warn("Ignoring the client-specified plugin-files.\n"
               "The client specifying plugins to the daemon never made sense, and was removed in "
               "Nix >=2.14.");
        } else if (trusted || name == settings.buildTimeout.name ||
                   name == settings.maxSilentTime.name || name == settings.pollInterval.name ||
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

static void performOp(tunnel_logger_t* logger, ref<Store> store, TrustedFlag trusted,
                      RecursiveFlag recursive, WorkerProto::BasicServerConnection& conn,
                      WorkerProto::Op op) {
  WorkerProto::ReadConn rconn(conn);
  WorkerProto::WriteConn wconn(conn);

  switch (op) {
    case WorkerProto::Op::IsValidPath: {
      auto path = WorkerProto::Serialise<StorePath>::read(*store, rconn);
      logger->startWork();
      bool result = store->isValidPath(path);
      logger->stopWork();
      conn.to << result;
      break;
    }

    case WorkerProto::Op::QueryValidPaths: {
      auto paths = WorkerProto::Serialise<StorePathSet>::read(*store, rconn);

      SubstituteFlag substitute = NoSubstitute;
      if (GET_PROTOCOL_MINOR(conn.protoVersion) >= 27) {
        substitute = readInt(conn.from) ? Substitute : NoSubstitute;
      }

      logger->startWork();
      if (substitute) {
        store->substitutePaths(paths);
      }
      auto res = store->queryValidPaths(paths, substitute);
      logger->stopWork();
      WorkerProto::write(*store, wconn, res);
      break;
    }

    case WorkerProto::Op::HasSubstitutes: {
      auto path = WorkerProto::Serialise<StorePath>::read(*store, rconn);
      logger->startWork();
      StorePathSet paths; // FIXME
      paths.insert(path);
      auto res = store->querySubstitutablePaths(paths);
      logger->stopWork();
      conn.to << (res.count(path) != 0);
      break;
    }

    case WorkerProto::Op::QuerySubstitutablePaths: {
      auto paths = WorkerProto::Serialise<StorePathSet>::read(*store, rconn);
      logger->startWork();
      auto res = store->querySubstitutablePaths(paths);
      logger->stopWork();
      WorkerProto::write(*store, wconn, res);
      break;
    }

    case WorkerProto::Op::QueryPathHash: {
      auto path = WorkerProto::Serialise<StorePath>::read(*store, rconn);
      logger->startWork();
      auto hash = store->queryPathInfo(path)->narHash;
      logger->stopWork();
      conn.to << hash.to_string(hash_format_t::Base16, false);
      break;
    }

    case WorkerProto::Op::QueryReferences:
    case WorkerProto::Op::QueryReferrers:
    case WorkerProto::Op::QueryValidDerivers:
    case WorkerProto::Op::QueryDerivationOutputs: {
      auto path = WorkerProto::Serialise<StorePath>::read(*store, rconn);
      logger->startWork();
      StorePathSet paths;
      if (op == WorkerProto::Op::QueryReferences)
        for (auto& i : store->queryPathInfo(path)->references)
          paths.insert(i);
      else if (op == WorkerProto::Op::QueryReferrers)
        store->queryReferrers(path, paths);
      else if (op == WorkerProto::Op::QueryValidDerivers)
        paths = store->queryValidDerivers(path);
      else
        paths = store->queryDerivationOutputs(path);
      logger->stopWork();
      WorkerProto::write(*store, wconn, paths);
      break;
    }

    case WorkerProto::Op::QueryDerivationOutputNames: {
      auto path = WorkerProto::Serialise<StorePath>::read(*store, rconn);
      logger->startWork();
      auto names = store->readDerivation(path).outputNames();
      logger->stopWork();
      conn.to << names;
      break;
    }

    case WorkerProto::Op::QueryDerivationOutputMap: {
      auto path = WorkerProto::Serialise<StorePath>::read(*store, rconn);
      logger->startWork();
      auto outputs = store->queryPartialDerivationOutputMap(path);
      logger->stopWork();
      WorkerProto::write(*store, wconn, outputs);
      break;
    }

    case WorkerProto::Op::QueryDeriver: {
      auto path = WorkerProto::Serialise<StorePath>::read(*store, rconn);
      logger->startWork();
      auto info = store->queryPathInfo(path);
      logger->stopWork();
      WorkerProto::write(*store, conn, info->deriver);
      break;
    }

    case WorkerProto::Op::QueryPathFromHashPart: {
      auto hashPart = readString(conn.from);
      logger->startWork();
      auto path = store->queryPathFromHashPart(hashPart);
      logger->stopWork();
      WorkerProto::write(*store, conn, path);
      break;
    }

    case WorkerProto::Op::AddToStore: {
      if (GET_PROTOCOL_MINOR(conn.protoVersion) >= 25) {
        auto name = readString(conn.from);
        auto camStr = readString(conn.from);
        auto refs = WorkerProto::Serialise<StorePathSet>::read(*store, rconn);
        bool repairBool;
        conn.from >> repairBool;
        auto repair = RepairFlag{repairBool};

        logger->startWork();
        auto pathInfo = [&]() {
          // NB: FramedSource must be out of scope before logger->stopWork();
          // FIXME: this means that if there is an error
          // half-way through, the client will keep sending
          // data, since we haven't sent it the error yet.
          auto [contentAddressMethod, hashAlgo] = ContentAddressMethod::parseWithAlgo(camStr);
          framed_source_t source(conn.from);
          file_serialisation_method_t dumpMethod;
          switch (contentAddressMethod.getFileIngestionMethod()) {
            case file_ingestion_method_t::Flat:
              dumpMethod = file_serialisation_method_t::Flat;
              break;
            case file_ingestion_method_t::NixArchive:
              dumpMethod = file_serialisation_method_t::NixArchive;
              break;
            case file_ingestion_method_t::Git:
              // Use NAR; Git is not a serialization method
              dumpMethod = file_serialisation_method_t::NixArchive;
              break;
            default:
              assert(false);
          }
          // TODO these two steps are essentially RemoteStore::addCAToStore. Move it up to Store.
          auto path = store->addToStoreFromDump(source, name, dumpMethod, contentAddressMethod,
                                                hashAlgo, refs, repair);
          return store->queryPathInfo(path);
        }();
        logger->stopWork();

        WorkerProto::Serialise<ValidPathInfo>::write(*store, wconn, *pathInfo);
      } else {
        hash_algorithm_t hashAlgo;
        std::string baseName;
        ContentAddressMethod method;
        {
          bool fixed;
          uint8_t recursive;
          std::string hashAlgoRaw;
          conn.from >> baseName >> fixed /* obsolete */ >> recursive >> hashAlgoRaw;
          if (recursive > true)
            throw Error("unsupported FileIngestionMethod with value of %i; you may need to upgrade "
                        "nix-daemon",
                        recursive);
          method =
              recursive ? ContentAddressMethod::raw_t::NixArchive : ContentAddressMethod::raw_t::Flat;
          /* Compatibility hack. */
          if (!fixed) {
            hashAlgoRaw = "sha256";
            method = ContentAddressMethod::raw_t::NixArchive;
          }
          hashAlgo = parseHashAlgo(hashAlgoRaw);
        }

        // Old protocol always sends NAR, regardless of hashing method
        auto dumpSource = sinkToSource([&](Sink& saved) {
          /* We parse the NAR dump through into `saved` unmodified,
             so why all this extra work? We still parse the NAR so
             that we aren't sending arbitrary data to `saved`
             unwittingly`, and we know when the NAR ends so we don't
             consume the rest of `conn.from` and can't parse another
             command. (We don't trust `addToStoreFromDump` to not
             eagerly consume the entire stream it's given, past the
             length of the Nar. */
          tee_source_t savedNARSource(conn.from, saved);
          null_file_system_object_sink_t sink; /* just parse the NAR */
          parseDump(sink, savedNARSource);
        });
        logger->startWork();
        auto path = store->addToStoreFromDump(
            *dumpSource, baseName, file_serialisation_method_t::NixArchive, method, hashAlgo);
        logger->stopWork();

        WorkerProto::write(*store, wconn, path);
      }
      break;
    }

    case WorkerProto::Op::AddMultipleToStore: {
      bool repair, dontCheckSigs;
      conn.from >> repair >> dontCheckSigs;
      if (!trusted && dontCheckSigs)
        dontCheckSigs = false;

      logger->startWork();
      {
        framed_source_t source(conn.from);
        store->addMultipleToStore(source, RepairFlag{repair},
                                  dontCheckSigs ? NoCheckSigs : CheckSigs);
      }
      logger->stopWork();
      break;
    }

    case WorkerProto::Op::AddTextToStore: {
      std::string suffix = readString(conn.from);
      std::string s = readString(conn.from);
      auto refs = WorkerProto::Serialise<StorePathSet>::read(*store, rconn);
      logger->startWork();
      auto path = ({
        string_source_t source{s};
        store->addToStoreFromDump(source, suffix, file_serialisation_method_t::Flat,
                                  ContentAddressMethod::raw_t::Text, hash_algorithm_t::SHA256, refs,
                                  NoRepair);
      });
      logger->stopWork();
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
      logger->startWork();
      store->buildPaths(drvs, mode);
      logger->stopWork();
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

      logger->startWork();
      auto results = store->buildPathsWithResults(drvs, mode);
      logger->stopWork();

      WorkerProto::write(*store, wconn, results);

      break;
    }

    case WorkerProto::Op::BuildDerivation: {
      auto drvPath = WorkerProto::Serialise<StorePath>::read(*store, rconn);
      BasicDerivation drv;
      /*
       * Note: unlike wopEnsurePath, this operation reads a
       * derivation-to-be-realized from the client with
       * readDerivation(Source,Store) rather than reading it from
       * the local store with Store::readDerivation().  Since the
       * derivation-to-be-realized is not registered in the store
       * it cannot be trusted that its outPath was calculated
       * correctly.
       */
      readDerivation(conn.from, *store, drv, Derivation::nameFromPath(drvPath));
      auto buildMode = WorkerProto::Serialise<BuildMode>::read(*store, rconn);
      logger->startWork();

      auto drvType = drv.type();

      /* Content-addressing derivations are trustless because their output paths
         are verified by their content alone, so any derivation is free to
         try to produce such a path.

         Input-addressed derivation output paths, however, are calculated
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
      if (!(drvType.isCA() || trusted))
        throw Error("you are not privileged to build input-addressed derivations");

      /* Make sure that the non-input-addressed derivations that got this far
         are in fact content-addressed if we don't trust them. */
      assert(drvType.isCA() || trusted);

      /* Recompute the derivation path when we cannot trust the original. */
      if (!trusted) {
        /* Recomputing the derivation path for input-address derivations
           makes it harder to audit them after the fact, since we need the
           original not-necessarily-resolved derivation to verify the drv
           derivation as adequate claim to the input-addressed output
           paths. */
        assert(drvType.isCA());

        Derivation drv2;
        static_cast<BasicDerivation&>(drv2) = drv;
        drvPath = writeDerivation(*store, Derivation{drv2});
      }

      auto res = store->buildDerivation(drvPath, drv, buildMode);
      logger->stopWork();
      WorkerProto::write(*store, wconn, res);
      break;
    }

    case WorkerProto::Op::EnsurePath: {
      auto path = WorkerProto::Serialise<StorePath>::read(*store, rconn);
      logger->startWork();
      store->ensurePath(path);
      logger->stopWork();
      conn.to << 1;
      break;
    }

    case WorkerProto::Op::AddTempRoot: {
      auto path = WorkerProto::Serialise<StorePath>::read(*store, rconn);
      logger->startWork();
      store->addTempRoot(path);
      logger->stopWork();
      conn.to << 1;
      break;
    }

    case WorkerProto::Op::AddPermRoot: {
      if (!trusted)
        throw Error("you are not privileged to create perm roots\n\n"
                    "hint: you can just do this client-side without special privileges, and "
                    "probably want to do that instead.");
      auto storePath = WorkerProto::Serialise<StorePath>::read(*store, rconn);
      Path gcRoot = absPath(readString(conn.from));
      logger->startWork();
      auto& localFSStore = require<LocalFSStore>(*store);
      localFSStore.addPermRoot(storePath, gcRoot);
      logger->stopWork();
      conn.to << gcRoot;
      break;
    }

    case WorkerProto::Op::AddIndirectRoot: {
      Path path = absPath(readString(conn.from));

      logger->startWork();
      auto& indirectRootStore = require<IndirectRootStore>(*store);
      indirectRootStore.addIndirectRoot(path);
      logger->stopWork();

      conn.to << 1;
      break;
    }

    // Obsolete.
    case WorkerProto::Op::SyncWithGC: {
      logger->startWork();
      logger->stopWork();
      conn.to << 1;
      break;
    }

    case WorkerProto::Op::FindRoots: {
      logger->startWork();
      auto& gcStore = require<GcStore>(*store);
      Roots roots = gcStore.findRoots(!trusted);
      logger->stopWork();

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
      options.pathsToDelete = WorkerProto::Serialise<StorePathSet>::read(*store, rconn);
      conn.from >> options.ignoreLiveness >> options.maxFreed;
      options.censor = !trusted;
      // obsolete fields
      readInt(conn.from);
      readInt(conn.from);
      readInt(conn.from);

      GCResults results;

      logger->startWork();
      if (options.ignoreLiveness && !getEnv("_NIX_IN_TEST").has_value())
        throw Error("you are not allowed to ignore liveness");
      auto& gcStore = require<GcStore>(*store);
      gcStore.collectGarbage(options, results);
      logger->stopWork();

      conn.to << results.paths << results.bytesFreed << 0 /* obsolete */;

      break;
    }

    case WorkerProto::Op::SetOptions: {
      client_settings_t clientSettings;

      clientSettings.keepFailed = readInt(conn.from);
      clientSettings.keepGoing = readInt(conn.from);
      clientSettings.tryFallback = readInt(conn.from);
      clientSettings.verbosity = (verbosity_t)readInt(conn.from);
      clientSettings.maxBuildJobs = readInt(conn.from);
      clientSettings.maxSilentTime = readInt(conn.from);
      readInt(conn.from); // obsolete useBuildHook
      clientSettings.verboseBuild = lvlError == (verbosity_t)readInt(conn.from);
      readInt(conn.from); // obsolete logType
      readInt(conn.from); // obsolete printBuildTrace
      clientSettings.buildCores = readInt(conn.from);
      clientSettings.useSubstitutes = readInt(conn.from);

      unsigned int n = readInt(conn.from);
      for (unsigned int i = 0; i < n; i++) {
        auto name = readString(conn.from);
        auto value = readString(conn.from);
        clientSettings.overrides.emplace(name, value);
      }

      logger->startWork();

      // FIXME: use some setting in recursive mode. Will need to use
      // non-global variables.
      if (!recursive)
        clientSettings.apply(trusted);

      logger->stopWork();
      break;
    }

    case WorkerProto::Op::QuerySubstitutablePathInfo: {
      auto path = WorkerProto::Serialise<StorePath>::read(*store, rconn);
      logger->startWork();
      SubstitutablePathInfos infos;
      store->querySubstitutablePathInfos({{path, std::nullopt}}, infos);
      logger->stopWork();
      auto i = infos.find(path);
      if (i == infos.end())
        conn.to << 0;
      else {
        conn.to << 1;
        WorkerProto::write(*store, wconn, i->second.deriver);
        WorkerProto::write(*store, wconn, i->second.references);
        conn.to << i->second.downloadSize << i->second.narSize;
      }
      break;
    }

    case WorkerProto::Op::QuerySubstitutablePathInfos: {
      SubstitutablePathInfos infos;
      StorePathCAMap pathsMap = {};
      if (GET_PROTOCOL_MINOR(conn.protoVersion) < 22) {
        auto paths = WorkerProto::Serialise<StorePathSet>::read(*store, rconn);
        for (auto& path : paths)
          pathsMap.emplace(path, std::nullopt);
      } else
        pathsMap = WorkerProto::Serialise<StorePathCAMap>::read(*store, rconn);
      logger->startWork();
      store->querySubstitutablePathInfos(pathsMap, infos);
      logger->stopWork();
      conn.to << infos.size();
      for (auto& i : infos) {
        WorkerProto::write(*store, wconn, i.first);
        WorkerProto::write(*store, wconn, i.second.deriver);
        WorkerProto::write(*store, wconn, i.second.references);
        conn.to << i.second.downloadSize << i.second.narSize;
      }
      break;
    }

    case WorkerProto::Op::QueryAllValidPaths: {
      logger->startWork();
      auto paths = store->queryAllValidPaths();
      logger->stopWork();
      WorkerProto::write(*store, wconn, paths);
      break;
    }

    case WorkerProto::Op::QueryPathInfo: {
      auto path = WorkerProto::Serialise<StorePath>::read(*store, rconn);
      std::shared_ptr<const ValidPathInfo> info;
      logger->startWork();
      info = store->queryPathInfo(path);
      logger->stopWork();
      if (info) {
        conn.to << 1;
        WorkerProto::write(*store, wconn, static_cast<const UnkeyedValidPathInfo&>(*info));
      } else {
        conn.to << 0;
      }
      break;
    }

    case WorkerProto::Op::OptimiseStore:
      logger->startWork();
      store->optimiseStore();
      logger->stopWork();
      conn.to << 1;
      break;

    case WorkerProto::Op::VerifyStore: {
      bool checkContents, repair;
      conn.from >> checkContents >> repair;
      logger->startWork();
      if (repair && !trusted)
        throw Error("you are not privileged to repair paths");
      bool errors = store->verifyStore(checkContents, (RepairFlag)repair);
      logger->stopWork();
      conn.to << errors;
      break;
    }

    case WorkerProto::Op::AddSignatures: {
      auto path = WorkerProto::Serialise<StorePath>::read(*store, rconn);
      string_set_t sigs = readStrings<string_set_t>(conn.from);
      logger->startWork();
      store->addSignatures(path, sigs);
      logger->stopWork();
      conn.to << 1;
      break;
    }

    case WorkerProto::Op::NarFromPath: {
      auto path = WorkerProto::Serialise<StorePath>::read(*store, rconn);
      logger->startWork();
      logger->stopWork();
      store->narFromPath(path, conn.to);
      break;
    }

    case WorkerProto::Op::AddToStoreNar: {
      bool repair, dontCheckSigs;
      auto path = WorkerProto::Serialise<StorePath>::read(*store, rconn);
      auto deriver = WorkerProto::Serialise<std::optional<StorePath>>::read(*store, rconn);
      auto narHash = Hash::parseAny(readString(conn.from), hash_algorithm_t::SHA256);
      ValidPathInfo info{path, {*store, narHash}};
      info.deriver = std::move(deriver);
      info.references = WorkerProto::Serialise<StorePathSet>::read(*store, rconn);
      conn.from >> info.registrationTime >> info.narSize >> info.ultimate;
      info.sigs = readStrings<string_set_t>(conn.from);
      info.ca = ContentAddress::parseOpt(readString(conn.from));
      conn.from >> repair >> dontCheckSigs;
      if (!trusted && dontCheckSigs)
        dontCheckSigs = false;
      if (!trusted)
        info.ultimate = false;

      if (GET_PROTOCOL_MINOR(conn.protoVersion) >= 23) {
        logger->startWork();
        {
          framed_source_t source(conn.from);
          store->addToStore(info, source, (RepairFlag)repair,
                            dontCheckSigs ? NoCheckSigs : CheckSigs);
        }
        logger->stopWork();
      }

      else {
        std::unique_ptr<Source> source;
        string_sink_t saved;
        if (GET_PROTOCOL_MINOR(conn.protoVersion) >= 21)
          source = std::make_unique<tunnel_source_t>(conn.from, conn.to);
        else {
          tee_source_t tee{conn.from, saved};
          null_file_system_object_sink_t ether;
          parseDump(ether, tee);
          source = std::make_unique<string_source_t>(saved.s);
        }

        logger->startWork();

        // FIXME: race if addToStore doesn't read source?
        store->addToStore(info, *source, (RepairFlag)repair,
                          dontCheckSigs ? NoCheckSigs : CheckSigs);

        logger->stopWork();
      }

      break;
    }

    case WorkerProto::Op::QueryMissing: {
      auto targets = WorkerProto::Serialise<DerivedPaths>::read(*store, rconn);
      logger->startWork();
      auto missing = store->queryMissing(targets);
      logger->stopWork();
      WorkerProto::write(*store, wconn, missing.willBuild);
      WorkerProto::write(*store, wconn, missing.willSubstitute);
      WorkerProto::write(*store, wconn, missing.unknown);
      conn.to << missing.downloadSize << missing.narSize;
      break;
    }

    case WorkerProto::Op::RegisterDrvOutput: {
      logger->startWork();
      if (GET_PROTOCOL_MINOR(conn.protoVersion) < 31) {
        auto outputId = WorkerProto::Serialise<DrvOutput>::read(*store, rconn);
        auto outputPath = StorePath(readString(conn.from));
        store->registerDrvOutput(Realisation{{.outPath = outputPath}, outputId});
      } else {
        auto realisation = WorkerProto::Serialise<Realisation>::read(*store, rconn);
        store->registerDrvOutput(realisation);
      }
      logger->stopWork();
      break;
    }

    case WorkerProto::Op::QueryRealisation: {
      logger->startWork();
      auto outputId = WorkerProto::Serialise<DrvOutput>::read(*store, rconn);
      auto info = store->queryRealisation(outputId);
      logger->stopWork();
      if (GET_PROTOCOL_MINOR(conn.protoVersion) < 31) {
        std::set<StorePath> outPaths;
        if (info)
          outPaths.insert(info->outPath);
        WorkerProto::write(*store, wconn, outPaths);
      } else {
        std::set<Realisation> realisations;
        if (info)
          realisations.insert({*info, outputId});
        WorkerProto::write(*store, wconn, realisations);
      }
      break;
    }

    case WorkerProto::Op::AddBuildLog: {
      StorePath path{readString(conn.from)};
      logger->startWork();
      if (!trusted)
        throw Error("you are not privileged to add logs");
      auto& logStore = require<LogStore>(*store);
      {
        framed_source_t source(conn.from);
        string_sink_t sink;
        source.drainInto(sink);
        logStore.addBuildLog(path, sink.s);
      }
      logger->stopWork();
      conn.to << 1;
      break;
    }

    case WorkerProto::Op::QueryFailedPaths:
    case WorkerProto::Op::ClearFailedPaths:
      throw Error("Removed operation %1%", op);

    case WorkerProto::Op::QueryActiveBuilds: {
      logger->startWork();
      auto& activeBuildsStore = require<QueryActiveBuildsStore>(*store);
      auto activeBuilds = activeBuildsStore.queryActiveBuilds();
      logger->stopWork();
      conn.to << nlohmann::json(activeBuilds).dump();
      break;
    }

    default:
      throw Error("invalid operation %1%", op);
  }
}

void processConnection(ref<Store> store, fd_source_t&& from, fd_sink_t&& to, TrustedFlag trusted,
                       RecursiveFlag recursive) {
#ifndef _WIN32 // TODO need graceful async exit support on Windows?
  auto monitor = !recursive ? std::make_unique<MonitorFdHup>(from.fd) : nullptr;
  (void)monitor; // suppress warning
  receive_interrupts_t receiveInterrupts;
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

  auto tunnelLogger_ = std::make_unique<tunnel_logger_t>(conn.to, protoVersion);
  auto tunnelLogger = tunnelLogger_.get();
  std::unique_ptr<Logger> prevLogger_;
  auto prevLogger = logger.get();
  // FIXME
  if (!recursive) {
    prevLogger_ = std::move(logger);
    logger = std::move(tunnelLogger_);
    applyJSONLogger();
  }

  unsigned int opCount = 0;

  finally_t finally([&]() {
    setInterrupted(false);
    printMsgUsing(prevLogger, lvlDebug, "%d operations", opCount);
  });

  conn.postHandshake(
      *store, {
                  .daemonNixVersion = nixVersion,
                  // We and the underlying store both need to trust the client for
                  // it to be trusted.
                  .remoteTrustsUs = trusted ? store->isTrustedClient() : std::optional{NotTrusted},
              });

  /* Send startup error messages to the client. */
  tunnelLogger->startWork();

  try {
    tunnelLogger->stopWork();
    conn.to.flush();

    /* Process client requests. */
    while (true) {
      WorkerProto::Op op;
      try {
        op = (enum WorkerProto::Op)readInt(conn.from);
      } catch (Interrupted& e) {
        break;
      } catch (EndOfFile& e) {
        break;
      }

      printMsgUsing(prevLogger, lvlDebug, "received daemon op %d", op);

      opCount++;

      debug("performing daemon worker op: %d", op);

      try {
        performOp(tunnelLogger, store, trusted, recursive, conn, op);
      } catch (Error& e) {
        /* If we're not in a state where we can send replies, then
           something went wrong processing the input of the
           client.  This can happen especially if I/O errors occur
           during addTextToStore() / importPath().  If that
           happens, just send the error message and exit. */
        bool errorAllowed = tunnelLogger->state_.lock()->canSendStderr;
        tunnelLogger->stopWork(&e);
        if (!errorAllowed)
          throw;
      } catch (std::bad_alloc& e) {
        auto ex = Error("Nix daemon out of memory");
        tunnelLogger->stopWork(&ex);
        throw;
      }

      conn.to.flush();

      assert(!tunnelLogger->state_.lock()->canSendStderr);
    };

  } catch (Error& e) {
    tunnelLogger->stopWork(&e);
    conn.to.flush();
    return;
  } catch (std::exception& e) {
    auto ex = Error(e.what());
    tunnelLogger->stopWork(&ex);
    conn.to.flush();
    return;
  }
}

} // namespace nix::daemon
