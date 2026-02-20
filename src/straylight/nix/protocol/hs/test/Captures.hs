-- SPDX-License-Identifier: MIT
-- | Test Haskell serializers against captured binary data

module Main (main) where

import Data.ByteString (ByteString)
import qualified Data.ByteString as BS
import Data.Text (Text)
import qualified Data.Text as T
import qualified Data.Text.Encoding as TE
import Data.Word (Word64)
import Nix.Protocol
import System.FilePath ((</>))
import Test.Tasty
import Test.Tasty.HUnit

-- | Directory containing captures (relative to test execution)
capturesDir :: FilePath
capturesDir = "captures"

readCapture :: FilePath -> IO ByteString
readCapture name = BS.readFile (capturesDir </> name)

-- | Parse a string from captured binary data at given offset
parseString :: ByteString -> Int -> (Text, Int)
parseString bs offset =
  let len = fromIntegral $ readWord64LE bs offset
      str = TE.decodeUtf8 $ BS.take len $ BS.drop (offset + 8) bs
      padding = (8 - (len `mod` 8)) `mod` 8
   in (str, offset + 8 + len + padding)

-- | Read a little-endian Word64 from ByteString
readWord64LE :: ByteString -> Int -> Word64
readWord64LE bs offset =
  let bytes = BS.unpack $ BS.take 8 $ BS.drop offset bs
   in foldr (\(i, b) acc -> acc + fromIntegral b * (256 ^ i)) 0 (zip [0 ..] bytes)

-- =============================================================================
-- Tests
-- =============================================================================

main :: IO ()
main = defaultMain tests

tests :: TestTree
tests =
  testGroup
    "Captures"
    [ testGroup
        "Writer"
        [ testClientHello,
          testServerHello,
          testIsValidPathRequest,
          testQueryPathInfoRequest,
          testQueryReferrersRequest,
          testAddTempRootRequest,
          testAddIndirectRootRequest,
          testFindRootsRequest,
          testNarFromPathRequest,
          testQueryMissingRequest,
          testBuildPathsRequest,
          testBuildPathsWithResultsRequest,
          testSetOptionsRequest,
          testAddToStoreNarRequest
        ],
      testGroup
        "Reader"
        [ testReadServerHello,
          testReadIsValidPathResponse,
          testReadQueryPathInfoResponse,
          testReadQueryMissingResponse,
          testReadQueryReferrersResponse,
          testReadBuildPathsWithResultsResponse
        ]
    ]

testClientHello :: TestTree
testClientHello = testCase "client_hello" $ do
  let generated = execWriter $ writeClientHello 0x0126
  expected <- readCapture "client_hello.bin"
  generated @?= expected

testServerHello :: TestTree
testServerHello = testCase "server_hello" $ do
  let generated = execWriter $ writeServerHello 0x0126
  expected <- readCapture "server_hello.bin"
  generated @?= expected

testIsValidPathRequest :: TestTree
testIsValidPathRequest = testCase "isvalidpath_request" $ do
  captured <- readCapture "isvalidpath_request.bin"
  let (path, _) = parseString captured 8
      generated = execWriter $ writeIsValidPathRequest path
  generated @?= captured

testQueryPathInfoRequest :: TestTree
testQueryPathInfoRequest = testCase "querypathinfo_request" $ do
  captured <- readCapture "querypathinfo_request.bin"
  let (path, _) = parseString captured 8
      generated = execWriter $ writeQueryPathInfoRequest path
  generated @?= captured

testQueryReferrersRequest :: TestTree
testQueryReferrersRequest = testCase "queryreferrers_request" $ do
  captured <- readCapture "queryreferrers_request.bin"
  let (path, _) = parseString captured 8
      generated = execWriter $ writeQueryReferrersRequest path
  generated @?= captured

testAddTempRootRequest :: TestTree
testAddTempRootRequest = testCase "addtemproot_request" $ do
  captured <- readCapture "addtemproot_request.bin"
  let (path, _) = parseString captured 8
      generated = execWriter $ writeAddTempRootRequest path
  generated @?= captured

