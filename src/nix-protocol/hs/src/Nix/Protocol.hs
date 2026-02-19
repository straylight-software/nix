-- SPDX-License-Identifier: MIT
{-# LANGUAGE LambdaCase #-}

-- |
-- Module      : Nix.Protocol
-- Description : Nix daemon protocol serialization
--
-- Wire format primitives:
--
-- * Integers: little-endian u64
-- * Strings: u64 length + bytes + padding to 8-byte boundary
-- * Booleans: u64 (0 = false, nonzero = true)
-- * Lists: u64 count + elements
module Nix.Protocol
  ( -- * Protocol constants
    workerMagic1,
    workerMagic2,
    stderrLast,
    stderrError,

    -- * Operations
    Op (..),
    BuildMode (..),

    -- * Writer
    Writer,
    runWriter,
    execWriter,

    -- * Write primitives
    writeU64,
    writeBool,
    writeBytes,
    writeString,
    writeStringList,
    writeStringSet,
    writeStorePath,
    writeStorePathSet,
    writeDerivedPath,
    writeDerivedPathList,

    -- * Handshake (write)
    writeClientHello,
    writeServerHello,

    -- * Requests
    writeIsValidPathRequest,
    writeQueryPathInfoRequest,
    writeQueryReferrersRequest,
    writeAddTempRootRequest,
    writeAddIndirectRootRequest,
    writeFindRootsRequest,
    writeNarFromPathRequest,
    writeQueryMissingRequest,
    writeBuildPathsRequest,
    writeBuildPathsWithResultsRequest,
    writeSetOptionsRequest,
    writeAddToStoreNarRequest,

    -- * Settings
    ClientSettings (..),
    defaultClientSettings,
    AddToStoreNarRequest (..),

    -- * Reader
    Reader,
    runReader,

    -- * Read primitives
    readU64,
    readBool,
    readBytes,
    readString,
    readStringList,
    readStringSet,
    readStorePath,
    readStorePathSet,

    -- * Response parsing
    ServerHello (..),
    readServerHello,
    readIsValidPathResponse,
    ValidPathInfo (..),
    readQueryPathInfoResponse,
    QueryMissingResult (..),
    readQueryMissingResponse,
    readQueryReferrersResponse,
    GcRoot (..),
    readFindRootsResponse,
    Realisation (..),
    BuildResultWithPath (..),
    readBuildPathsWithResultsResponse,
  )
where

import Control.Monad (replicateM)
import Data.Bits ((.&.))
import Data.ByteString (ByteString)
import qualified Data.ByteString as BS
import Data.ByteString.Builder (Builder)
import qualified Data.ByteString.Builder as B
import qualified Data.ByteString.Lazy as LBS
import Data.List (sort)
import Data.Text (Text)
import qualified Data.Text as T
import qualified Data.Text.Encoding as TE
import Data.Word (Word64)

-- =============================================================================
-- Protocol Constants
-- =============================================================================

-- | Client -> Server magic "nixc"
workerMagic1 :: Word64
workerMagic1 = 0x6e697863

-- | Server -> Client magic "dxio"
workerMagic2 :: Word64
workerMagic2 = 0x6478696f

stderrNext, stderrRead, stderrWrite, stderrLast, stderrError :: Word64
stderrNext = 0x6f6c6d67
stderrRead = 0x64617461
stderrWrite = 0x64617416
stderrLast = 0x616c7473
stderrError = 0x63787470

-- | Protocol operations
data Op
  = OpIsValidPath
  | OpHasSubstitutes
  | OpQueryReferrers
  | OpAddToStore
  | OpBuildPaths
  | OpEnsurePath
  | OpAddTempRoot
  | OpAddIndirectRoot
  | OpSyncWithGC
  | OpFindRoots
  | OpSetOptions
  | OpCollectGarbage
  | OpQuerySubstitutablePathInfo
  | OpQueryAllValidPaths
  | OpQueryPathInfo
  | OpQueryPathFromHashPart
  | OpQuerySubstitutablePathInfos
  | OpQueryValidPaths
  | OpQuerySubstitutablePaths
  | OpQueryValidDerivers
  | OpOptimiseStore
  | OpVerifyStore
  | OpBuildDerivation
  | OpAddSignatures
  | OpNarFromPath
  | OpAddToStoreNar
  | OpQueryMissing
  | OpQueryDerivationOutputMap
  | OpRegisterDrvOutput
  | OpQueryRealisation
  | OpAddMultipleToStore
  | OpAddBuildLog
  | OpBuildPathsWithResults
  | OpAddPermRoot
  | OpQueryActiveBuilds
  deriving (Show, Eq)

opToWord64 :: Op -> Word64
opToWord64 = \case
  OpIsValidPath -> 1
  OpHasSubstitutes -> 3
  OpQueryReferrers -> 6
  OpAddToStore -> 7
  OpBuildPaths -> 9
  OpEnsurePath -> 10
  OpAddTempRoot -> 11
  OpAddIndirectRoot -> 12
  OpSyncWithGC -> 13
  OpFindRoots -> 14
  OpSetOptions -> 19
  OpCollectGarbage -> 20
  OpQuerySubstitutablePathInfo -> 21
  OpQueryAllValidPaths -> 23
  OpQueryPathInfo -> 26
  OpQueryPathFromHashPart -> 29
  OpQuerySubstitutablePathInfos -> 30
  OpQueryValidPaths -> 31
  OpQuerySubstitutablePaths -> 32
  OpQueryValidDerivers -> 33
  OpOptimiseStore -> 34
  OpVerifyStore -> 35
  OpBuildDerivation -> 36
  OpAddSignatures -> 37
  OpNarFromPath -> 38
  OpAddToStoreNar -> 39
  OpQueryMissing -> 40
  OpQueryDerivationOutputMap -> 41
  OpRegisterDrvOutput -> 42
  OpQueryRealisation -> 43
  OpAddMultipleToStore -> 44
  OpAddBuildLog -> 45
  OpBuildPathsWithResults -> 46
  OpAddPermRoot -> 47
  OpQueryActiveBuilds -> 48

-- | Build mode
data BuildMode = Normal | Repair | Check
  deriving (Show, Eq)

buildModeToWord64 :: BuildMode -> Word64
buildModeToWord64 Normal = 0
buildModeToWord64 Repair = 1
buildModeToWord64 Check = 2

-- =============================================================================
-- Writer
-- =============================================================================

-- | Writer monad for building protocol messages
newtype Writer a = Writer {unWriter :: Builder -> (a, Builder)}
  deriving (Functor)

instance Applicative Writer where
  pure a = Writer $ \b -> (a, b)
  Writer f <*> Writer x = Writer $ \b ->
    let (g, b') = f b
        (a, b'') = x b'
     in (g a, b'')

instance Monad Writer where
  Writer m >>= k = Writer $ \b ->
    let (a, b') = m b
     in unWriter (k a) b'

-- | Run a writer and get the result and ByteString
runWriter :: Writer a -> (a, ByteString)
runWriter (Writer f) =
  let (a, builder) = f mempty
   in (a, LBS.toStrict $ B.toLazyByteString builder)

-- | Execute a writer and get just the ByteString
execWriter :: Writer () -> ByteString
execWriter = snd . runWriter

-- | Emit bytes directly
emit :: Builder -> Writer ()
emit b = Writer $ \acc -> ((), acc <> b)

-- =============================================================================
-- Primitives
-- =============================================================================

-- | Write a u64 in little-endian format
writeU64 :: Word64 -> Writer ()
writeU64 = emit . B.word64LE

-- | Write a boolean as u64
writeBool :: Bool -> Writer ()
writeBool b = writeU64 (if b then 1 else 0)

-- | Write bytes with length prefix and padding
writeBytes :: ByteString -> Writer ()
writeBytes bs = do
  writeU64 (fromIntegral $ BS.length bs)
  emit (B.byteString bs)
  writePadding (BS.length bs)

-- | Write padding to align to 8 bytes
writePadding :: Int -> Writer ()
writePadding len =
  let pad = (8 - (len .&. 7)) .&. 7
   in emit (B.byteString $ BS.replicate pad 0)

-- | Write a text string
writeString :: Text -> Writer ()
writeString = writeBytes . TE.encodeUtf8

-- | Write a list of strings
writeStringList :: [Text] -> Writer ()
writeStringList xs = do
  writeU64 (fromIntegral $ length xs)
  mapM_ writeString xs

-- | Write a sorted set of strings
writeStringSet :: [Text] -> Writer ()
writeStringSet xs = do
  let sorted = sort xs
  writeU64 (fromIntegral $ length sorted)
  mapM_ writeString sorted

-- | Write a store path
writeStorePath :: Text -> Writer ()
writeStorePath = writeString

-- | Write a sorted set of store paths
writeStorePathSet :: [Text] -> Writer ()
writeStorePathSet = writeStringSet

-- | Write a derived path
writeDerivedPath :: Text -> Writer ()
writeDerivedPath = writeString

-- | Write a list of derived paths
writeDerivedPathList :: [Text] -> Writer ()
writeDerivedPathList paths = do
  writeU64 (fromIntegral $ length paths)
  mapM_ writeDerivedPath paths

-- | Write an operation code
writeOp :: Op -> Writer ()
writeOp = writeU64 . opToWord64

-- =============================================================================
-- Handshake
-- =============================================================================

-- | Write client hello message
writeClientHello :: Word64 -> Writer ()
writeClientHello version = do
  writeU64 workerMagic1
  writeU64 version

-- | Write server hello message
writeServerHello :: Word64 -> Writer ()
writeServerHello version = do
  writeU64 workerMagic2
  writeU64 version

-- =============================================================================
-- Requests
-- =============================================================================

-- | IsValidPath request
writeIsValidPathRequest :: Text -> Writer ()
writeIsValidPathRequest path = do
  writeOp OpIsValidPath
  writeStorePath path

-- | QueryPathInfo request
writeQueryPathInfoRequest :: Text -> Writer ()
writeQueryPathInfoRequest path = do
  writeOp OpQueryPathInfo
  writeStorePath path

-- | QueryReferrers request
writeQueryReferrersRequest :: Text -> Writer ()
writeQueryReferrersRequest path = do
  writeOp OpQueryReferrers
  writeStorePath path

-- | AddTempRoot request
writeAddTempRootRequest :: Text -> Writer ()
writeAddTempRootRequest path = do
  writeOp OpAddTempRoot
  writeStorePath path

-- | AddIndirectRoot request
writeAddIndirectRootRequest :: Text -> Writer ()
writeAddIndirectRootRequest path = do
  writeOp OpAddIndirectRoot
  writeString path

-- | FindRoots request
writeFindRootsRequest :: Writer ()
writeFindRootsRequest = writeOp OpFindRoots

-- | NarFromPath request
writeNarFromPathRequest :: Text -> Writer ()
writeNarFromPathRequest path = do
  writeOp OpNarFromPath
  writeStorePath path

-- | QueryMissing request
writeQueryMissingRequest :: [Text] -> Writer ()
writeQueryMissingRequest targets = do
  writeOp OpQueryMissing
  writeDerivedPathList targets

-- | BuildPaths request
writeBuildPathsRequest :: [Text] -> BuildMode -> Writer ()
writeBuildPathsRequest paths mode = do
  writeOp OpBuildPaths
  writeDerivedPathList paths
  writeU64 (buildModeToWord64 mode)

-- | BuildPathsWithResults request
writeBuildPathsWithResultsRequest :: [Text] -> BuildMode -> Writer ()
writeBuildPathsWithResultsRequest paths mode = do
  writeOp OpBuildPathsWithResults
  writeDerivedPathList paths
  writeU64 (buildModeToWord64 mode)

-- =============================================================================
-- SetOptions
-- =============================================================================

-- | Client settings for SetOptions
data ClientSettings = ClientSettings
  { csKeepFailed :: !Bool,
    csKeepGoing :: !Bool,
    csTryFallback :: !Bool,
    csVerbosity :: !Word64,
    csMaxBuildJobs :: !Word64,
    csMaxSilentTime :: !Word64,
    csUseBuildHook :: !Bool,
    csVerboseBuild :: !Word64,
    csLogType :: !Word64,
    csPrintBuildTrace :: !Word64,
    csBuildCores :: !Word64,
    csUseSubstitutes :: !Bool,
    csOverrides :: ![(Text, Text)]
  }
  deriving (Show, Eq)

-- | Default client settings
defaultClientSettings :: ClientSettings
defaultClientSettings =
  ClientSettings
    { csKeepFailed = False,
      csKeepGoing = False,
      csTryFallback = False,
      csVerbosity = 0,
      csMaxBuildJobs = 1,
      csMaxSilentTime = 0,
      csUseBuildHook = True,
      csVerboseBuild = 0,
      csLogType = 0,
      csPrintBuildTrace = 0,
      csBuildCores = 0,
      csUseSubstitutes = True,
      csOverrides = []
    }

-- | SetOptions request
writeSetOptionsRequest :: ClientSettings -> Word64 -> Writer ()
writeSetOptionsRequest settings protocolVersion = do
  writeOp OpSetOptions
  writeBool (csKeepFailed settings)
  writeBool (csKeepGoing settings)
  writeBool (csTryFallback settings)
  writeU64 (csVerbosity settings)
  writeU64 (csMaxBuildJobs settings)
  writeU64 (csMaxSilentTime settings)
  writeBool (csUseBuildHook settings)
  writeU64 (csVerboseBuild settings)
  writeU64 (csLogType settings)
  writeU64 (csPrintBuildTrace settings)
  writeU64 (csBuildCores settings)
  writeBool (csUseSubstitutes settings)
  when (protocolVersion >= 12) $ do
    writeU64 (fromIntegral $ length (csOverrides settings))
    mapM_ (\(k, v) -> writeString k >> writeString v) (csOverrides settings)
  where
    when True m = m
    when False _ = pure ()

-- =============================================================================
-- AddToStoreNar
-- =============================================================================

-- | AddToStoreNar request data
data AddToStoreNarRequest = AddToStoreNarRequest
  { asnPath :: !Text,
    asnDeriver :: !Text, -- empty = none
    asnNarHash :: !Text,
    asnReferences :: ![Text],
    asnRegistrationTime :: !Word64,
    asnNarSize :: !Word64,
    asnUltimate :: !Bool,
    asnSignatures :: ![Text],
    asnCa :: !Text, -- empty = none
    asnRepair :: !Bool,
    asnDontCheckSigs :: !Bool
  }
  deriving (Show, Eq)

-- | AddToStoreNar request
writeAddToStoreNarRequest :: AddToStoreNarRequest -> Writer ()
writeAddToStoreNarRequest req = do
  writeOp OpAddToStoreNar
  writeStorePath (asnPath req)
  writeString (asnDeriver req)
  writeString (asnNarHash req)
  writeStorePathSet (asnReferences req)
  writeU64 (asnRegistrationTime req)
  writeU64 (asnNarSize req)
  writeBool (asnUltimate req)
  writeStringSet (asnSignatures req)
  writeString (asnCa req)
  writeBool (asnRepair req)
  writeBool (asnDontCheckSigs req)

-- =============================================================================
-- Reader
-- =============================================================================

-- | Reader monad for parsing protocol messages
newtype Reader a = Reader {unReader :: ByteString -> Either String (a, ByteString)}
  deriving (Functor)

instance Applicative Reader where
  pure a = Reader $ \bs -> Right (a, bs)
  Reader f <*> Reader x = Reader $ \bs -> do
    (g, bs') <- f bs
    (a, bs'') <- x bs'
    pure (g a, bs'')

instance Monad Reader where
  Reader m >>= k = Reader $ \bs -> do
    (a, bs') <- m bs
    unReader (k a) bs'

-- | Run a reader and get the result
runReader :: Reader a -> ByteString -> Either String a
runReader (Reader f) bs = fst <$> f bs

-- | Fail with an error message
readerFail :: String -> Reader a
readerFail msg = Reader $ \_ -> Left msg

-- =============================================================================
-- Read Primitives
-- =============================================================================

-- | Read a u64 in little-endian format
readU64 :: Reader Word64
readU64 = Reader $ \bs ->
  if BS.length bs < 8
    then Left "unexpected end of input"
    else
      let (bytes, rest) = BS.splitAt 8 bs
          val = fromIntegral (BS.index bytes 0)
            + fromIntegral (BS.index bytes 1) * 256
            + fromIntegral (BS.index bytes 2) * 65536
            + fromIntegral (BS.index bytes 3) * 16777216
            + fromIntegral (BS.index bytes 4) * 4294967296
            + fromIntegral (BS.index bytes 5) * 1099511627776
            + fromIntegral (BS.index bytes 6) * 281474976710656
            + fromIntegral (BS.index bytes 7) * 72057594037927936
       in Right (val, rest)

-- | Read a boolean
readBool :: Reader Bool
readBool = (/= 0) <$> readU64

-- | Read padding bytes
readPadding :: Int -> Reader ()
readPadding len = Reader $ \bs ->
  let pad = (8 - (len .&. 7)) .&. 7
   in if BS.length bs < pad
        then Left "unexpected end of input (padding)"
        else
          let (padding, rest) = BS.splitAt pad bs
           in if BS.all (== 0) padding
                then Right ((), rest)
                else Left "non-zero padding bytes"

-- | Read bytes with length prefix and padding
readBytes :: Reader ByteString
readBytes = do
  len <- readU64
  Reader $ \bs ->
    let len' = fromIntegral len
     in if BS.length bs < len'
          then Left "unexpected end of input"
          else
            let (bytes, rest) = BS.splitAt len' bs
             in Right (bytes, rest)
  >>= \bytes -> do
    readPadding (BS.length bytes)
    pure bytes

-- | Read a text string
readString :: Reader Text
readString = do
  bytes <- readBytes
  case TE.decodeUtf8' bytes of
    Left _ -> readerFail "invalid UTF-8"
    Right t -> pure t

-- | Read a list of strings
readStringList :: Reader [Text]
readStringList = do
  count <- readU64
  replicateM (fromIntegral count) readString

-- | Read a sorted set of strings (just a list on wire)
readStringSet :: Reader [Text]
readStringSet = readStringList

-- | Read a store path
readStorePath :: Reader Text
readStorePath = readString

-- | Read a set of store paths
readStorePathSet :: Reader [Text]
readStorePathSet = readStringList

-- =============================================================================
-- Response Parsing
-- =============================================================================

-- | Server hello response
data ServerHello = ServerHello
  { shMagic :: !Word64,
    shVersion :: !Word64
  }
  deriving (Show, Eq)

-- | Read server hello
readServerHello :: Reader ServerHello
readServerHello = do
  magic <- readU64
  if magic /= workerMagic2
    then readerFail $ "invalid magic: " ++ show magic
    else do
      version <- readU64
      pure $ ServerHello magic version

-- | Read IsValidPath response
readIsValidPathResponse :: Reader Bool
readIsValidPathResponse = readBool

-- | Valid path info
data ValidPathInfo = ValidPathInfo
  { vpiDeriver :: !Text,
    vpiNarHash :: !Text,
    vpiReferences :: ![Text],
    vpiRegistrationTime :: !Word64,
    vpiNarSize :: !Word64,
    vpiUltimate :: !Bool,
    vpiSignatures :: ![Text],
    vpiCa :: !Text
  }
  deriving (Show, Eq)

-- | Read QueryPathInfo response
readQueryPathInfoResponse :: Word64 -> Reader (Maybe ValidPathInfo)
readQueryPathInfoResponse protocolVersion = do
  valid <- readBool
  if not valid
    then pure Nothing
    else do
      deriver <- readString
      narHash <- readString
      refs <- readStorePathSet
      regTime <- readU64
      narSize <- readU64
      (ultimate, sigs, ca) <-
        if protocolVersion >= 16
          then do
            ult <- readBool
            sigs' <- readStringSet
            ca' <- readString
            pure (ult, sigs', ca')
          else pure (False, [], T.empty)
      pure $
        Just $
          ValidPathInfo
            { vpiDeriver = deriver,
              vpiNarHash = narHash,
              vpiReferences = refs,
              vpiRegistrationTime = regTime,
              vpiNarSize = narSize,
              vpiUltimate = ultimate,
              vpiSignatures = sigs,
              vpiCa = ca
            }

-- | QueryMissing result
data QueryMissingResult = QueryMissingResult
  { qmrWillBuild :: ![Text],
    qmrWillSubstitute :: ![Text],
    qmrUnknown :: ![Text],
    qmrDownloadSize :: !Word64,
    qmrNarSize :: !Word64
  }
  deriving (Show, Eq)

-- | Read QueryMissing response
readQueryMissingResponse :: Reader QueryMissingResult
readQueryMissingResponse = do
  willBuild <- readStorePathSet
  willSub <- readStorePathSet
  unknown <- readStorePathSet
  dlSize <- readU64
  narSize <- readU64
  pure $
    QueryMissingResult
      { qmrWillBuild = willBuild,
        qmrWillSubstitute = willSub,
        qmrUnknown = unknown,
        qmrDownloadSize = dlSize,
        qmrNarSize = narSize
      }

-- | Read QueryReferrers response
readQueryReferrersResponse :: Reader [Text]
readQueryReferrersResponse = readStorePathSet

-- | GC root entry
data GcRoot = GcRoot
  { grLink :: !Text,
    grTarget :: !Text
  }
  deriving (Show, Eq)

-- | Read FindRoots response
readFindRootsResponse :: Reader [GcRoot]
readFindRootsResponse = do
  count <- readU64
  replicateM (fromIntegral count) $ do
    link <- readString
    target <- readStorePath
    pure $ GcRoot link target

-- | Realisation (from JSON)
data Realisation = Realisation
  { rId :: !Text,
    rOutPath :: !Text,
    rSignatures :: ![Text]
  }
  deriving (Show, Eq)

-- | Parse realisation from JSON string (minimal)
parseRealisationJson :: Text -> Realisation
parseRealisationJson json =
  Realisation
    { rId = extractField "\"id\":\"" json,
      rOutPath = extractField "\"outPath\":\"" json,
      rSignatures = []
    }
  where
    extractField prefix txt =
      case T.breakOn (T.pack prefix) txt of
        (_, rest)
          | T.null rest -> T.empty
          | otherwise ->
              let after = T.drop (T.length (T.pack prefix)) rest
               in T.takeWhile (/= '"') after

-- | Build result with path
data BuildResultWithPath = BuildResultWithPath
  { brpPath :: !Text,
    brpStatus :: !Word64,
    brpErrorMsg :: !Text,
    brpTimesBuilt :: !Word64,
    brpIsNonDeterministic :: !Bool,
    brpStartTime :: !Word64,
    brpStopTime :: !Word64,
    brpCpuUser :: !Word64,
    brpCpuSystem :: !Word64,
    brpBuiltOutputs :: ![(Text, Realisation)]
  }
  deriving (Show, Eq)

-- | Read BuildPathsWithResults response
readBuildPathsWithResultsResponse :: Word64 -> Reader [BuildResultWithPath]
readBuildPathsWithResultsResponse protocolVersion = do
  count <- readU64
  replicateM (fromIntegral count) $ do
    path <- readString
    status <- readU64
    errorMsg <- readString

    (timesBuilt, isNonDet) <-
      if protocolVersion >= 0x11d
        then do
          tb <- readU64
          nd <- readBool
          pure (tb, nd)
        else pure (0, False)

    (startTime, stopTime) <-
      if protocolVersion >= 0x11e
        then do
          st <- readU64
          et <- readU64
          pure (st, et)
        else pure (0, 0)

    -- CPU times (read unconditionally for high versions)
    (cpuUser, cpuSystem) <-
      if protocolVersion >= 0x11c
        then do
          cu <- readU64
          cs <- readU64
          pure (cu, cs)
        else pure (0, 0)

    -- Built outputs
    builtOutputs <-
      if protocolVersion >= 0x11c
        then do
          outCount <- readU64
          replicateM (fromIntegral outCount) $ do
            outName <- readString
            jsonStr <- readString
            pure (outName, parseRealisationJson jsonStr)
        else pure []

    pure $
      BuildResultWithPath
        { brpPath = path,
          brpStatus = status,
          brpErrorMsg = errorMsg,
          brpTimesBuilt = timesBuilt,
          brpIsNonDeterministic = isNonDet,
          brpStartTime = startTime,
          brpStopTime = stopTime,
          brpCpuUser = cpuUser,
          brpCpuSystem = cpuSystem,
          brpBuiltOutputs = builtOutputs
        }
