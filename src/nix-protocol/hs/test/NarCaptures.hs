-- SPDX-License-Identifier: MIT
{-# LANGUAGE OverloadedStrings #-}

-- | Test Haskell NAR serializers against captured binary data

module Main (main) where

import Data.ByteString (ByteString)
import Control.Monad (forM_)
import qualified Data.ByteString as BS
import qualified Data.Text as T
import Nix.Nar hiding (symlink, file, executable, emptyDir, directory)
import System.FilePath ((</>))
import Test.Tasty
import Test.Tasty.HUnit

-- | Directory containing NAR captures (relative to test execution)
narCapturesDir :: FilePath
narCapturesDir = "nar_captures"

readCapture :: FilePath -> IO ByteString
readCapture name = BS.readFile (narCapturesDir </> name)

-- =============================================================================
-- Tests against captured NARs
-- =============================================================================

main :: IO ()
main = defaultMain tests

tests :: TestTree
tests =
  testGroup
    "NAR Captures"
    [ testRegularFile,
      testExecutableFile,
      testSymlinkNar,
      testEmptyFile,
      testEmptyDirectory,
      testDirectory,
      testProperties,
      testParsing
    ]

testParsing :: TestTree
testParsing =
  testGroup
    "Parsing"
    [ testParseRegularFile,
      testParseExecutableFile,
      testParseSymlink,
      testParseEmptyFile,
      testParseEmptyDirectory,
      testParseDirectory,
      testRoundtripAll
    ]

testRegularFile :: TestTree
testRegularFile = testCase "regular_file" $ do
  -- Captured: echo "hello world" > file && nix-store --dump file
  -- Content is "hello world\n" (12 bytes including newline)
  let generated = execWriter $ dumpString "hello world\n"
  expected <- readCapture "regular_file.nar"
  generated @?= expected

testExecutableFile :: TestTree
testExecutableFile = testCase "executable_file" $ do
  -- Captured: echo "#!/bin/bash" > script && chmod +x script && nix-store --dump script
  -- Content is "#!/bin/bash\n" (12 bytes)
  let generated = execWriter $ dumpExecutable "#!/bin/bash\n"
  expected <- readCapture "executable_file.nar"
  generated @?= expected

testSymlinkNar :: TestTree
testSymlinkNar = testCase "symlink" $ do
  -- Captured: ln -s /etc/passwd link && nix-store --dump link
  let generated = execWriter $ dumpSymlink (T.pack "/etc/passwd")
  expected <- readCapture "symlink.nar"
  generated @?= expected

testEmptyFile :: TestTree
testEmptyFile = testCase "empty_file" $ do
  -- Captured: touch empty && nix-store --dump empty
  let generated = execWriter $ dumpString ""
  expected <- readCapture "empty_file.nar"
  generated @?= expected

testEmptyDirectory :: TestTree
testEmptyDirectory = testCase "empty_directory" $ do
  -- Captured: mkdir empty_dir && nix-store --dump empty_dir
  let generated = execWriter dumpEmptyDirectory
  expected <- readCapture "empty_directory.nar"
  generated @?= expected

testDirectory :: TestTree
testDirectory = testCase "directory" $ do
  -- Captured:
  --   mkdir -p dir/subdir
  --   echo "file1" > dir/a.txt
  --   echo "file2" > dir/b.txt
  --   echo "nested" > dir/subdir/c.txt
  --   nix-store --dump dir
  let tree =
        Directory
          [ (T.pack "a.txt", RegularFile "file1\n" False),
            (T.pack "b.txt", RegularFile "file2\n" False),
            ( T.pack "subdir",
              Directory
                [(T.pack "c.txt", RegularFile "nested\n" False)]
            )
          ]
      generated = execWriter $ toNar tree
  expected <- readCapture "directory.nar"
  generated @?= expected

-- =============================================================================
-- Property tests
-- =============================================================================

testProperties :: TestTree
testProperties =
  testGroup
    "Properties"
    [ test8ByteAlignment,
      testDeterminism,
      testDirectorySorting
    ]

test8ByteAlignment :: TestTree
test8ByteAlignment = testCase "8-byte alignment" $ do
  -- Test various content sizes
  let testLen n =
        let content = BS.replicate n (toEnum (fromEnum 'x'))
            generated = execWriter $ dumpString content
         in BS.length generated `mod` 8 @?= 0
  mapM_ testLen [0 .. 50]

testDeterminism :: TestTree
testDeterminism = testCase "determinism" $ do
  -- Same content should always produce identical NAR
  let content = "determinism test"
      generated1 = execWriter $ dumpString content
      generated2 = execWriter $ dumpString content
  generated1 @?= generated2

testDirectorySorting :: TestTree
testDirectorySorting = testCase "directory entry sorting" $ do
  -- Entries should be sorted regardless of input order
  let tree1 =
        Directory
          [ ("z", RegularFile "z" False),
            ("a", RegularFile "a" False),
            ("m", RegularFile "m" False)
          ]
      tree2 =
        Directory
          [ ("a", RegularFile "a" False),
            ("m", RegularFile "m" False),
            ("z", RegularFile "z" False)
          ]
      generated1 = execWriter $ toNar tree1
      generated2 = execWriter $ toNar tree2
  generated1 @?= generated2

-- =============================================================================
-- Round-trip tests (parse captured NAR -> serialize -> compare)
-- =============================================================================

testParseRegularFile :: TestTree
testParseRegularFile = testCase "parse regular_file" $ do
  nar <- readCapture "regular_file.nar"
  let parsed = parseNar nar
  parsed @?= Right (RegularFile "hello world\n" False)

testParseExecutableFile :: TestTree
testParseExecutableFile = testCase "parse executable_file" $ do
  nar <- readCapture "executable_file.nar"
  let parsed = parseNar nar
  parsed @?= Right (RegularFile "#!/bin/bash\n" True)

testParseSymlink :: TestTree
testParseSymlink = testCase "parse symlink" $ do
  nar <- readCapture "symlink.nar"
  let parsed = parseNar nar
  parsed @?= Right (Symlink "/etc/passwd")

testParseEmptyFile :: TestTree
testParseEmptyFile = testCase "parse empty_file" $ do
  nar <- readCapture "empty_file.nar"
  let parsed = parseNar nar
  parsed @?= Right (RegularFile "" False)

testParseEmptyDirectory :: TestTree
testParseEmptyDirectory = testCase "parse empty_directory" $ do
  nar <- readCapture "empty_directory.nar"
  let parsed = parseNar nar
  parsed @?= Right (Directory [])

testParseDirectory :: TestTree
testParseDirectory = testCase "parse directory" $ do
  nar <- readCapture "directory.nar"
  let parsed = parseNar nar
      expected = Directory
        [ ("a.txt", RegularFile "file1\n" False)
        , ("b.txt", RegularFile "file2\n" False)
        , ("subdir", Directory
            [ ("c.txt", RegularFile "nested\n" False)
            ])
        ]
  parsed @?= Right expected

testRoundtripAll :: TestTree
testRoundtripAll = testCase "roundtrip all captures" $ do
  -- For each capture, parse it, serialize back, and ensure byte-identical
  let files = ["regular_file.nar", "executable_file.nar", "symlink.nar",
               "empty_file.nar", "empty_directory.nar", "directory.nar"]
  forM_ files $ \name -> do
    original <- readCapture name
    case parseNar original of
      Left err -> assertFailure $ "Failed to parse " ++ name ++ ": " ++ show err
      Right obj -> do
        let reserialized = execWriter $ toNar obj
        reserialized @?= original
