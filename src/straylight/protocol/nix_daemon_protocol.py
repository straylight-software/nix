# This is a generated file! Please edit source .ksy file and use kaitai-struct-compiler to rebuild

import kaitaistruct
from kaitaistruct import KaitaiStruct, KaitaiStream, BytesIO
from enum import Enum


if getattr(kaitaistruct, 'API_VERSION', (0, 9)) < (0, 9):
    raise Exception("Incompatible Kaitai Struct Python API: 0.9 or later is required, but you have %s" % (kaitaistruct.__version__))

class NixDaemonProtocol(KaitaiStruct):
    """The Nix daemon protocol ("worker protocol") is used for communication
    between nix CLI tools and nix-daemon over unix sockets or SSH.
    
    Wire format primitives:
    - Integers: little-endian u64
    - Strings: u64 length + bytes + padding to 8-byte boundary
    - Booleans: u64 (0 = false, nonzero = true)
    - Lists: u64 count + elements
    - Sets: same as lists (sorted)
    - Optional<T>: empty string = None for string-like, tag byte for others
    
    Protocol version: 1.38 (current), minimum 1.10
    """

    class GcAction(Enum):
        return_live = 0
        return_dead = 1
        delete_dead = 2
        delete_specific = 3

    class LoggerFieldType(Enum):
        int = 0
        string = 1

    class BuildStatus(Enum):
        built = 0
        substituted = 1
        already_valid = 2
        permanent_failure = 3
        input_rejected = 4
        output_rejected = 5
        transient_failure = 6
        cached_failure = 7
        timed_out = 8
        misc_failure = 9
        dependency_failed = 10
        log_limit_exceeded = 11
        not_deterministic = 12
        resolved_drv_failed = 13
        no_substituters = 14

    class Operation(Enum):
        is_valid_path = 1
        has_substitutes = 3
        query_path_hash = 4
        query_references = 5
        query_referrers = 6
        add_to_store = 7
        add_text_to_store = 8
        build_paths = 9
        ensure_path = 10
        add_temp_root = 11
        add_indirect_root = 12
        sync_with_gc = 13
        find_roots = 14
        query_deriver = 18
        set_options = 19
        collect_garbage = 20
        query_substitutable_path_info = 21
        query_derivation_outputs = 22
        query_all_valid_paths = 23
        query_failed_paths = 24
        clear_failed_paths = 25
        query_path_info = 26
        query_derivation_output_names = 28
        query_path_from_hash_part = 29
        query_substitutable_path_infos = 30
        query_valid_paths = 31
        query_substitutable_paths = 32
        query_valid_derivers = 33
        optimise_store = 34
        verify_store = 35
        build_derivation = 36
        add_signatures = 37
        nar_from_path = 38
        add_to_store_nar = 39
        query_missing = 40
        query_derivation_output_map = 41
        register_drv_output = 42
        query_realisation = 43
        add_multiple_to_store = 44
        add_build_log = 45
        build_paths_with_results = 46
        add_perm_root = 47
        query_active_builds = 48

    class TrustLevel(Enum):
        unknown = 0
        trusted = 1
        not_trusted = 2

    class BuildMode(Enum):
        normal = 0
        repair = 1
        check = 2
    def __init__(self, protocol_version, _io, _parent=None, _root=None):
        self._io = _io
        self._parent = _parent
        self._root = _root if _root else self
        self.protocol_version = protocol_version
        self._read()

    def _read(self):
        pass

    class ValidPathInfo(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.path = NixDaemonProtocol.StorePath(self._io, self, self._root)
            self.info = NixDaemonProtocol.UnkeyedValidPathInfo(self._io, self, self._root)


    class StderrStopActivity(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.act_id = self._io.read_u8le()


    class AddToStoreRequest(KaitaiStruct):
        """Add content to store. Protocol >= 1.25 uses new format.
        NAR data follows via framed source after request.
        """
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            if self._root.protocol_version >= 25:
                self.name = NixDaemonProtocol.NixString(self._io, self, self._root)

            if self._root.protocol_version >= 25:
                self.ca_method = NixDaemonProtocol.NixString(self._io, self, self._root)

            if self._root.protocol_version >= 25:
                self.refs = NixDaemonProtocol.StorePathSet(self._io, self, self._root)

            if self._root.protocol_version >= 25:
                self.repair = self._io.read_u8le()

            if self._root.protocol_version < 25:
                self.base_name = NixDaemonProtocol.NixString(self._io, self, self._root)

            if self._root.protocol_version < 25:
                self.fixed = self._io.read_u8le()

            if self._root.protocol_version < 25:
                self.recursive = self._io.read_u1()

            if self._root.protocol_version < 25:
                self.hash_algo = NixDaemonProtocol.NixString(self._io, self, self._root)



    class DerivationOutputEntry(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.name = NixDaemonProtocol.NixString(self._io, self, self._root)
            self.path = NixDaemonProtocol.OptionalStorePath(self._io, self, self._root)


    class QueryValidPathsRequest(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.paths = NixDaemonProtocol.StorePathSet(self._io, self, self._root)
            if self._root.protocol_version >= 27:
                self.substitute = self._io.read_u8le()



    class Realisation(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.id = NixDaemonProtocol.DrvOutput(self._io, self, self._root)
            self.out_path = NixDaemonProtocol.StorePath(self._io, self, self._root)
            self.signatures = NixDaemonProtocol.NixStringSet(self._io, self, self._root)
            self.dependent_realisations = NixDaemonProtocol.DrvOutputs(self._io, self, self._root)


    class QueryMissingResponse(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.will_build = NixDaemonProtocol.StorePathSet(self._io, self, self._root)
            self.will_substitute = NixDaemonProtocol.StorePathSet(self._io, self, self._root)
            self.unknown = NixDaemonProtocol.StorePathSet(self._io, self, self._root)
            self.download_size = self._io.read_u8le()
            self.nar_size = self._io.read_u8le()


    class Request(KaitaiStruct):
        """Client request to daemon."""
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.op = KaitaiStream.resolve_enum(NixDaemonProtocol.Operation, self._io.read_u8le())
            _on = self.op
            if _on == NixDaemonProtocol.Operation.query_derivation_output_map:
                self.payload = NixDaemonProtocol.QueryDerivationOutputMapRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.nar_from_path:
                self.payload = NixDaemonProtocol.NarFromPathRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.query_path_from_hash_part:
                self.payload = NixDaemonProtocol.QueryPathFromHashPartRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.find_roots:
                self.payload = NixDaemonProtocol.EmptyRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.query_referrers:
                self.payload = NixDaemonProtocol.QueryReferrersRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.add_to_store_nar:
                self.payload = NixDaemonProtocol.AddToStoreNarRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.query_valid_derivers:
                self.payload = NixDaemonProtocol.QueryValidDeriversRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.build_paths:
                self.payload = NixDaemonProtocol.BuildPathsRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.query_derivation_output_names:
                self.payload = NixDaemonProtocol.QueryDerivationOutputNamesRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.ensure_path:
                self.payload = NixDaemonProtocol.EnsurePathRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.add_signatures:
                self.payload = NixDaemonProtocol.AddSignaturesRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.query_substitutable_path_infos:
                self.payload = NixDaemonProtocol.QuerySubstitutablePathInfosRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.add_text_to_store:
                self.payload = NixDaemonProtocol.AddTextToStoreRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.query_substitutable_path_info:
                self.payload = NixDaemonProtocol.QuerySubstitutablePathInfoRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.query_missing:
                self.payload = NixDaemonProtocol.QueryMissingRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.is_valid_path:
                self.payload = NixDaemonProtocol.IsValidPathRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.query_path_hash:
                self.payload = NixDaemonProtocol.QueryPathHashRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.sync_with_gc:
                self.payload = NixDaemonProtocol.EmptyRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.add_perm_root:
                self.payload = NixDaemonProtocol.AddPermRootRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.add_multiple_to_store:
                self.payload = NixDaemonProtocol.AddMultipleToStoreRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.has_substitutes:
                self.payload = NixDaemonProtocol.HasSubstitutesRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.optimise_store:
                self.payload = NixDaemonProtocol.EmptyRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.add_indirect_root:
                self.payload = NixDaemonProtocol.AddIndirectRootRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.query_path_info:
                self.payload = NixDaemonProtocol.QueryPathInfoRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.query_active_builds:
                self.payload = NixDaemonProtocol.EmptyRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.query_substitutable_paths:
                self.payload = NixDaemonProtocol.QuerySubstitutablePathsRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.register_drv_output:
                self.payload = NixDaemonProtocol.RegisterDrvOutputRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.add_temp_root:
                self.payload = NixDaemonProtocol.AddTempRootRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.build_paths_with_results:
                self.payload = NixDaemonProtocol.BuildPathsWithResultsRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.query_realisation:
                self.payload = NixDaemonProtocol.QueryRealisationRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.build_derivation:
                self.payload = NixDaemonProtocol.BuildDerivationRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.query_derivation_outputs:
                self.payload = NixDaemonProtocol.QueryDerivationOutputsRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.collect_garbage:
                self.payload = NixDaemonProtocol.CollectGarbageRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.query_references:
                self.payload = NixDaemonProtocol.QueryReferencesRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.verify_store:
                self.payload = NixDaemonProtocol.VerifyStoreRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.add_to_store:
                self.payload = NixDaemonProtocol.AddToStoreRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.query_deriver:
                self.payload = NixDaemonProtocol.QueryDeriverRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.add_build_log:
                self.payload = NixDaemonProtocol.AddBuildLogRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.set_options:
                self.payload = NixDaemonProtocol.SetOptionsRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.query_valid_paths:
                self.payload = NixDaemonProtocol.QueryValidPathsRequest(self._io, self, self._root)
            elif _on == NixDaemonProtocol.Operation.query_all_valid_paths:
                self.payload = NixDaemonProtocol.EmptyRequest(self._io, self, self._root)


    class DrvOutput(KaitaiStruct):
        """Derivation output identifier (drv path + output name)."""
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.drv_path = NixDaemonProtocol.NixString(self._io, self, self._root)
            self.output_name = NixDaemonProtocol.NixString(self._io, self, self._root)


    class AddBuildLogRequest(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.drv_path = NixDaemonProtocol.NixString(self._io, self, self._root)


    class AddTextToStoreRequest(KaitaiStruct):
        """Obsolete since 1.25."""
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.suffix = NixDaemonProtocol.NixString(self._io, self, self._root)
            self.text = NixDaemonProtocol.NixString(self._io, self, self._root)
            self.refs = NixDaemonProtocol.StorePathSet(self._io, self, self._root)


    class FindRootsResponse(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.num_roots = self._io.read_u8le()
            self.roots = []
            for i in range(self.num_roots):
                self.roots.append(NixDaemonProtocol.RootEntry(self._io, self, self._root))



    class CollectGarbageResponse(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.paths = NixDaemonProtocol.NixStringSet(self._io, self, self._root)
            self.bytes_freed = self._io.read_u8le()
            self.obsolete = self._io.read_u8le()


    class AddTempRootRequest(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.path = NixDaemonProtocol.StorePath(self._io, self, self._root)


    class SettingOverrides(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.num_settings = self._io.read_u8le()
            self.settings = []
            for i in range(self.num_settings):
                self.settings.append(NixDaemonProtocol.SettingPair(self._io, self, self._root))



    class QuerySubstitutablePathInfosResponse(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.num_infos = self._io.read_u8le()
            self.infos = []
            for i in range(self.num_infos):
                self.infos.append(NixDaemonProtocol.SubstitutablePathInfoEntry(self._io, self, self._root))



    class StderrResult(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.act_id = self._io.read_u8le()
            self.result_type = self._io.read_u8le()
            self.fields = NixDaemonProtocol.LoggerFields(self._io, self, self._root)


    class SettingPair(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.name = NixDaemonProtocol.NixString(self._io, self, self._root)
            self.value = NixDaemonProtocol.NixString(self._io, self, self._root)


    class IsValidPathResponse(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.valid = self._io.read_u8le()


    class LoggerFields(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.num_fields = self._io.read_u8le()
            self.fields = []
            for i in range(self.num_fields):
                self.fields.append(NixDaemonProtocol.LoggerField(self._io, self, self._root))



    class OptionalDuration(KaitaiStruct):
        """Optional microseconds duration."""
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.tag = self._io.read_u1()
            if self.tag == 1:
                self.microseconds = self._io.read_s8le()



    class HasSubstitutesRequest(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.path = NixDaemonProtocol.StorePath(self._io, self, self._root)


    class NarFromPathRequest(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.path = NixDaemonProtocol.StorePath(self._io, self, self._root)


    class NixStringSet(KaitaiStruct):
        """Sorted set of strings (same wire format as list)."""
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.num_items = self._io.read_u8le()
            self.items = []
            for i in range(self.num_items):
                self.items.append(NixDaemonProtocol.NixString(self._io, self, self._root))



    class ServerHandshakeInfo(KaitaiStruct):
        """Server info sent after version negotiation."""
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            if self._root.protocol_version >= 33:
                self.daemon_version = NixDaemonProtocol.NixString(self._io, self, self._root)

            if self._root.protocol_version >= 35:
                self.trust_level = KaitaiStream.resolve_enum(NixDaemonProtocol.TrustLevel, self._io.read_u8le())



    class AddSignaturesRequest(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.path = NixDaemonProtocol.StorePath(self._io, self, self._root)
            self.sigs = NixDaemonProtocol.NixStringSet(self._io, self, self._root)


    class NixError(KaitaiStruct):
        """Structured error (protocol >= 1.26)."""
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.type = NixDaemonProtocol.NixString(self._io, self, self._root)
            self.level = self._io.read_u8le()
            self.name = NixDaemonProtocol.NixString(self._io, self, self._root)
            self.msg = NixDaemonProtocol.NixString(self._io, self, self._root)
            self.have_pos = self._io.read_u8le()
            self.traces = NixDaemonProtocol.ErrorTraceList(self._io, self, self._root)


    class QueryReferencesRequest(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.path = NixDaemonProtocol.StorePath(self._io, self, self._root)


    class IsValidPathRequest(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.path = NixDaemonProtocol.StorePath(self._io, self, self._root)


    class StorePath(KaitaiStruct):
        """Store path, e.g. "/nix/store/abc123-hello-1.0"
        """
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.path = NixDaemonProtocol.NixString(self._io, self, self._root)


    class QuerySubstitutablePathsRequest(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.paths = NixDaemonProtocol.StorePathSet(self._io, self, self._root)


    class ClientHandshakeContinuation(KaitaiStruct):
        """After version exchange, client sends additional handshake info.
        Fields depend on negotiated version.
        """
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            if self._root.protocol_version >= 14:
                self.cpu_affinity_tag = self._io.read_u8le()

            if  ((self._root.protocol_version >= 14) and (self.cpu_affinity_tag != 0)) :
                self.cpu_affinity = self._io.read_u8le()

            if self._root.protocol_version >= 11:
                self.reserve_space = self._io.read_u8le()



    class StderrMessage(KaitaiStruct):
        """During request processing, daemon sends stderr messages.
        Client must handle these until receiving STDERR_LAST.
        """
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.msg_type = self._io.read_u8le()
            _on = self.msg_type
            if _on == 1634497651:
                self.payload = NixDaemonProtocol.StderrLast(self._io, self, self._root)
            elif _on == 1381190740:
                self.payload = NixDaemonProtocol.StderrResult(self._io, self, self._root)
            elif _on == 1684108310:
                self.payload = NixDaemonProtocol.StderrWrite(self._io, self, self._root)
            elif _on == 1684108385:
                self.payload = NixDaemonProtocol.StderrRead(self._io, self, self._root)
            elif _on == 1668838512:
                self.payload = NixDaemonProtocol.StderrError(self._io, self, self._root)
            elif _on == 1869376871:
                self.payload = NixDaemonProtocol.StderrNext(self._io, self, self._root)
            elif _on == 1398034256:
                self.payload = NixDaemonProtocol.StderrStopActivity(self._io, self, self._root)
            elif _on == 1398035028:
                self.payload = NixDaemonProtocol.StderrStartActivity(self._io, self, self._root)


    class StderrStartActivity(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.act_id = self._io.read_u8le()
            self.verbosity = self._io.read_u8le()
            self.activity_type = self._io.read_u8le()
            self.msg = NixDaemonProtocol.NixString(self._io, self, self._root)
            self.fields = NixDaemonProtocol.LoggerFields(self._io, self, self._root)
            self.parent = self._io.read_u8le()


    class StorePathSet(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.num_paths = self._io.read_u8le()
            self.paths = []
            for i in range(self.num_paths):
                self.paths.append(NixDaemonProtocol.StorePath(self._io, self, self._root))



    class StorePathCaEntry(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.path = NixDaemonProtocol.StorePath(self._io, self, self._root)
            self.ca = NixDaemonProtocol.NixString(self._io, self, self._root)


    class NixString(KaitaiStruct):
        """Length-prefixed string, padded to 8-byte boundary."""
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.len = self._io.read_u8le()
            self.data = (self._io.read_bytes(self.len)).decode(u"UTF-8")
            self.padding = self._io.read_bytes(((8 - (self.len % 8)) % 8))


    class BuildPathsWithResultsRequest(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.paths = NixDaemonProtocol.DerivedPathList(self._io, self, self._root)
            self.build_mode = KaitaiStream.resolve_enum(NixDaemonProtocol.BuildMode, self._io.read_u1())


    class ErrorTrace(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.have_pos = self._io.read_u8le()
            self.msg = NixDaemonProtocol.NixString(self._io, self, self._root)


    class SetOptionsRequest(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.keep_failed = self._io.read_u8le()
            self.keep_going = self._io.read_u8le()
            self.try_fallback = self._io.read_u8le()
            self.verbosity = self._io.read_u8le()
            self.max_build_jobs = self._io.read_u8le()
            self.max_silent_time = self._io.read_u8le()
            self.obsolete_use_build_hook = self._io.read_u8le()
            self.verbose_build = self._io.read_u8le()
            self.obsolete_log_type = self._io.read_u8le()
            self.obsolete_print_build_trace = self._io.read_u8le()
            self.build_cores = self._io.read_u8le()
            self.use_substitutes = self._io.read_u8le()
            if self._root.protocol_version >= 12:
                self.overrides = NixDaemonProtocol.SettingOverrides(self._io, self, self._root)



    class QueryRealisationResponse(KaitaiStruct):
        """Protocol >= 1.31."""
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.num_realisations = self._io.read_u8le()
            self.realisations = []
            for i in range(self.num_realisations):
                self.realisations.append(NixDaemonProtocol.Realisation(self._io, self, self._root))



    class QuerySubstitutablePathInfoRequest(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.path = NixDaemonProtocol.StorePath(self._io, self, self._root)


    class QueryDerivationOutputMapRequest(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.path = NixDaemonProtocol.StorePath(self._io, self, self._root)


    class BuildDerivationRequest(KaitaiStruct):
        """Build a derivation. The derivation is sent inline (not read from store).
        Derivation format is ATerm-based, parsed separately.
        """
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.drv_path = NixDaemonProtocol.StorePath(self._io, self, self._root)
            self.derivation = NixDaemonProtocol.NixBytes(self._io, self, self._root)
            self.build_mode = KaitaiStream.resolve_enum(NixDaemonProtocol.BuildMode, self._io.read_u1())


    class StderrNext(KaitaiStruct):
        """Log message from daemon."""
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.msg = NixDaemonProtocol.NixString(self._io, self, self._root)


    class DerivationOutputMap(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.num_outputs = self._io.read_u8le()
            self.outputs = []
            for i in range(self.num_outputs):
                self.outputs.append(NixDaemonProtocol.DerivationOutputEntry(self._io, self, self._root))



    class EmptyRequest(KaitaiStruct):
        """Operations with no request payload."""
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            pass


    class AddPermRootRequest(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.store_path = NixDaemonProtocol.StorePath(self._io, self, self._root)
            self.gc_root = NixDaemonProtocol.NixString(self._io, self, self._root)


    class QuerySubstitutablePathInfoResponse(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.found = self._io.read_u8le()
            if self.found != 0:
                self.deriver = NixDaemonProtocol.OptionalStorePath(self._io, self, self._root)

            if self.found != 0:
                self.references = NixDaemonProtocol.StorePathSet(self._io, self, self._root)

            if self.found != 0:
                self.download_size = self._io.read_u8le()

            if self.found != 0:
                self.nar_size = self._io.read_u8le()



    class QueryPathFromHashPartRequest(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.hash_part = NixDaemonProtocol.NixString(self._io, self, self._root)


    class DerivedPathList(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.num_paths = self._io.read_u8le()
            self.paths = []
            for i in range(self.num_paths):
                self.paths.append(NixDaemonProtocol.DerivedPath(self._io, self, self._root))



    class DerivedPath(KaitaiStruct):
        """String representation of a derived path.
        Format: "/nix/store/...-foo" or "/nix/store/...-foo.drv^out,dev"
        """
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.path = NixDaemonProtocol.NixString(self._io, self, self._root)


    class QueryMissingRequest(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.targets = NixDaemonProtocol.DerivedPathList(self._io, self, self._root)


    class QueryValidDeriversRequest(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.path = NixDaemonProtocol.StorePath(self._io, self, self._root)


    class UnkeyedValidPathInfo(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.deriver = NixDaemonProtocol.OptionalStorePath(self._io, self, self._root)
            self.nar_hash = NixDaemonProtocol.NixString(self._io, self, self._root)
            self.references = NixDaemonProtocol.StorePathSet(self._io, self, self._root)
            self.registration_time = self._io.read_u8le()
            self.nar_size = self._io.read_u8le()
            if self._root.protocol_version >= 16:
                self.ultimate = self._io.read_u8le()

            if self._root.protocol_version >= 16:
                self.sigs = NixDaemonProtocol.NixStringSet(self._io, self, self._root)

            if self._root.protocol_version >= 16:
                self.ca = NixDaemonProtocol.NixString(self._io, self, self._root)



    class FeatureExchange(KaitaiStruct):
        """Protocol >= 1.38 feature negotiation."""
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.features = NixDaemonProtocol.NixStringSet(self._io, self, self._root)


    class NixStringList(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.num_items = self._io.read_u8le()
            self.items = []
            for i in range(self.num_items):
                self.items.append(NixDaemonProtocol.NixString(self._io, self, self._root))



    class ErrorTraceList(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.num_traces = self._io.read_u8le()
            self.traces = []
            for i in range(self.num_traces):
                self.traces.append(NixDaemonProtocol.ErrorTrace(self._io, self, self._root))



    class QueryReferrersRequest(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.path = NixDaemonProtocol.StorePath(self._io, self, self._root)


    class NixBytes(KaitaiStruct):
        """Length-prefixed bytes, padded to 8-byte boundary."""
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.len = self._io.read_u8le()
            self.data = self._io.read_bytes(self.len)
            self.padding = self._io.read_bytes(((8 - (self.len % 8)) % 8))


    class QueryDerivationOutputNamesRequest(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.path = NixDaemonProtocol.StorePath(self._io, self, self._root)


    class VerifyStoreRequest(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.check_contents = self._io.read_u8le()
            self.repair = self._io.read_u8le()


    class ServerHello(KaitaiStruct):
        """Response from daemon after receiving client_hello."""
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.magic = self._io.read_bytes(4)
            if not self.magic == b"\x6F\x69\x78\x64":
                raise kaitaistruct.ValidationNotEqualError(b"\x6F\x69\x78\x64", self.magic, self._io, u"/types/server_hello/seq/0")
            self.padding1 = self._io.read_bytes(4)
            if not self.padding1 == b"\x00\x00\x00\x00":
                raise kaitaistruct.ValidationNotEqualError(b"\x00\x00\x00\x00", self.padding1, self._io, u"/types/server_hello/seq/1")
            self.server_version = self._io.read_u8le()


    class QueryPathInfoResponse(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.valid = self._io.read_u8le()
            if self.valid != 0:
                self.info = NixDaemonProtocol.UnkeyedValidPathInfo(self._io, self, self._root)



    class ClientHello(KaitaiStruct):
        """Initial message from client to daemon."""
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.magic = self._io.read_bytes(4)
            if not self.magic == b"\x63\x78\x69\x6E":
                raise kaitaistruct.ValidationNotEqualError(b"\x63\x78\x69\x6E", self.magic, self._io, u"/types/client_hello/seq/0")
            self.padding1 = self._io.read_bytes(4)
            if not self.padding1 == b"\x00\x00\x00\x00":
                raise kaitaistruct.ValidationNotEqualError(b"\x00\x00\x00\x00", self.padding1, self._io, u"/types/client_hello/seq/1")
            self.client_version = self._io.read_u8le()


    class QueryRealisationRequest(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.output_id = NixDaemonProtocol.DrvOutput(self._io, self, self._root)


    class StderrError(KaitaiStruct):
        """Error during request processing."""
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            if self._root.protocol_version >= 26:
                self.error = NixDaemonProtocol.NixError(self._io, self, self._root)

            if self._root.protocol_version < 26:
                self.error_msg = NixDaemonProtocol.NixString(self._io, self, self._root)

            if self._root.protocol_version < 26:
                self.status = self._io.read_u8le()



    class BuildPathsWithResultsResponse(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.num_results = self._io.read_u8le()
            self.results = []
            for i in range(self.num_results):
                self.results.append(NixDaemonProtocol.KeyedBuildResult(self._io, self, self._root))



    class EnsurePathRequest(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.path = NixDaemonProtocol.StorePath(self._io, self, self._root)


    class RootEntry(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.link = NixDaemonProtocol.NixString(self._io, self, self._root)
            self.target = NixDaemonProtocol.StorePath(self._io, self, self._root)


    class LoggerField(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.field_type = KaitaiStream.resolve_enum(NixDaemonProtocol.LoggerFieldType, self._io.read_u8le())
            if self.field_type == NixDaemonProtocol.LoggerFieldType.int:
                self.int_value = self._io.read_u8le()

            if self.field_type == NixDaemonProtocol.LoggerFieldType.string:
                self.string_value = NixDaemonProtocol.NixString(self._io, self, self._root)



    class StderrWrite(KaitaiStruct):
        """Daemon sending data to client (for streaming downloads)."""
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.data = NixDaemonProtocol.NixString(self._io, self, self._root)


    class QueryDeriverRequest(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.path = NixDaemonProtocol.StorePath(self._io, self, self._root)


    class QueryDerivationOutputsRequest(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.path = NixDaemonProtocol.StorePath(self._io, self, self._root)


    class QueryRealisationResponseOld(KaitaiStruct):
        """Protocol < 1.31."""
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.out_paths = NixDaemonProtocol.StorePathSet(self._io, self, self._root)


    class CollectGarbageRequest(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.action = KaitaiStream.resolve_enum(NixDaemonProtocol.GcAction, self._io.read_u8le())
            self.paths_to_delete = NixDaemonProtocol.StorePathSet(self._io, self, self._root)
            self.ignore_liveness = self._io.read_u8le()
            self.max_freed = self._io.read_u8le()
            self.obsolete1 = self._io.read_u8le()
            self.obsolete2 = self._io.read_u8le()
            self.obsolete3 = self._io.read_u8le()


    class SubstitutablePathInfoEntry(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.path = NixDaemonProtocol.StorePath(self._io, self, self._root)
            self.deriver = NixDaemonProtocol.OptionalStorePath(self._io, self, self._root)
            self.references = NixDaemonProtocol.StorePathSet(self._io, self, self._root)
            self.download_size = self._io.read_u8le()
            self.nar_size = self._io.read_u8le()


    class QueryPathInfoRequest(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.path = NixDaemonProtocol.StorePath(self._io, self, self._root)


    class KeyedBuildResult(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.path = NixDaemonProtocol.DerivedPath(self._io, self, self._root)
            self.result = NixDaemonProtocol.BuildResult(self._io, self, self._root)


    class DrvOutputEntry(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.output_id = NixDaemonProtocol.DrvOutput(self._io, self, self._root)
            self.realisation = NixDaemonProtocol.Realisation(self._io, self, self._root)


    class OptionalStorePath(KaitaiStruct):
        """Empty string means None."""
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.path = NixDaemonProtocol.NixString(self._io, self, self._root)

        @property
        def is_present(self):
            if hasattr(self, '_m_is_present'):
                return self._m_is_present

            self._m_is_present = self.path.len > 0
            return getattr(self, '_m_is_present', None)


    class BuildResult(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.status = KaitaiStream.resolve_enum(NixDaemonProtocol.BuildStatus, self._io.read_u8le())
            self.error_msg = NixDaemonProtocol.NixString(self._io, self, self._root)
            if self._root.protocol_version >= 29:
                self.times_built = self._io.read_u8le()

            if self._root.protocol_version >= 29:
                self.is_non_deterministic = self._io.read_u8le()

            if self._root.protocol_version >= 29:
                self.start_time = self._io.read_u8le()

            if self._root.protocol_version >= 29:
                self.stop_time = self._io.read_u8le()

            if self._root.protocol_version >= 37:
                self.cpu_user = NixDaemonProtocol.OptionalDuration(self._io, self, self._root)

            if self._root.protocol_version >= 37:
                self.cpu_system = NixDaemonProtocol.OptionalDuration(self._io, self, self._root)

            if self._root.protocol_version >= 28:
                self.built_outputs = NixDaemonProtocol.DrvOutputs(self._io, self, self._root)



    class StderrRead(KaitaiStruct):
        """Daemon requesting data from client (for streaming uploads)."""
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.len = self._io.read_u8le()


    class QueryPathHashRequest(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.path = NixDaemonProtocol.StorePath(self._io, self, self._root)


    class BoolResponse(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.value = self._io.read_u8le()


    class BuildPathsRequest(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.paths = NixDaemonProtocol.DerivedPathList(self._io, self, self._root)
            self.build_mode = KaitaiStream.resolve_enum(NixDaemonProtocol.BuildMode, self._io.read_u1())


    class StderrLast(KaitaiStruct):
        """End of request processing (success)."""
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            pass


    class AddMultipleToStoreRequest(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.repair = self._io.read_u8le()
            self.dont_check_sigs = self._io.read_u8le()


    class StorePathCaMap(KaitaiStruct):
        """Map from StorePath to optional ContentAddress (protocol >= 1.22)."""
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.num_entries = self._io.read_u8le()
            self.entries = []
            for i in range(self.num_entries):
                self.entries.append(NixDaemonProtocol.StorePathCaEntry(self._io, self, self._root))



    class QuerySubstitutablePathInfosRequest(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            if self._root.protocol_version < 22:
                self.paths = NixDaemonProtocol.StorePathSet(self._io, self, self._root)

            if self._root.protocol_version >= 22:
                self.paths_with_ca = NixDaemonProtocol.StorePathCaMap(self._io, self, self._root)



    class AddToStoreNarRequest(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.path = NixDaemonProtocol.StorePath(self._io, self, self._root)
            self.deriver = NixDaemonProtocol.OptionalStorePath(self._io, self, self._root)
            self.nar_hash = NixDaemonProtocol.NixString(self._io, self, self._root)
            self.refs = NixDaemonProtocol.StorePathSet(self._io, self, self._root)
            self.registration_time = self._io.read_u8le()
            self.nar_size = self._io.read_u8le()
            self.ultimate = self._io.read_u8le()
            self.sigs = NixDaemonProtocol.NixStringSet(self._io, self, self._root)
            self.ca = NixDaemonProtocol.NixString(self._io, self, self._root)
            self.repair = self._io.read_u8le()
            self.dont_check_sigs = self._io.read_u8le()


    class RegisterDrvOutputRequest(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            if self._root.protocol_version < 31:
                self.output_id = NixDaemonProtocol.DrvOutput(self._io, self, self._root)

            if self._root.protocol_version < 31:
                self.output_path = NixDaemonProtocol.NixString(self._io, self, self._root)

            if self._root.protocol_version >= 31:
                self.realisation = NixDaemonProtocol.Realisation(self._io, self, self._root)



    class AddIndirectRootRequest(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.path = NixDaemonProtocol.NixString(self._io, self, self._root)


    class DrvOutputs(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            self._io = _io
            self._parent = _parent
            self._root = _root if _root else self
            self._read()

        def _read(self):
            self.num_outputs = self._io.read_u8le()
            self.outputs = []
            for i in range(self.num_outputs):
                self.outputs.append(NixDaemonProtocol.DrvOutputEntry(self._io, self, self._root))




