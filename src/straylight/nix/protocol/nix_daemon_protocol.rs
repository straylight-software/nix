// This is a generated file! Please edit source .ksy file and use kaitai-struct-compiler to rebuild

use std::option::Option;
use std::boxed::Box;
use std::io::Result;
use std::io::Cursor;
use std::vec::Vec;
use std::default::Default;
use kaitai_struct::KaitaiStream;
use kaitai_struct::KaitaiStruct;


/*
 * The Nix daemon protocol ("worker protocol") is used for communication
 * between nix CLI tools and nix-daemon over unix sockets or SSH.
 * 
 * Wire format primitives:
 * - Integers: little-endian u64
 * - Strings: u64 length + bytes + padding to 8-byte boundary
 * - Booleans: u64 (0 = false, nonzero = true)
 * - Lists: u64 count + elements
 * - Sets: same as lists (sorted)
 * - Optional<T>: empty string = None for string-like, tag byte for others
 * 
 * Protocol version: 1.38 (current), minimum 1.10
 */
#[derive(Default)]
pub struct NixDaemonProtocol {
}

impl KaitaiStruct for NixDaemonProtocol {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
    }
}

impl NixDaemonProtocol {
}
enum NixDaemonProtocol__GcAction {
    RETURN_LIVE,
    RETURN_DEAD,
    DELETE_DEAD,
    DELETE_SPECIFIC,
}
enum NixDaemonProtocol__LoggerFieldType {
    INT,
    STRING,
}
enum NixDaemonProtocol__BuildStatus {
    BUILT,
    SUBSTITUTED,
    ALREADY_VALID,
    PERMANENT_FAILURE,
    INPUT_REJECTED,
    OUTPUT_REJECTED,
    TRANSIENT_FAILURE,
    CACHED_FAILURE,
    TIMED_OUT,
    MISC_FAILURE,
    DEPENDENCY_FAILED,
    LOG_LIMIT_EXCEEDED,
    NOT_DETERMINISTIC,
    RESOLVED_DRV_FAILED,
    NO_SUBSTITUTERS,
}
enum NixDaemonProtocol__Operation {
    IS_VALID_PATH,
    HAS_SUBSTITUTES,
    QUERY_PATH_HASH,
    QUERY_REFERENCES,
    QUERY_REFERRERS,
    ADD_TO_STORE,
    ADD_TEXT_TO_STORE,
    BUILD_PATHS,
    ENSURE_PATH,
    ADD_TEMP_ROOT,
    ADD_INDIRECT_ROOT,
    SYNC_WITH_GC,
    FIND_ROOTS,
    QUERY_DERIVER,
    SET_OPTIONS,
    COLLECT_GARBAGE,
    QUERY_SUBSTITUTABLE_PATH_INFO,
    QUERY_DERIVATION_OUTPUTS,
    QUERY_ALL_VALID_PATHS,
    QUERY_FAILED_PATHS,
    CLEAR_FAILED_PATHS,
    QUERY_PATH_INFO,
    QUERY_DERIVATION_OUTPUT_NAMES,
    QUERY_PATH_FROM_HASH_PART,
    QUERY_SUBSTITUTABLE_PATH_INFOS,
    QUERY_VALID_PATHS,
    QUERY_SUBSTITUTABLE_PATHS,
    QUERY_VALID_DERIVERS,
    OPTIMISE_STORE,
    VERIFY_STORE,
    BUILD_DERIVATION,
    ADD_SIGNATURES,
    NAR_FROM_PATH,
    ADD_TO_STORE_NAR,
    QUERY_MISSING,
    QUERY_DERIVATION_OUTPUT_MAP,
    REGISTER_DRV_OUTPUT,
    QUERY_REALISATION,
    ADD_MULTIPLE_TO_STORE,
    ADD_BUILD_LOG,
    BUILD_PATHS_WITH_RESULTS,
    ADD_PERM_ROOT,
    QUERY_ACTIVE_BUILDS,
}
enum NixDaemonProtocol__TrustLevel {
    UNKNOWN,
    TRUSTED,
    NOT_TRUSTED,
}
enum NixDaemonProtocol__BuildMode {
    NORMAL,
    REPAIR,
    CHECK,
}
#[derive(Default)]
pub struct NixDaemonProtocol__ValidPathInfo {
    pub path: Box<NixDaemonProtocol__StorePath>,
    pub info: Box<NixDaemonProtocol__UnkeyedValidPathInfo>,
}

impl KaitaiStruct for NixDaemonProtocol__ValidPathInfo {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.path = Box::new(NixDaemonProtocol__StorePath::new(self.stream, self, _root)?);
        self.info = Box::new(NixDaemonProtocol__UnkeyedValidPathInfo::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__ValidPathInfo {
}
#[derive(Default)]
pub struct NixDaemonProtocol__StderrStopActivity {
    pub actId: u64,
}

impl KaitaiStruct for NixDaemonProtocol__StderrStopActivity {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.actId = self.stream.read_u8le()?;
    }
}

impl NixDaemonProtocol__StderrStopActivity {
}

/*
 * Add content to store. Protocol >= 1.25 uses new format.
 * NAR data follows via framed source after request.
 */
#[derive(Default)]
pub struct NixDaemonProtocol__AddToStoreRequest {
    pub name: Box<NixDaemonProtocol__NixString>,
    pub caMethod: Box<NixDaemonProtocol__NixString>,
    pub refs: Box<NixDaemonProtocol__StorePathSet>,
    pub repair: u64,
    pub baseName: Box<NixDaemonProtocol__NixString>,
    pub fixed: u64,
    pub recursive: u8,
    pub hashAlgo: Box<NixDaemonProtocol__NixString>,
}

impl KaitaiStruct for NixDaemonProtocol__AddToStoreRequest {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        if self._root.protocol_version >= 25 {
            self.name = Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?);
        }
        if self._root.protocol_version >= 25 {
            self.caMethod = Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?);
        }
        if self._root.protocol_version >= 25 {
            self.refs = Box::new(NixDaemonProtocol__StorePathSet::new(self.stream, self, _root)?);
        }
        if self._root.protocol_version >= 25 {
            self.repair = self.stream.read_u8le()?;
        }
        if self._root.protocol_version < 25 {
            self.baseName = Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?);
        }
        if self._root.protocol_version < 25 {
            self.fixed = self.stream.read_u8le()?;
        }
        if self._root.protocol_version < 25 {
            self.recursive = self.stream.read_u1()?;
        }
        if self._root.protocol_version < 25 {
            self.hashAlgo = Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?);
        }
    }
}

impl NixDaemonProtocol__AddToStoreRequest {

    /*
     * Content address method string (e.g. "nar:sha256")
     */
}
#[derive(Default)]
pub struct NixDaemonProtocol__DerivationOutputEntry {
    pub name: Box<NixDaemonProtocol__NixString>,
    pub path: Box<NixDaemonProtocol__OptionalStorePath>,
}

impl KaitaiStruct for NixDaemonProtocol__DerivationOutputEntry {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.name = Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?);
        self.path = Box::new(NixDaemonProtocol__OptionalStorePath::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__DerivationOutputEntry {
}
#[derive(Default)]
pub struct NixDaemonProtocol__QueryValidPathsRequest {
    pub paths: Box<NixDaemonProtocol__StorePathSet>,
    pub substitute: u64,
}

impl KaitaiStruct for NixDaemonProtocol__QueryValidPathsRequest {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.paths = Box::new(NixDaemonProtocol__StorePathSet::new(self.stream, self, _root)?);
        if self._root.protocol_version >= 27 {
            self.substitute = self.stream.read_u8le()?;
        }
    }
}

impl NixDaemonProtocol__QueryValidPathsRequest {

    /*
     * Boolean - whether to substitute missing paths
     */
}
#[derive(Default)]
pub struct NixDaemonProtocol__Realisation {
    pub id: Box<NixDaemonProtocol__DrvOutput>,
    pub outPath: Box<NixDaemonProtocol__StorePath>,
    pub signatures: Box<NixDaemonProtocol__NixStringSet>,
    pub dependentRealisations: Box<NixDaemonProtocol__DrvOutputs>,
}