testAddIndirectRootRequest :: TestTree
testAddIndirectRootRequest = testCase "addindirectroot_request" $ do
  captured <- readCapture "addindirectroot_request.bin"
  let (path, _) = parseString captured 8
      generated = execWriter $ writeAddIndirectRootRequest path
  generated @?= captured

testFindRootsRequest :: TestTree
testFindRootsRequest = testCase "findroots_request" $ do
  let generated = execWriter writeFindRootsRequest
  expected <- readCapture "findroots_request.bin"
  generated @?= expected

testNarFromPathRequest :: TestTree
testNarFromPathRequest = testCase "narfrompath_request" $ do
  captured <- readCapture "narfrompath_request.bin"
  let (path, _) = parseString captured 8
      generated = execWriter $ writeNarFromPathRequest path
  generated @?= captured

testQueryMissingRequest :: TestTree
testQueryMissingRequest = testCase "querymissing_request" $ do
  captured <- readCapture "querymissing_request.bin"
  let numPaths = fromIntegral $ readWord64LE captured 8 :: Int
      parsePaths :: Int -> Int -> [Text] -> ([Text], Int)
      parsePaths 0 offset acc = (acc, offset)
      parsePaths n offset acc =
        let (path, newOffset) = parseString captured offset
         in parsePaths (n - 1) newOffset (acc ++ [path])
      (paths, _) = parsePaths numPaths 16 []
      generated = execWriter $ writeQueryMissingRequest paths
  generated @?= captured

testBuildPathsRequest :: TestTree
testBuildPathsRequest = testCase "buildpaths_request" $ do
  captured <- readCapture "buildpaths_request.bin"
  let numPaths = fromIntegral $ readWord64LE captured 8
      parsePaths 0 offset acc = (acc, offset)
      parsePaths n offset acc =
        let (path, newOffset) = parseString captured offset
         in parsePaths (n - 1) newOffset (acc ++ [path])
      (paths, modeOffset) = parsePaths numPaths 16 []
      modeVal = readWord64LE captured modeOffset
      mode = case modeVal of
        0 -> Normal
        1 -> Repair
        2 -> Check
        _ -> error "Unknown build mode"
      generated = execWriter $ writeBuildPathsRequest paths mode
  generated @?= captured

testBuildPathsWithResultsRequest :: TestTree
testBuildPathsWithResultsRequest = testCase "buildpathswithresults_request" $ do
  captured <- readCapture "buildpathswithresults_request.bin"
  let numPaths = fromIntegral $ readWord64LE captured 8
      parsePaths 0 offset acc = (acc, offset)
      parsePaths n offset acc =
        let (path, newOffset) = parseString captured offset
         in parsePaths (n - 1) newOffset (acc ++ [path])
      (paths, modeOffset) = parsePaths numPaths 16 []
      modeVal = readWord64LE captured modeOffset
      mode = case modeVal of
        0 -> Normal
        1 -> Repair
        2 -> Check
        _ -> error "Unknown build mode"
      generated = execWriter $ writeBuildPathsWithResultsRequest paths mode
  generated @?= captured

testSetOptionsRequest :: TestTree
testSetOptionsRequest = testCase "setoptions_request" $ do
  let settings =
        ClientSettings
          { csKeepFailed = False,
            csKeepGoing = False,
            csTryFallback = False,
            csVerbosity = 3,
            csMaxBuildJobs = 32,
            csMaxSilentTime = 0,
            csUseBuildHook = True,
            csVerboseBuild = 7,
            csLogType = 0,
            csPrintBuildTrace = 0,
            csBuildCores = 0,
            csUseSubstitutes = True,
            csOverrides =
              [ (T.pack "extra-platforms", T.pack "aarch64-linux"),
                (T.pack "sandbox", T.pack "false"),
                (T.pack "substituters", T.pack "https://cache.nixos.org https://weyl-ai.cachix.org"),
                ( T.pack "trusted-public-keys",
                  T.pack $
                    "cache.nixos.org-1:6NCHdD59X431o0gWypbMrAURkbJ16ZPMQFGspcDShjY= "
                      ++ "weyl-ai.cachix.org-1:cR0SpSAPw7wejZ21ep4SLojE77gp5F2os260eEWqTTw="
                )
              ]
          }
      generated = execWriter $ writeSetOptionsRequest settings 38
  expected <- readCapture "setoptions_request.bin"
  generated @?= expected