impl KaitaiStruct for NixDaemonProtocol__Realisation {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.id = Box::new(NixDaemonProtocol__DrvOutput::new(self.stream, self, _root)?);
        self.outPath = Box::new(NixDaemonProtocol__StorePath::new(self.stream, self, _root)?);
        self.signatures = Box::new(NixDaemonProtocol__NixStringSet::new(self.stream, self, _root)?);
        self.dependentRealisations = Box::new(NixDaemonProtocol__DrvOutputs::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__Realisation {
}
#[derive(Default)]
pub struct NixDaemonProtocol__QueryMissingResponse {
    pub willBuild: Box<NixDaemonProtocol__StorePathSet>,
    pub willSubstitute: Box<NixDaemonProtocol__StorePathSet>,
    pub unknown: Box<NixDaemonProtocol__StorePathSet>,
    pub downloadSize: u64,
    pub narSize: u64,
}

impl KaitaiStruct for NixDaemonProtocol__QueryMissingResponse {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.willBuild = Box::new(NixDaemonProtocol__StorePathSet::new(self.stream, self, _root)?);
        self.willSubstitute = Box::new(NixDaemonProtocol__StorePathSet::new(self.stream, self, _root)?);
        self.unknown = Box::new(NixDaemonProtocol__StorePathSet::new(self.stream, self, _root)?);
        self.downloadSize = self.stream.read_u8le()?;
        self.narSize = self.stream.read_u8le()?;
    }
}

impl NixDaemonProtocol__QueryMissingResponse {
}

/*
 * Client request to daemon
 */
#[derive(Default)]
pub struct NixDaemonProtocol__Request {
    pub op: Box<NixDaemonProtocol__Operation>,
    pub payload: Option<Box<KaitaiStruct>>,
}

impl KaitaiStruct for NixDaemonProtocol__Request {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.op = self.stream.read_u8le()?;
        match self.op {
            NixDaemonProtocol__Operation::QUERY_DERIVATION_OUTPUT_MAP => {
                self.payload = Box::new(NixDaemonProtocol__QueryDerivationOutputMapRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::NAR_FROM_PATH => {
                self.payload = Box::new(NixDaemonProtocol__NarFromPathRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::QUERY_PATH_FROM_HASH_PART => {
                self.payload = Box::new(NixDaemonProtocol__QueryPathFromHashPartRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::FIND_ROOTS => {
                self.payload = Box::new(NixDaemonProtocol__EmptyRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::QUERY_REFERRERS => {
                self.payload = Box::new(NixDaemonProtocol__QueryReferrersRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::ADD_TO_STORE_NAR => {
                self.payload = Box::new(NixDaemonProtocol__AddToStoreNarRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::QUERY_VALID_DERIVERS => {
                self.payload = Box::new(NixDaemonProtocol__QueryValidDeriversRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::BUILD_PATHS => {
                self.payload = Box::new(NixDaemonProtocol__BuildPathsRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::QUERY_DERIVATION_OUTPUT_NAMES => {
                self.payload = Box::new(NixDaemonProtocol__QueryDerivationOutputNamesRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::ENSURE_PATH => {
                self.payload = Box::new(NixDaemonProtocol__EnsurePathRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::ADD_SIGNATURES => {
                self.payload = Box::new(NixDaemonProtocol__AddSignaturesRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::QUERY_SUBSTITUTABLE_PATH_INFOS => {
                self.payload = Box::new(NixDaemonProtocol__QuerySubstitutablePathInfosRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::ADD_TEXT_TO_STORE => {
                self.payload = Box::new(NixDaemonProtocol__AddTextToStoreRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::QUERY_SUBSTITUTABLE_PATH_INFO => {
                self.payload = Box::new(NixDaemonProtocol__QuerySubstitutablePathInfoRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::QUERY_MISSING => {
                self.payload = Box::new(NixDaemonProtocol__QueryMissingRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::IS_VALID_PATH => {
                self.payload = Box::new(NixDaemonProtocol__IsValidPathRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::QUERY_PATH_HASH => {
                self.payload = Box::new(NixDaemonProtocol__QueryPathHashRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::SYNC_WITH_GC => {
                self.payload = Box::new(NixDaemonProtocol__EmptyRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::ADD_PERM_ROOT => {
                self.payload = Box::new(NixDaemonProtocol__AddPermRootRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::ADD_MULTIPLE_TO_STORE => {
                self.payload = Box::new(NixDaemonProtocol__AddMultipleToStoreRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::HAS_SUBSTITUTES => {
                self.payload = Box::new(NixDaemonProtocol__HasSubstitutesRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::OPTIMISE_STORE => {
                self.payload = Box::new(NixDaemonProtocol__EmptyRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::ADD_INDIRECT_ROOT => {
                self.payload = Box::new(NixDaemonProtocol__AddIndirectRootRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::QUERY_PATH_INFO => {
                self.payload = Box::new(NixDaemonProtocol__QueryPathInfoRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::QUERY_ACTIVE_BUILDS => {
                self.payload = Box::new(NixDaemonProtocol__EmptyRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::QUERY_SUBSTITUTABLE_PATHS => {
                self.payload = Box::new(NixDaemonProtocol__QuerySubstitutablePathsRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::REGISTER_DRV_OUTPUT => {
                self.payload = Box::new(NixDaemonProtocol__RegisterDrvOutputRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::ADD_TEMP_ROOT => {
                self.payload = Box::new(NixDaemonProtocol__AddTempRootRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::BUILD_PATHS_WITH_RESULTS => {
                self.payload = Box::new(NixDaemonProtocol__BuildPathsWithResultsRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::QUERY_REALISATION => {
                self.payload = Box::new(NixDaemonProtocol__QueryRealisationRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::BUILD_DERIVATION => {
                self.payload = Box::new(NixDaemonProtocol__BuildDerivationRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::QUERY_DERIVATION_OUTPUTS => {
                self.payload = Box::new(NixDaemonProtocol__QueryDerivationOutputsRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::COLLECT_GARBAGE => {
                self.payload = Box::new(NixDaemonProtocol__CollectGarbageRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::QUERY_REFERENCES => {
                self.payload = Box::new(NixDaemonProtocol__QueryReferencesRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::VERIFY_STORE => {
                self.payload = Box::new(NixDaemonProtocol__VerifyStoreRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::ADD_TO_STORE => {
                self.payload = Box::new(NixDaemonProtocol__AddToStoreRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::QUERY_DERIVER => {
                self.payload = Box::new(NixDaemonProtocol__QueryDeriverRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::ADD_BUILD_LOG => {
                self.payload = Box::new(NixDaemonProtocol__AddBuildLogRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::SET_OPTIONS => {
                self.payload = Box::new(NixDaemonProtocol__SetOptionsRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::QUERY_VALID_PATHS => {
                self.payload = Box::new(NixDaemonProtocol__QueryValidPathsRequest::new(self.stream, self, _root)?);
            },
            NixDaemonProtocol__Operation::QUERY_ALL_VALID_PATHS => {
                self.payload = Box::new(NixDaemonProtocol__EmptyRequest::new(self.stream, self, _root)?);
            },
        }
    }
}

impl NixDaemonProtocol__Request {
}

/*
 * Derivation output identifier (drv path + output name)
 */
#[derive(Default)]
pub struct NixDaemonProtocol__DrvOutput {
    pub drvPath: Box<NixDaemonProtocol__NixString>,
    pub outputName: Box<NixDaemonProtocol__NixString>,
}

impl KaitaiStruct for NixDaemonProtocol__DrvOutput {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.drvPath = Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?);
        self.outputName = Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__DrvOutput {
}
#[derive(Default)]
pub struct NixDaemonProtocol__AddBuildLogRequest {
    pub drvPath: Box<NixDaemonProtocol__NixString>,
}

impl KaitaiStruct for NixDaemonProtocol__AddBuildLogRequest {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.drvPath = Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__AddBuildLogRequest {

    /*
     * StorePath as string
     */
}

/*
 * Obsolete since 1.25
 */
#[derive(Default)]
pub struct NixDaemonProtocol__AddTextToStoreRequest {
    pub suffix: Box<NixDaemonProtocol__NixString>,
    pub text: Box<NixDaemonProtocol__NixString>,
    pub refs: Box<NixDaemonProtocol__StorePathSet>,
}

impl KaitaiStruct for NixDaemonProtocol__AddTextToStoreRequest {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.suffix = Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?);
        self.text = Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?);
        self.refs = Box::new(NixDaemonProtocol__StorePathSet::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__AddTextToStoreRequest {
}
#[derive(Default)]
pub struct NixDaemonProtocol__FindRootsResponse {
    pub numRoots: u64,
    pub roots: Vec<Box<NixDaemonProtocol__RootEntry>>,
}

impl KaitaiStruct for NixDaemonProtocol__FindRootsResponse {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.numRoots = self.stream.read_u8le()?;
        self.roots = vec!();
        for i in 0..self.num_roots {
            self.roots.append(Box::new(NixDaemonProtocol__RootEntry::new(self.stream, self, _root)?));
        }
    }
}

impl NixDaemonProtocol__FindRootsResponse {
}
#[derive(Default)]
pub struct NixDaemonProtocol__CollectGarbageResponse {
    pub paths: Box<NixDaemonProtocol__NixStringSet>,
    pub bytesFreed: u64,
    pub obsolete: u64,
}

impl KaitaiStruct for NixDaemonProtocol__CollectGarbageResponse {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.paths = Box::new(NixDaemonProtocol__NixStringSet::new(self.stream, self, _root)?);
        self.bytesFreed = self.stream.read_u8le()?;
        self.obsolete = self.stream.read_u8le()?;
    }
}

impl NixDaemonProtocol__CollectGarbageResponse {
}
#[derive(Default)]
pub struct NixDaemonProtocol__AddTempRootRequest {
    pub path: Box<NixDaemonProtocol__StorePath>,
}

impl KaitaiStruct for NixDaemonProtocol__AddTempRootRequest {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.path = Box::new(NixDaemonProtocol__StorePath::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__AddTempRootRequest {
}
#[derive(Default)]
pub struct NixDaemonProtocol__SettingOverrides {
    pub numSettings: u64,
    pub settings: Vec<Box<NixDaemonProtocol__SettingPair>>,
}

impl KaitaiStruct for NixDaemonProtocol__SettingOverrides {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.numSettings = self.stream.read_u8le()?;
        self.settings = vec!();
        for i in 0..self.num_settings {
            self.settings.append(Box::new(NixDaemonProtocol__SettingPair::new(self.stream, self, _root)?));
        }
    }
}

impl NixDaemonProtocol__SettingOverrides {
}
#[derive(Default)]
pub struct NixDaemonProtocol__QuerySubstitutablePathInfosResponse {
    pub numInfos: u64,
    pub infos: Vec<Box<NixDaemonProtocol__SubstitutablePathInfoEntry>>,
}

impl KaitaiStruct for NixDaemonProtocol__QuerySubstitutablePathInfosResponse {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.numInfos = self.stream.read_u8le()?;
        self.infos = vec!();
        for i in 0..self.num_infos {
            self.infos.append(Box::new(NixDaemonProtocol__SubstitutablePathInfoEntry::new(self.stream, self, _root)?));
        }
    }
}

impl NixDaemonProtocol__QuerySubstitutablePathInfosResponse {
}
#[derive(Default)]
pub struct NixDaemonProtocol__StderrResult {
    pub actId: u64,
    pub resultType: u64,
    pub fields: Box<NixDaemonProtocol__LoggerFields>,
}

impl KaitaiStruct for NixDaemonProtocol__StderrResult {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.actId = self.stream.read_u8le()?;
        self.resultType = self.stream.read_u8le()?;
        self.fields = Box::new(NixDaemonProtocol__LoggerFields::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__StderrResult {
}
#[derive(Default)]
pub struct NixDaemonProtocol__SettingPair {
    pub name: Box<NixDaemonProtocol__NixString>,
    pub value: Box<NixDaemonProtocol__NixString>,
}

impl KaitaiStruct for NixDaemonProtocol__SettingPair {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.name = Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?);
        self.value = Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__SettingPair {
}
#[derive(Default)]
pub struct NixDaemonProtocol__IsValidPathResponse {
    pub valid: u64,
}

impl KaitaiStruct for NixDaemonProtocol__IsValidPathResponse {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.valid = self.stream.read_u8le()?;
    }
}

impl NixDaemonProtocol__IsValidPathResponse {
}
#[derive(Default)]
pub struct NixDaemonProtocol__LoggerFields {
    pub numFields: u64,
    pub fields: Vec<Box<NixDaemonProtocol__LoggerField>>,
}

impl KaitaiStruct for NixDaemonProtocol__LoggerFields {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.numFields = self.stream.read_u8le()?;
        self.fields = vec!();
        for i in 0..self.num_fields {
            self.fields.append(Box::new(NixDaemonProtocol__LoggerField::new(self.stream, self, _root)?));
        }
    }
}

impl NixDaemonProtocol__LoggerFields {
}

/*
 * Optional microseconds duration
 */
#[derive(Default)]
pub struct NixDaemonProtocol__OptionalDuration {
    pub tag: u8,
    pub microseconds: i64,
}

impl KaitaiStruct for NixDaemonProtocol__OptionalDuration {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.tag = self.stream.read_u1()?;
        if self.tag == 1 {
            self.microseconds = self.stream.read_s8le()?;
        }
    }
}

impl NixDaemonProtocol__OptionalDuration {
}
#[derive(Default)]
pub struct NixDaemonProtocol__HasSubstitutesRequest {
    pub path: Box<NixDaemonProtocol__StorePath>,
}

impl KaitaiStruct for NixDaemonProtocol__HasSubstitutesRequest {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.path = Box::new(NixDaemonProtocol__StorePath::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__HasSubstitutesRequest {
}
#[derive(Default)]
pub struct NixDaemonProtocol__NarFromPathRequest {
    pub path: Box<NixDaemonProtocol__StorePath>,
}

impl KaitaiStruct for NixDaemonProtocol__NarFromPathRequest {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.path = Box::new(NixDaemonProtocol__StorePath::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__NarFromPathRequest {
}

/*
 * Sorted set of strings (same wire format as list)
 */
#[derive(Default)]
pub struct NixDaemonProtocol__NixStringSet {
    pub numItems: u64,
    pub items: Vec<Box<NixDaemonProtocol__NixString>>,
}

impl KaitaiStruct for NixDaemonProtocol__NixStringSet {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.numItems = self.stream.read_u8le()?;
        self.items = vec!();
        for i in 0..self.num_items {
            self.items.append(Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?));
        }
    }
}

impl NixDaemonProtocol__NixStringSet {
}

/*
 * Server info sent after version negotiation
 */
#[derive(Default)]
pub struct NixDaemonProtocol__ServerHandshakeInfo {
    pub daemonVersion: Box<NixDaemonProtocol__NixString>,
    pub trustLevel: Box<NixDaemonProtocol__TrustLevel>,
}

impl KaitaiStruct for NixDaemonProtocol__ServerHandshakeInfo {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        if self._root.protocol_version >= 33 {
            self.daemonVersion = Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?);
        }
        if self._root.protocol_version >= 35 {
            self.trustLevel = self.stream.read_u8le()?;
        }
    }
}

impl NixDaemonProtocol__ServerHandshakeInfo {
}
#[derive(Default)]
pub struct NixDaemonProtocol__AddSignaturesRequest {
    pub path: Box<NixDaemonProtocol__StorePath>,
    pub sigs: Box<NixDaemonProtocol__NixStringSet>,
}

impl KaitaiStruct for NixDaemonProtocol__AddSignaturesRequest {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.path = Box::new(NixDaemonProtocol__StorePath::new(self.stream, self, _root)?);
        self.sigs = Box::new(NixDaemonProtocol__NixStringSet::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__AddSignaturesRequest {
}

/*
 * Structured error (protocol >= 1.26)
 */
#[derive(Default)]
pub struct NixDaemonProtocol__NixError {
    pub type: Box<NixDaemonProtocol__NixString>,
    pub level: u64,
    pub name: Box<NixDaemonProtocol__NixString>,
    pub msg: Box<NixDaemonProtocol__NixString>,
    pub havePos: u64,
    pub traces: Box<NixDaemonProtocol__ErrorTraceList>,
}

impl KaitaiStruct for NixDaemonProtocol__NixError {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.type = Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?);
        self.level = self.stream.read_u8le()?;
        self.name = Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?);
        self.msg = Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?);
        self.havePos = self.stream.read_u8le()?;
        self.traces = Box::new(NixDaemonProtocol__ErrorTraceList::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__NixError {
}
#[derive(Default)]
pub struct NixDaemonProtocol__QueryReferencesRequest {
    pub path: Box<NixDaemonProtocol__StorePath>,
}

impl KaitaiStruct for NixDaemonProtocol__QueryReferencesRequest {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.path = Box::new(NixDaemonProtocol__StorePath::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__QueryReferencesRequest {
}
#[derive(Default)]
pub struct NixDaemonProtocol__IsValidPathRequest {
    pub path: Box<NixDaemonProtocol__StorePath>,
}

impl KaitaiStruct for NixDaemonProtocol__IsValidPathRequest {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.path = Box::new(NixDaemonProtocol__StorePath::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__IsValidPathRequest {
}

/*
 * Store path, e.g. "/nix/store/abc123-hello-1.0"
 */
#[derive(Default)]
pub struct NixDaemonProtocol__StorePath {
    pub path: Box<NixDaemonProtocol__NixString>,
}

impl KaitaiStruct for NixDaemonProtocol__StorePath {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.path = Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__StorePath {
}
#[derive(Default)]
pub struct NixDaemonProtocol__QuerySubstitutablePathsRequest {
    pub paths: Box<NixDaemonProtocol__StorePathSet>,
}

impl KaitaiStruct for NixDaemonProtocol__QuerySubstitutablePathsRequest {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.paths = Box::new(NixDaemonProtocol__StorePathSet::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__QuerySubstitutablePathsRequest {
}

/*
 * After version exchange, client sends additional handshake info.
 * Fields depend on negotiated version.
 */
#[derive(Default)]
pub struct NixDaemonProtocol__ClientHandshakeContinuation {
    pub cpuAffinityTag: u64,
    pub cpuAffinity: u64,
    pub reserveSpace: u64,
}

impl KaitaiStruct for NixDaemonProtocol__ClientHandshakeContinuation {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        if self._root.protocol_version >= 14 {
            self.cpuAffinityTag = self.stream.read_u8le()?;
        }
        if  ((self._root.protocol_version >= 14) && (self.cpu_affinity_tag != 0))  {
            self.cpuAffinity = self.stream.read_u8le()?;
        }
        if self._root.protocol_version >= 11 {
            self.reserveSpace = self.stream.read_u8le()?;
        }
    }
}

impl NixDaemonProtocol__ClientHandshakeContinuation {
}

/*
 * During request processing, daemon sends stderr messages.
 * Client must handle these until receiving STDERR_LAST.
 */
#[derive(Default)]
pub struct NixDaemonProtocol__StderrMessage {
    pub msgType: u64,
    pub payload: Option<Box<KaitaiStruct>>,
}

impl KaitaiStruct for NixDaemonProtocol__StderrMessage {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.msgType = self.stream.read_u8le()?;
        match self.msg_type {
            1634497651 => {
                self.payload = Box::new(NixDaemonProtocol__StderrLast::new(self.stream, self, _root)?);
            },
            1381190740 => {
                self.payload = Box::new(NixDaemonProtocol__StderrResult::new(self.stream, self, _root)?);
            },
            1684108310 => {
                self.payload = Box::new(NixDaemonProtocol__StderrWrite::new(self.stream, self, _root)?);
            },
            1684108385 => {
                self.payload = Box::new(NixDaemonProtocol__StderrRead::new(self.stream, self, _root)?);
            },
            1668838512 => {
                self.payload = Box::new(NixDaemonProtocol__StderrError::new(self.stream, self, _root)?);
            },
            1869376871 => {
                self.payload = Box::new(NixDaemonProtocol__StderrNext::new(self.stream, self, _root)?);
            },
            1398034256 => {
                self.payload = Box::new(NixDaemonProtocol__StderrStopActivity::new(self.stream, self, _root)?);
            },
            1398035028 => {
                self.payload = Box::new(NixDaemonProtocol__StderrStartActivity::new(self.stream, self, _root)?);
            },
        }
    }
}

impl NixDaemonProtocol__StderrMessage {
}
#[derive(Default)]
pub struct NixDaemonProtocol__StderrStartActivity {
    pub actId: u64,
    pub verbosity: u64,
    pub activityType: u64,
    pub msg: Box<NixDaemonProtocol__NixString>,
    pub fields: Box<NixDaemonProtocol__LoggerFields>,
    pub parent: u64,
}

impl KaitaiStruct for NixDaemonProtocol__StderrStartActivity {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.actId = self.stream.read_u8le()?;
        self.verbosity = self.stream.read_u8le()?;
        self.activityType = self.stream.read_u8le()?;
        self.msg = Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?);
        self.fields = Box::new(NixDaemonProtocol__LoggerFields::new(self.stream, self, _root)?);
        self.parent = self.stream.read_u8le()?;
    }
}

impl NixDaemonProtocol__StderrStartActivity {
}
#[derive(Default)]
pub struct NixDaemonProtocol__StorePathSet {
    pub numPaths: u64,
    pub paths: Vec<Box<NixDaemonProtocol__StorePath>>,
}

impl KaitaiStruct for NixDaemonProtocol__StorePathSet {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.numPaths = self.stream.read_u8le()?;
        self.paths = vec!();
        for i in 0..self.num_paths {
            self.paths.append(Box::new(NixDaemonProtocol__StorePath::new(self.stream, self, _root)?));
        }
    }
}

impl NixDaemonProtocol__StorePathSet {
}
#[derive(Default)]
pub struct NixDaemonProtocol__StorePathCaEntry {
    pub path: Box<NixDaemonProtocol__StorePath>,
    pub ca: Box<NixDaemonProtocol__NixString>,
}

impl KaitaiStruct for NixDaemonProtocol__StorePathCaEntry {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.path = Box::new(NixDaemonProtocol__StorePath::new(self.stream, self, _root)?);
        self.ca = Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__StorePathCaEntry {

    /*
     * Content address string, empty = none
     */
}

/*
 * Length-prefixed string, padded to 8-byte boundary
 */
#[derive(Default)]
pub struct NixDaemonProtocol__NixString {
    pub len: u64,
    pub data: String,
    pub padding: Vec<u8>,
}

impl KaitaiStruct for NixDaemonProtocol__NixString {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.len = self.stream.read_u8le()?;
        self.data = panic!("Unimplemented encoding for bytesToStr: {}", "UTF-8");
        self.padding = self.stream.read_bytes((8 - self.len % 8) % 8)?;
    }
}

impl NixDaemonProtocol__NixString {
}
#[derive(Default)]
pub struct NixDaemonProtocol__BuildPathsWithResultsRequest {
    pub paths: Box<NixDaemonProtocol__DerivedPathList>,
    pub buildMode: Box<NixDaemonProtocol__BuildMode>,
}

impl KaitaiStruct for NixDaemonProtocol__BuildPathsWithResultsRequest {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.paths = Box::new(NixDaemonProtocol__DerivedPathList::new(self.stream, self, _root)?);
        self.buildMode = self.stream.read_u1()?;
    }
}

impl NixDaemonProtocol__BuildPathsWithResultsRequest {
}
#[derive(Default)]
pub struct NixDaemonProtocol__ErrorTrace {
    pub havePos: u64,
    pub msg: Box<NixDaemonProtocol__NixString>,
}

impl KaitaiStruct for NixDaemonProtocol__ErrorTrace {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.havePos = self.stream.read_u8le()?;
        self.msg = Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__ErrorTrace {
}
#[derive(Default)]
pub struct NixDaemonProtocol__SetOptionsRequest {
    pub keepFailed: u64,
    pub keepGoing: u64,
    pub tryFallback: u64,
    pub verbosity: u64,
    pub maxBuildJobs: u64,
    pub maxSilentTime: u64,
    pub obsoleteUseBuildHook: u64,
    pub verboseBuild: u64,
    pub obsoleteLogType: u64,
    pub obsoletePrintBuildTrace: u64,
    pub buildCores: u64,
    pub useSubstitutes: u64,
    pub overrides: Box<NixDaemonProtocol__SettingOverrides>,
}

impl KaitaiStruct for NixDaemonProtocol__SetOptionsRequest {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.keepFailed = self.stream.read_u8le()?;
        self.keepGoing = self.stream.read_u8le()?;
        self.tryFallback = self.stream.read_u8le()?;
        self.verbosity = self.stream.read_u8le()?;
        self.maxBuildJobs = self.stream.read_u8le()?;
        self.maxSilentTime = self.stream.read_u8le()?;
        self.obsoleteUseBuildHook = self.stream.read_u8le()?;
        self.verboseBuild = self.stream.read_u8le()?;
        self.obsoleteLogType = self.stream.read_u8le()?;
        self.obsoletePrintBuildTrace = self.stream.read_u8le()?;
        self.buildCores = self.stream.read_u8le()?;
        self.useSubstitutes = self.stream.read_u8le()?;
        if self._root.protocol_version >= 12 {
            self.overrides = Box::new(NixDaemonProtocol__SettingOverrides::new(self.stream, self, _root)?);
        }
    }
}

impl NixDaemonProtocol__SetOptionsRequest {
}

/*
 * Protocol >= 1.31
 */
#[derive(Default)]
pub struct NixDaemonProtocol__QueryRealisationResponse {
    pub numRealisations: u64,
    pub realisations: Vec<Box<NixDaemonProtocol__Realisation>>,
}

impl KaitaiStruct for NixDaemonProtocol__QueryRealisationResponse {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.numRealisations = self.stream.read_u8le()?;
        self.realisations = vec!();
        for i in 0..self.num_realisations {
            self.realisations.append(Box::new(NixDaemonProtocol__Realisation::new(self.stream, self, _root)?));
        }
    }
}

impl NixDaemonProtocol__QueryRealisationResponse {
}
#[derive(Default)]
pub struct NixDaemonProtocol__QuerySubstitutablePathInfoRequest {
    pub path: Box<NixDaemonProtocol__StorePath>,
}

impl KaitaiStruct for NixDaemonProtocol__QuerySubstitutablePathInfoRequest {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.path = Box::new(NixDaemonProtocol__StorePath::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__QuerySubstitutablePathInfoRequest {
}
#[derive(Default)]
pub struct NixDaemonProtocol__QueryDerivationOutputMapRequest {
    pub path: Box<NixDaemonProtocol__StorePath>,
}

impl KaitaiStruct for NixDaemonProtocol__QueryDerivationOutputMapRequest {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.path = Box::new(NixDaemonProtocol__StorePath::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__QueryDerivationOutputMapRequest {
}

/*
 * Build a derivation. The derivation is sent inline (not read from store).
 * Derivation format is ATerm-based, parsed separately.
 */
#[derive(Default)]
pub struct NixDaemonProtocol__BuildDerivationRequest {
    pub drvPath: Box<NixDaemonProtocol__StorePath>,
    pub derivation: Box<NixDaemonProtocol__NixBytes>,
    pub buildMode: Box<NixDaemonProtocol__BuildMode>,
}

impl KaitaiStruct for NixDaemonProtocol__BuildDerivationRequest {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.drvPath = Box::new(NixDaemonProtocol__StorePath::new(self.stream, self, _root)?);
        self.derivation = Box::new(NixDaemonProtocol__NixBytes::new(self.stream, self, _root)?);
        self.buildMode = self.stream.read_u1()?;
    }
}

impl NixDaemonProtocol__BuildDerivationRequest {

    /*
     * Serialized BasicDerivation in ATerm format
     */
}

/*
 * Log message from daemon
 */
#[derive(Default)]
pub struct NixDaemonProtocol__StderrNext {
    pub msg: Box<NixDaemonProtocol__NixString>,
}

impl KaitaiStruct for NixDaemonProtocol__StderrNext {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.msg = Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__StderrNext {
}
#[derive(Default)]
pub struct NixDaemonProtocol__DerivationOutputMap {
    pub numOutputs: u64,
    pub outputs: Vec<Box<NixDaemonProtocol__DerivationOutputEntry>>,
}

impl KaitaiStruct for NixDaemonProtocol__DerivationOutputMap {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.numOutputs = self.stream.read_u8le()?;
        self.outputs = vec!();
        for i in 0..self.num_outputs {
            self.outputs.append(Box::new(NixDaemonProtocol__DerivationOutputEntry::new(self.stream, self, _root)?));
        }
    }
}

impl NixDaemonProtocol__DerivationOutputMap {
}

/*
 * Operations with no request payload
 */
#[derive(Default)]
pub struct NixDaemonProtocol__EmptyRequest {
}

impl KaitaiStruct for NixDaemonProtocol__EmptyRequest {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
    }
}

impl NixDaemonProtocol__EmptyRequest {
}
#[derive(Default)]
pub struct NixDaemonProtocol__AddPermRootRequest {
    pub storePath: Box<NixDaemonProtocol__StorePath>,
    pub gcRoot: Box<NixDaemonProtocol__NixString>,
}

impl KaitaiStruct for NixDaemonProtocol__AddPermRootRequest {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.storePath = Box::new(NixDaemonProtocol__StorePath::new(self.stream, self, _root)?);
        self.gcRoot = Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__AddPermRootRequest {

    /*
     * Absolute filesystem path for the symlink
     */
}
#[derive(Default)]
pub struct NixDaemonProtocol__QuerySubstitutablePathInfoResponse {
    pub found: u64,
    pub deriver: Box<NixDaemonProtocol__OptionalStorePath>,
    pub references: Box<NixDaemonProtocol__StorePathSet>,
    pub downloadSize: u64,
    pub narSize: u64,
}

impl KaitaiStruct for NixDaemonProtocol__QuerySubstitutablePathInfoResponse {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.found = self.stream.read_u8le()?;
        if self.found != 0 {
            self.deriver = Box::new(NixDaemonProtocol__OptionalStorePath::new(self.stream, self, _root)?);
        }
        if self.found != 0 {
            self.references = Box::new(NixDaemonProtocol__StorePathSet::new(self.stream, self, _root)?);
        }
        if self.found != 0 {
            self.downloadSize = self.stream.read_u8le()?;
        }
        if self.found != 0 {
            self.narSize = self.stream.read_u8le()?;
        }
    }
}

impl NixDaemonProtocol__QuerySubstitutablePathInfoResponse {
}
#[derive(Default)]
pub struct NixDaemonProtocol__QueryPathFromHashPartRequest {
    pub hashPart: Box<NixDaemonProtocol__NixString>,
}

impl KaitaiStruct for NixDaemonProtocol__QueryPathFromHashPartRequest {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.hashPart = Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__QueryPathFromHashPartRequest {

    /*
     * First 32 chars of store path hash
     */
}
#[derive(Default)]
pub struct NixDaemonProtocol__DerivedPathList {
    pub numPaths: u64,
    pub paths: Vec<Box<NixDaemonProtocol__DerivedPath>>,
}

impl KaitaiStruct for NixDaemonProtocol__DerivedPathList {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.numPaths = self.stream.read_u8le()?;
        self.paths = vec!();
        for i in 0..self.num_paths {
            self.paths.append(Box::new(NixDaemonProtocol__DerivedPath::new(self.stream, self, _root)?));
        }
    }
}

impl NixDaemonProtocol__DerivedPathList {
}

/*
 * String representation of a derived path.
 * Format: "/nix/store/...-foo" or "/nix/store/...-foo.drv^out,dev"
 */
#[derive(Default)]
pub struct NixDaemonProtocol__DerivedPath {
    pub path: Box<NixDaemonProtocol__NixString>,
}

impl KaitaiStruct for NixDaemonProtocol__DerivedPath {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.path = Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__DerivedPath {
}
#[derive(Default)]
pub struct NixDaemonProtocol__QueryMissingRequest {
    pub targets: Box<NixDaemonProtocol__DerivedPathList>,
}

impl KaitaiStruct for NixDaemonProtocol__QueryMissingRequest {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.targets = Box::new(NixDaemonProtocol__DerivedPathList::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__QueryMissingRequest {
}
#[derive(Default)]
pub struct NixDaemonProtocol__QueryValidDeriversRequest {
    pub path: Box<NixDaemonProtocol__StorePath>,
}

impl KaitaiStruct for NixDaemonProtocol__QueryValidDeriversRequest {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.path = Box::new(NixDaemonProtocol__StorePath::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__QueryValidDeriversRequest {
}
#[derive(Default)]
pub struct NixDaemonProtocol__UnkeyedValidPathInfo {
    pub deriver: Box<NixDaemonProtocol__OptionalStorePath>,
    pub narHash: Box<NixDaemonProtocol__NixString>,
    pub references: Box<NixDaemonProtocol__StorePathSet>,
    pub registrationTime: u64,
    pub narSize: u64,
    pub ultimate: u64,
    pub sigs: Box<NixDaemonProtocol__NixStringSet>,
    pub ca: Box<NixDaemonProtocol__NixString>,
}

impl KaitaiStruct for NixDaemonProtocol__UnkeyedValidPathInfo {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.deriver = Box::new(NixDaemonProtocol__OptionalStorePath::new(self.stream, self, _root)?);
        self.narHash = Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?);
        self.references = Box::new(NixDaemonProtocol__StorePathSet::new(self.stream, self, _root)?);
        self.registrationTime = self.stream.read_u8le()?;
        self.narSize = self.stream.read_u8le()?;
        if self._root.protocol_version >= 16 {
            self.ultimate = self.stream.read_u8le()?;
        }
        if self._root.protocol_version >= 16 {
            self.sigs = Box::new(NixDaemonProtocol__NixStringSet::new(self.stream, self, _root)?);
        }
        if self._root.protocol_version >= 16 {
            self.ca = Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?);
        }
    }
}

impl NixDaemonProtocol__UnkeyedValidPathInfo {

    /*
     * SHA256 hash in base16
     */

    /*
     * Content address (empty = none)
     */
}

/*
 * Protocol >= 1.38 feature negotiation
 */
#[derive(Default)]
pub struct NixDaemonProtocol__FeatureExchange {
    pub features: Box<NixDaemonProtocol__NixStringSet>,
}

impl KaitaiStruct for NixDaemonProtocol__FeatureExchange {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.features = Box::new(NixDaemonProtocol__NixStringSet::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__FeatureExchange {
}
#[derive(Default)]
pub struct NixDaemonProtocol__NixStringList {
    pub numItems: u64,
    pub items: Vec<Box<NixDaemonProtocol__NixString>>,
}

impl KaitaiStruct for NixDaemonProtocol__NixStringList {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.numItems = self.stream.read_u8le()?;
        self.items = vec!();
        for i in 0..self.num_items {
            self.items.append(Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?));
        }
    }
}

impl NixDaemonProtocol__NixStringList {
}
#[derive(Default)]
pub struct NixDaemonProtocol__ErrorTraceList {
    pub numTraces: u64,
    pub traces: Vec<Box<NixDaemonProtocol__ErrorTrace>>,
}

impl KaitaiStruct for NixDaemonProtocol__ErrorTraceList {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.numTraces = self.stream.read_u8le()?;
        self.traces = vec!();
        for i in 0..self.num_traces {
            self.traces.append(Box::new(NixDaemonProtocol__ErrorTrace::new(self.stream, self, _root)?));
        }
    }
}

impl NixDaemonProtocol__ErrorTraceList {
}
#[derive(Default)]
pub struct NixDaemonProtocol__QueryReferrersRequest {
    pub path: Box<NixDaemonProtocol__StorePath>,
}

impl KaitaiStruct for NixDaemonProtocol__QueryReferrersRequest {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.path = Box::new(NixDaemonProtocol__StorePath::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__QueryReferrersRequest {
}

/*
 * Length-prefixed bytes, padded to 8-byte boundary
 */
#[derive(Default)]
pub struct NixDaemonProtocol__NixBytes {
    pub len: u64,
    pub data: Vec<u8>,
    pub padding: Vec<u8>,
}

impl KaitaiStruct for NixDaemonProtocol__NixBytes {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.len = self.stream.read_u8le()?;
        self.data = self.stream.read_bytes(self.len)?;
        self.padding = self.stream.read_bytes((8 - self.len % 8) % 8)?;
    }
}

impl NixDaemonProtocol__NixBytes {
}
#[derive(Default)]
pub struct NixDaemonProtocol__QueryDerivationOutputNamesRequest {
    pub path: Box<NixDaemonProtocol__StorePath>,
}

impl KaitaiStruct for NixDaemonProtocol__QueryDerivationOutputNamesRequest {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.path = Box::new(NixDaemonProtocol__StorePath::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__QueryDerivationOutputNamesRequest {
}
#[derive(Default)]
pub struct NixDaemonProtocol__VerifyStoreRequest {
    pub checkContents: u64,
    pub repair: u64,
}

impl KaitaiStruct for NixDaemonProtocol__VerifyStoreRequest {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.checkContents = self.stream.read_u8le()?;
        self.repair = self.stream.read_u8le()?;
    }
}

impl NixDaemonProtocol__VerifyStoreRequest {
}

/*
 * Response from daemon after receiving client_hello
 */
#[derive(Default)]
pub struct NixDaemonProtocol__ServerHello {
    pub magic: Vec<u8>,
    pub padding1: Vec<u8>,
    pub serverVersion: u64,
}

impl KaitaiStruct for NixDaemonProtocol__ServerHello {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.magic = self.stream.read_bytes(4)?;
        self.padding1 = self.stream.read_bytes(4)?;
        self.serverVersion = self.stream.read_u8le()?;
    }
}

impl NixDaemonProtocol__ServerHello {

    /*
     * WORKER_MAGIC_2
     */

    /*
     * Protocol version (major << 8 | minor)
     */
}
#[derive(Default)]
pub struct NixDaemonProtocol__QueryPathInfoResponse {
    pub valid: u64,
    pub info: Box<NixDaemonProtocol__UnkeyedValidPathInfo>,
}

impl KaitaiStruct for NixDaemonProtocol__QueryPathInfoResponse {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.valid = self.stream.read_u8le()?;
        if self.valid != 0 {
            self.info = Box::new(NixDaemonProtocol__UnkeyedValidPathInfo::new(self.stream, self, _root)?);
        }
    }
}

impl NixDaemonProtocol__QueryPathInfoResponse {
}

/*
 * Initial message from client to daemon
 */
#[derive(Default)]
pub struct NixDaemonProtocol__ClientHello {
    pub magic: Vec<u8>,
    pub padding1: Vec<u8>,
    pub clientVersion: u64,
}

impl KaitaiStruct for NixDaemonProtocol__ClientHello {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.magic = self.stream.read_bytes(4)?;
        self.padding1 = self.stream.read_bytes(4)?;
        self.clientVersion = self.stream.read_u8le()?;
    }
}

impl NixDaemonProtocol__ClientHello {

    /*
     * WORKER_MAGIC_1
     */

    /*
     * Protocol version (major << 8 | minor)
     */
}
#[derive(Default)]
pub struct NixDaemonProtocol__QueryRealisationRequest {
    pub outputId: Box<NixDaemonProtocol__DrvOutput>,
}

impl KaitaiStruct for NixDaemonProtocol__QueryRealisationRequest {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.outputId = Box::new(NixDaemonProtocol__DrvOutput::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__QueryRealisationRequest {
}

/*
 * Error during request processing
 */
#[derive(Default)]
pub struct NixDaemonProtocol__StderrError {
    pub error: Box<NixDaemonProtocol__NixError>,
    pub errorMsg: Box<NixDaemonProtocol__NixString>,
    pub status: u64,
}

impl KaitaiStruct for NixDaemonProtocol__StderrError {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        if self._root.protocol_version >= 26 {
            self.error = Box::new(NixDaemonProtocol__NixError::new(self.stream, self, _root)?);
        }
        if self._root.protocol_version < 26 {
            self.errorMsg = Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?);
        }
        if self._root.protocol_version < 26 {
            self.status = self.stream.read_u8le()?;
        }
    }
}

impl NixDaemonProtocol__StderrError {
}
#[derive(Default)]
pub struct NixDaemonProtocol__BuildPathsWithResultsResponse {
    pub numResults: u64,
    pub results: Vec<Box<NixDaemonProtocol__KeyedBuildResult>>,
}

impl KaitaiStruct for NixDaemonProtocol__BuildPathsWithResultsResponse {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.numResults = self.stream.read_u8le()?;
        self.results = vec!();
        for i in 0..self.num_results {
            self.results.append(Box::new(NixDaemonProtocol__KeyedBuildResult::new(self.stream, self, _root)?));
        }
    }
}

impl NixDaemonProtocol__BuildPathsWithResultsResponse {
}
#[derive(Default)]
pub struct NixDaemonProtocol__EnsurePathRequest {
    pub path: Box<NixDaemonProtocol__StorePath>,
}

impl KaitaiStruct for NixDaemonProtocol__EnsurePathRequest {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.path = Box::new(NixDaemonProtocol__StorePath::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__EnsurePathRequest {
}
#[derive(Default)]
pub struct NixDaemonProtocol__RootEntry {
    pub link: Box<NixDaemonProtocol__NixString>,
    pub target: Box<NixDaemonProtocol__StorePath>,
}

impl KaitaiStruct for NixDaemonProtocol__RootEntry {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.link = Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?);
        self.target = Box::new(NixDaemonProtocol__StorePath::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__RootEntry {
}
#[derive(Default)]
pub struct NixDaemonProtocol__LoggerField {
    pub fieldType: Box<NixDaemonProtocol__LoggerFieldType>,
    pub intValue: u64,
    pub stringValue: Box<NixDaemonProtocol__NixString>,
}

impl KaitaiStruct for NixDaemonProtocol__LoggerField {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.fieldType = self.stream.read_u8le()?;
        if self.field_type == NixDaemonProtocol__LoggerFieldType::INT {
            self.intValue = self.stream.read_u8le()?;
        }
        if self.field_type == NixDaemonProtocol__LoggerFieldType::STRING {
            self.stringValue = Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?);
        }
    }
}

impl NixDaemonProtocol__LoggerField {
}

/*
 * Daemon sending data to client (for streaming downloads)
 */
#[derive(Default)]
pub struct NixDaemonProtocol__StderrWrite {
    pub data: Box<NixDaemonProtocol__NixString>,
}

impl KaitaiStruct for NixDaemonProtocol__StderrWrite {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.data = Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__StderrWrite {
}
#[derive(Default)]
pub struct NixDaemonProtocol__QueryDeriverRequest {
    pub path: Box<NixDaemonProtocol__StorePath>,
}

impl KaitaiStruct for NixDaemonProtocol__QueryDeriverRequest {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.path = Box::new(NixDaemonProtocol__StorePath::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__QueryDeriverRequest {
}
#[derive(Default)]
pub struct NixDaemonProtocol__QueryDerivationOutputsRequest {
    pub path: Box<NixDaemonProtocol__StorePath>,
}

impl KaitaiStruct for NixDaemonProtocol__QueryDerivationOutputsRequest {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.path = Box::new(NixDaemonProtocol__StorePath::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__QueryDerivationOutputsRequest {
}

/*
 * Protocol < 1.31
 */
#[derive(Default)]
pub struct NixDaemonProtocol__QueryRealisationResponseOld {
    pub outPaths: Box<NixDaemonProtocol__StorePathSet>,
}

impl KaitaiStruct for NixDaemonProtocol__QueryRealisationResponseOld {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.outPaths = Box::new(NixDaemonProtocol__StorePathSet::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__QueryRealisationResponseOld {
}
#[derive(Default)]
pub struct NixDaemonProtocol__CollectGarbageRequest {
    pub action: Box<NixDaemonProtocol__GcAction>,
    pub pathsToDelete: Box<NixDaemonProtocol__StorePathSet>,
    pub ignoreLiveness: u64,
    pub maxFreed: u64,
    pub obsolete1: u64,
    pub obsolete2: u64,
    pub obsolete3: u64,
}

impl KaitaiStruct for NixDaemonProtocol__CollectGarbageRequest {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.action = self.stream.read_u8le()?;
        self.pathsToDelete = Box::new(NixDaemonProtocol__StorePathSet::new(self.stream, self, _root)?);
        self.ignoreLiveness = self.stream.read_u8le()?;
        self.maxFreed = self.stream.read_u8le()?;
        self.obsolete1 = self.stream.read_u8le()?;
        self.obsolete2 = self.stream.read_u8le()?;
        self.obsolete3 = self.stream.read_u8le()?;
    }
}

impl NixDaemonProtocol__CollectGarbageRequest {
}
#[derive(Default)]
pub struct NixDaemonProtocol__SubstitutablePathInfoEntry {
    pub path: Box<NixDaemonProtocol__StorePath>,
    pub deriver: Box<NixDaemonProtocol__OptionalStorePath>,
    pub references: Box<NixDaemonProtocol__StorePathSet>,
    pub downloadSize: u64,
    pub narSize: u64,
}

impl KaitaiStruct for NixDaemonProtocol__SubstitutablePathInfoEntry {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.path = Box::new(NixDaemonProtocol__StorePath::new(self.stream, self, _root)?);
        self.deriver = Box::new(NixDaemonProtocol__OptionalStorePath::new(self.stream, self, _root)?);
        self.references = Box::new(NixDaemonProtocol__StorePathSet::new(self.stream, self, _root)?);
        self.downloadSize = self.stream.read_u8le()?;
        self.narSize = self.stream.read_u8le()?;
    }
}

impl NixDaemonProtocol__SubstitutablePathInfoEntry {
}
#[derive(Default)]
pub struct NixDaemonProtocol__QueryPathInfoRequest {
    pub path: Box<NixDaemonProtocol__StorePath>,
}

impl KaitaiStruct for NixDaemonProtocol__QueryPathInfoRequest {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.path = Box::new(NixDaemonProtocol__StorePath::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__QueryPathInfoRequest {
}
#[derive(Default)]
pub struct NixDaemonProtocol__KeyedBuildResult {
    pub path: Box<NixDaemonProtocol__DerivedPath>,
    pub result: Box<NixDaemonProtocol__BuildResult>,
}

impl KaitaiStruct for NixDaemonProtocol__KeyedBuildResult {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.path = Box::new(NixDaemonProtocol__DerivedPath::new(self.stream, self, _root)?);
        self.result = Box::new(NixDaemonProtocol__BuildResult::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__KeyedBuildResult {
}
#[derive(Default)]
pub struct NixDaemonProtocol__DrvOutputEntry {
    pub outputId: Box<NixDaemonProtocol__DrvOutput>,
    pub realisation: Box<NixDaemonProtocol__Realisation>,
}

impl KaitaiStruct for NixDaemonProtocol__DrvOutputEntry {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.outputId = Box::new(NixDaemonProtocol__DrvOutput::new(self.stream, self, _root)?);
        self.realisation = Box::new(NixDaemonProtocol__Realisation::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__DrvOutputEntry {
}

/*
 * Empty string means None
 */
#[derive(Default)]
pub struct NixDaemonProtocol__OptionalStorePath {
    pub path: Box<NixDaemonProtocol__NixString>,
    pub isPresent: Option<bool>,
}

impl KaitaiStruct for NixDaemonProtocol__OptionalStorePath {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.path = Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__OptionalStorePath {
    fn isPresent(&mut self) -> bool {
        if let Some(x) = self.isPresent {
            return x;
        }

        self.isPresent = self.path.len > 0;
        return self.isPresent;
    }
}
#[derive(Default)]
pub struct NixDaemonProtocol__BuildResult {
    pub status: Box<NixDaemonProtocol__BuildStatus>,
    pub errorMsg: Box<NixDaemonProtocol__NixString>,
    pub timesBuilt: u64,
    pub isNonDeterministic: u64,
    pub startTime: u64,
    pub stopTime: u64,
    pub cpuUser: Box<NixDaemonProtocol__OptionalDuration>,
    pub cpuSystem: Box<NixDaemonProtocol__OptionalDuration>,
    pub builtOutputs: Box<NixDaemonProtocol__DrvOutputs>,
}

impl KaitaiStruct for NixDaemonProtocol__BuildResult {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.status = self.stream.read_u8le()?;
        self.errorMsg = Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?);
        if self._root.protocol_version >= 29 {
            self.timesBuilt = self.stream.read_u8le()?;
        }
        if self._root.protocol_version >= 29 {
            self.isNonDeterministic = self.stream.read_u8le()?;
        }
        if self._root.protocol_version >= 29 {
            self.startTime = self.stream.read_u8le()?;
        }
        if self._root.protocol_version >= 29 {
            self.stopTime = self.stream.read_u8le()?;
        }
        if self._root.protocol_version >= 37 {
            self.cpuUser = Box::new(NixDaemonProtocol__OptionalDuration::new(self.stream, self, _root)?);
        }
        if self._root.protocol_version >= 37 {
            self.cpuSystem = Box::new(NixDaemonProtocol__OptionalDuration::new(self.stream, self, _root)?);
        }
        if self._root.protocol_version >= 28 {
            self.builtOutputs = Box::new(NixDaemonProtocol__DrvOutputs::new(self.stream, self, _root)?);
        }
    }
}

impl NixDaemonProtocol__BuildResult {
}

/*
 * Daemon requesting data from client (for streaming uploads)
 */
#[derive(Default)]
pub struct NixDaemonProtocol__StderrRead {
    pub len: u64,
}

impl KaitaiStruct for NixDaemonProtocol__StderrRead {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.len = self.stream.read_u8le()?;
    }
}

impl NixDaemonProtocol__StderrRead {
}
#[derive(Default)]
pub struct NixDaemonProtocol__QueryPathHashRequest {
    pub path: Box<NixDaemonProtocol__StorePath>,
}

impl KaitaiStruct for NixDaemonProtocol__QueryPathHashRequest {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.path = Box::new(NixDaemonProtocol__StorePath::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__QueryPathHashRequest {
}
#[derive(Default)]
pub struct NixDaemonProtocol__BoolResponse {
    pub value: u64,
}

impl KaitaiStruct for NixDaemonProtocol__BoolResponse {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.value = self.stream.read_u8le()?;
    }
}

impl NixDaemonProtocol__BoolResponse {
}
#[derive(Default)]
pub struct NixDaemonProtocol__BuildPathsRequest {
    pub paths: Box<NixDaemonProtocol__DerivedPathList>,
    pub buildMode: Box<NixDaemonProtocol__BuildMode>,
}

impl KaitaiStruct for NixDaemonProtocol__BuildPathsRequest {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.paths = Box::new(NixDaemonProtocol__DerivedPathList::new(self.stream, self, _root)?);
        self.buildMode = self.stream.read_u1()?;
    }
}

impl NixDaemonProtocol__BuildPathsRequest {
}

/*
 * End of request processing (success)
 */
#[derive(Default)]
pub struct NixDaemonProtocol__StderrLast {
}

impl KaitaiStruct for NixDaemonProtocol__StderrLast {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
    }
}

impl NixDaemonProtocol__StderrLast {
}
#[derive(Default)]
pub struct NixDaemonProtocol__AddMultipleToStoreRequest {
    pub repair: u64,
    pub dontCheckSigs: u64,
}

impl KaitaiStruct for NixDaemonProtocol__AddMultipleToStoreRequest {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.repair = self.stream.read_u8le()?;
        self.dontCheckSigs = self.stream.read_u8le()?;
    }
}

impl NixDaemonProtocol__AddMultipleToStoreRequest {
}

/*
 * Map from StorePath to optional ContentAddress (protocol >= 1.22)
 */
#[derive(Default)]
pub struct NixDaemonProtocol__StorePathCaMap {
    pub numEntries: u64,
    pub entries: Vec<Box<NixDaemonProtocol__StorePathCaEntry>>,
}

impl KaitaiStruct for NixDaemonProtocol__StorePathCaMap {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.numEntries = self.stream.read_u8le()?;
        self.entries = vec!();
        for i in 0..self.num_entries {
            self.entries.append(Box::new(NixDaemonProtocol__StorePathCaEntry::new(self.stream, self, _root)?));
        }
    }
}

impl NixDaemonProtocol__StorePathCaMap {
}
#[derive(Default)]
pub struct NixDaemonProtocol__QuerySubstitutablePathInfosRequest {
    pub paths: Box<NixDaemonProtocol__StorePathSet>,
    pub pathsWithCa: Box<NixDaemonProtocol__StorePathCaMap>,
}

impl KaitaiStruct for NixDaemonProtocol__QuerySubstitutablePathInfosRequest {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        if self._root.protocol_version < 22 {
            self.paths = Box::new(NixDaemonProtocol__StorePathSet::new(self.stream, self, _root)?);
        }
        if self._root.protocol_version >= 22 {
            self.pathsWithCa = Box::new(NixDaemonProtocol__StorePathCaMap::new(self.stream, self, _root)?);
        }
    }
}

impl NixDaemonProtocol__QuerySubstitutablePathInfosRequest {
}
#[derive(Default)]
pub struct NixDaemonProtocol__AddToStoreNarRequest {
    pub path: Box<NixDaemonProtocol__StorePath>,
    pub deriver: Box<NixDaemonProtocol__OptionalStorePath>,
    pub narHash: Box<NixDaemonProtocol__NixString>,
    pub refs: Box<NixDaemonProtocol__StorePathSet>,
    pub registrationTime: u64,
    pub narSize: u64,
    pub ultimate: u64,
    pub sigs: Box<NixDaemonProtocol__NixStringSet>,
    pub ca: Box<NixDaemonProtocol__NixString>,
    pub repair: u64,
    pub dontCheckSigs: u64,
}

impl KaitaiStruct for NixDaemonProtocol__AddToStoreNarRequest {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.path = Box::new(NixDaemonProtocol__StorePath::new(self.stream, self, _root)?);
        self.deriver = Box::new(NixDaemonProtocol__OptionalStorePath::new(self.stream, self, _root)?);
        self.narHash = Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?);
        self.refs = Box::new(NixDaemonProtocol__StorePathSet::new(self.stream, self, _root)?);
        self.registrationTime = self.stream.read_u8le()?;
        self.narSize = self.stream.read_u8le()?;
        self.ultimate = self.stream.read_u8le()?;
        self.sigs = Box::new(NixDaemonProtocol__NixStringSet::new(self.stream, self, _root)?);
        self.ca = Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?);
        self.repair = self.stream.read_u8le()?;
        self.dontCheckSigs = self.stream.read_u8le()?;
    }
}

impl NixDaemonProtocol__AddToStoreNarRequest {

    /*
     * SHA256 in base16
     */

    /*
     * Content address, empty = none
     */
}
#[derive(Default)]
pub struct NixDaemonProtocol__RegisterDrvOutputRequest {
    pub outputId: Box<NixDaemonProtocol__DrvOutput>,
    pub outputPath: Box<NixDaemonProtocol__NixString>,
    pub realisation: Box<NixDaemonProtocol__Realisation>,
}

impl KaitaiStruct for NixDaemonProtocol__RegisterDrvOutputRequest {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        if self._root.protocol_version < 31 {
            self.outputId = Box::new(NixDaemonProtocol__DrvOutput::new(self.stream, self, _root)?);
        }
        if self._root.protocol_version < 31 {
            self.outputPath = Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?);
        }
        if self._root.protocol_version >= 31 {
            self.realisation = Box::new(NixDaemonProtocol__Realisation::new(self.stream, self, _root)?);
        }
    }
}

impl NixDaemonProtocol__RegisterDrvOutputRequest {
}
#[derive(Default)]
pub struct NixDaemonProtocol__AddIndirectRootRequest {
    pub path: Box<NixDaemonProtocol__NixString>,
}

impl KaitaiStruct for NixDaemonProtocol__AddIndirectRootRequest {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.path = Box::new(NixDaemonProtocol__NixString::new(self.stream, self, _root)?);
    }
}

impl NixDaemonProtocol__AddIndirectRootRequest {

    /*
     * Absolute filesystem path (not store path)
     */
}
#[derive(Default)]
pub struct NixDaemonProtocol__DrvOutputs {
    pub numOutputs: u64,
    pub outputs: Vec<Box<NixDaemonProtocol__DrvOutputEntry>>,
}

impl KaitaiStruct for NixDaemonProtocol__DrvOutputs {
    fn new<S: KaitaiStream>(stream: &mut S,
                            _parent: &Option<Box<KaitaiStruct>>,
                            _root: &Option<Box<KaitaiStruct>>)
                            -> Result<Self>
        where Self: Sized {
        let mut s: Self = Default::default();

        s.stream = stream;
        s.read(stream, _parent, _root)?;

        Ok(s)
    }


    fn read<S: KaitaiStream>(&mut self,
                             stream: &mut S,
                             _parent: &Option<Box<KaitaiStruct>>,
                             _root: &Option<Box<KaitaiStruct>>)
                             -> Result<()>
        where Self: Sized {
        self.numOutputs = self.stream.read_u8le()?;
        self.outputs = vec!();
        for i in 0..self.num_outputs {
            self.outputs.append(Box::new(NixDaemonProtocol__DrvOutputEntry::new(self.stream, self, _root)?));
        }
    }
}

impl NixDaemonProtocol__DrvOutputs {
}