testAddToStoreNarRequest :: TestTree
testAddToStoreNarRequest = testCase "addtostorenar_request" $ do
  let req =
        AddToStoreNarRequest
          { asnPath = T.pack "/nix/store/v4wqkf7dq9619yyv1wbf1jvpw4dhbs2f-tf.txt",
            asnDeriver = T.empty,
            asnNarHash = T.pack "60e5106c2a1d4ef9f82b58df02be7a482f35438ff9e6556194ab02e355256352",
            asnReferences = [],
            asnRegistrationTime = 0,
            asnNarSize = 136,
            asnUltimate = False,
            asnSignatures = [],
            asnCa = T.pack "fixed:sha256:081b9nklhcc0gck2a6j821xij57djvnfxg1fps0f5l2bbpd1sgp1",
            asnRepair = False,
            asnDontCheckSigs = False
          }
      generated = execWriter $ writeAddToStoreNarRequest req
  expected <- readCapture "addtostorenar_request.bin"
  generated @?= expected

-- =============================================================================
-- Reader Tests
-- =============================================================================

testReadServerHello :: TestTree
testReadServerHello = testCase "read_server_hello" $ do
  captured <- readCapture "server_hello.bin"
  case runReader readServerHello captured of
    Left err -> assertFailure $ "Parse failed: " ++ err
    Right hello -> do
      shMagic hello @?= workerMagic2
      shVersion hello @?= 0x0126

testReadIsValidPathResponse :: TestTree
testReadIsValidPathResponse = testCase "read_isvalidpath_response" $ do
  captured <- readCapture "isvalidpath_response.bin"
  case runReader readIsValidPathResponse captured of
    Left err -> assertFailure $ "Parse failed: " ++ err
    Right valid -> valid @?= True

testReadQueryPathInfoResponse :: TestTree
testReadQueryPathInfoResponse = testCase "read_querypathinfo_response" $ do
  captured <- readCapture "querypathinfo_response.bin"
  case runReader (readQueryPathInfoResponse 0x0126) captured of
    Left err -> assertFailure $ "Parse failed: " ++ err
    Right Nothing -> assertFailure "Expected Some, got Nothing"
    Right (Just info) -> do
      assertBool "deriver contains bash" $ T.isInfixOf (T.pack "bash") (vpiDeriver info)
      assertBool "nar_hash starts with f7b02ee0" $ T.isPrefixOf (T.pack "f7b02ee0") (vpiNarHash info)
      length (vpiReferences info) @?= 2
      length (vpiSignatures info) @?= 1

testReadQueryMissingResponse :: TestTree
testReadQueryMissingResponse = testCase "read_querymissing_response" $ do
  captured <- readCapture "querymissing_response.bin"
  case runReader readQueryMissingResponse captured of
    Left err -> assertFailure $ "Parse failed: " ++ err
    Right result -> do
      null (qmrWillBuild result) @?= True
      null (qmrWillSubstitute result) @?= True
      qmrDownloadSize result @?= 0
      qmrNarSize result @?= 0

testReadQueryReferrersResponse :: TestTree
testReadQueryReferrersResponse = testCase "read_queryreferrers_response" $ do
  captured <- readCapture "queryreferrers_response.bin"
  case runReader readQueryReferrersResponse captured of
    Left err -> assertFailure $ "Parse failed: " ++ err
    Right refs -> do
      assertBool "not empty" $ not (null refs)
      assertBool "first is store path" $ T.isPrefixOf (T.pack "/nix/store/") (head refs)

testReadBuildPathsWithResultsResponse :: TestTree
testReadBuildPathsWithResultsResponse = testCase "read_buildpathswithresults_response" $ do
  captured <- readCapture "buildpathswithresults_response.bin"
  case runReader (readBuildPathsWithResultsResponse 0x0126) captured of
    Left err -> assertFailure $ "Parse failed: " ++ err
    Right results -> do
      length results @?= 1
      let result = head results
      assertBool "path contains hello" $ T.isInfixOf (T.pack "hello") (brpPath result)
      brpStatus result @?= 2
      length (brpBuiltOutputs result) @?= 1
