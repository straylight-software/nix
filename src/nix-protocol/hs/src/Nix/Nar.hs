-- SPDX-License-Identifier: MIT
{-# LANGUAGE LambdaCase #-}
{-# LANGUAGE OverloadedStrings #-}

-- |
-- Module      : Nix.Nar
-- Description : NAR (Nix Archive) format serialization
--
-- NAR is a deterministic archive format used by Nix for content-addressed storage.
--
-- Key properties:
--
-- * Deterministic: Same filesystem content always produces identical NAR
-- * Platform-independent: Portable across Unix-like systems
-- * Content-addressed: Only content matters, not metadata like timestamps
--
-- Wire format:
--
-- * Strings: u64 length + bytes + padding to 8-byte boundary
-- * Magic: @"nix-archive-1"@
-- * Node types: @regular@, @directory@, @symlink@
module Nix.Nar
  ( -- * Constants
    narVersionMagic,

    -- * Writer
    Writer,
    runWriter,
    execWriter,

    -- * Primitives
    writeU64,
    writeBytes,
    writeStr,
    writePadding,

    -- * NAR Structure
    writeMagic,
    writeOpen,
    writeClose,
    writeType,

    -- * Node serialization
    writeRegularFile,
    writeSymlink,
    beginDirectory,
    beginEntry,
    endEntry,
    endDirectory,

    -- * High-level API
    dumpString,
    dumpExecutable,
    dumpSymlink,
    dumpEmptyDirectory,

    -- * Filesystem tree
    FsObject (..),
    file,
    executable,
    symlink,
    emptyDir,
    directory,
    toNar,

    -- * Reader/Parser
    NarError (..),
    Reader,
    runReader,
    readU64,
    readBytes,
    readStr,
    readMagic,
    readNode,
    parseNar,
  )
where

import Data.Bits ((.&.))
import Data.ByteString (ByteString)
import qualified Data.ByteString as BS
import Data.ByteString.Builder (Builder)
import qualified Data.ByteString.Builder as B
import qualified Data.ByteString.Lazy as LBS
import Data.List (sortOn)
import Data.Text (Text)
import qualified Data.Text.Encoding as TE
import Data.Word (Word64)

-- =============================================================================
-- Constants
-- =============================================================================

-- | NAR version magic string
narVersionMagic :: ByteString
narVersionMagic = "nix-archive-1"

-- =============================================================================
-- Writer (reusing the same pattern as Protocol.hs)
-- =============================================================================

-- | Writer monad for building NAR archives
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

-- | Write padding to align to 8 bytes
writePadding :: Int -> Writer ()
writePadding len =
  let pad = (8 - (len .&. 7)) .&. 7
   in emit (B.byteString $ BS.replicate pad 0)

-- | Write bytes with length prefix and padding
writeBytes :: ByteString -> Writer ()
writeBytes bs = do
  writeU64 (fromIntegral $ BS.length bs)
  emit (B.byteString bs)
  writePadding (BS.length bs)

-- | Write a string (Text as UTF-8)
writeStr :: Text -> Writer ()
writeStr = writeBytes . TE.encodeUtf8

-- | Write a raw string (for NAR keywords)
writeRawStr :: ByteString -> Writer ()
writeRawStr = writeBytes

-- =============================================================================
-- NAR Structure
-- =============================================================================

-- | Write the NAR magic header
writeMagic :: Writer ()
writeMagic = writeRawStr narVersionMagic

-- | Write an opening parenthesis
writeOpen :: Writer ()
writeOpen = writeRawStr "("

-- | Write a closing parenthesis
writeClose :: Writer ()
writeClose = writeRawStr ")"

-- | Write type field
writeType :: ByteString -> Writer ()
writeType typeName = do
  writeRawStr "type"
  writeRawStr typeName

-- =============================================================================
-- Node types
-- =============================================================================

-- | Write a complete regular file node
writeRegularFile :: ByteString -> Bool -> Writer ()
writeRegularFile contents executable = do
  writeOpen
  writeType "regular"
  when executable $ do
    writeRawStr "executable"
    writeRawStr ""
  writeRawStr "contents"
  writeBytes contents
  writeClose
  where
    when True m = m
    when False _ = pure ()

-- | Write a complete symlink node
writeSymlink :: Text -> Writer ()
writeSymlink target = do
  writeOpen
  writeType "symlink"
  writeRawStr "target"
  writeStr target
  writeClose

-- | Begin a directory node
beginDirectory :: Writer ()
beginDirectory = do
  writeOpen
  writeType "directory"

-- | Write a directory entry header
beginEntry :: Text -> Writer ()
beginEntry name = do
  writeRawStr "entry"
  writeOpen
  writeRawStr "name"
  writeStr name
  writeRawStr "node"

-- | End a directory entry
endEntry :: Writer ()
endEntry = writeClose

-- | End a directory
endDirectory :: Writer ()
endDirectory = writeClose

-- =============================================================================
-- High-level API
-- =============================================================================

-- | Dump bytes as a simple NAR (single regular file at root)
dumpString :: ByteString -> Writer ()
dumpString contents = do
  writeMagic
  writeRegularFile contents False

-- | Dump bytes as an executable NAR
dumpExecutable :: ByteString -> Writer ()
dumpExecutable contents = do
  writeMagic
  writeRegularFile contents True

-- | Dump a symlink as a NAR
dumpSymlink :: Text -> Writer ()
dumpSymlink target = do
  writeMagic
  writeSymlink target

-- | Dump an empty directory as a NAR
dumpEmptyDirectory :: Writer ()
dumpEmptyDirectory = do
  writeMagic
  beginDirectory
  endDirectory

-- =============================================================================
-- Filesystem tree representation
-- =============================================================================

-- | A filesystem object that can be serialized to NAR
data FsObject
  = -- | Regular file with contents and executable flag
    RegularFile !ByteString !Bool
  | -- | Symbolic link with target path
    Symlink !Text
  | -- | Directory with entries (will be sorted during serialization)
    Directory ![(Text, FsObject)]
  deriving (Show, Eq)

-- | Create a regular file
file :: ByteString -> FsObject
file contents = RegularFile contents False

-- | Create an executable file
executable :: ByteString -> FsObject
executable contents = RegularFile contents True

-- | Create a symlink
symlink :: Text -> FsObject
symlink = Symlink

-- | Create an empty directory
emptyDir :: FsObject
emptyDir = Directory []

-- | Create a directory with entries (entries will be sorted)
directory :: [(Text, FsObject)] -> FsObject
directory = Directory . sortOn fst

-- | Serialize a filesystem object to NAR format
toNar :: FsObject -> Writer ()
toNar obj = do
  writeMagic
  writeNode obj

-- | Write a single node (without magic)
writeNode :: FsObject -> Writer ()
writeNode = \case
  RegularFile contents exec -> writeRegularFile contents exec
  Symlink target -> writeSymlink target
  Directory entries -> do
    beginDirectory
    mapM_ writeEntry (sortOn fst entries)
    endDirectory
  where
    writeEntry (name, child) = do
      beginEntry name
      writeNode child
      endEntry

-- =============================================================================
-- Reader/Parser
-- =============================================================================

-- | NAR parse error
data NarError
  = UnexpectedEof
  | InvalidMagic !ByteString
  | ExpectedToken !ByteString !ByteString  -- expected, got
  | InvalidNodeType !ByteString
  | NonZeroPadding
  deriving (Show, Eq)

-- | Reader monad for parsing NAR archives
newtype Reader a = Reader {unReader :: ByteString -> Either NarError (a, ByteString)}
  deriving (Functor)

instance Applicative Reader where
  pure a = Reader $ \bs -> Right (a, bs)
  Reader f <*> Reader x = Reader $ \bs -> do
    (g, bs') <- f bs
    (a, bs'') <- x bs'
    Right (g a, bs'')

instance Monad Reader where
  Reader m >>= k = Reader $ \bs -> do
    (a, bs') <- m bs
    unReader (k a) bs'

-- | Run a reader and get the result and remaining bytes
runReader :: Reader a -> ByteString -> Either NarError (a, ByteString)
runReader = unReader

-- | Fail with an error
failWith :: NarError -> Reader a
failWith e = Reader $ \_ -> Left e

-- =============================================================================
-- Reader primitives
-- =============================================================================

-- | Read a u64 in little-endian format
readU64 :: Reader Word64
readU64 = Reader $ \bs ->
  if BS.length bs < 8
    then Left UnexpectedEof
    else
      let (bytes, rest) = BS.splitAt 8 bs
          val = foldr (\(i, b) acc -> acc + fromIntegral b * (256 ^ i)) 0
                  (zip [0 :: Int ..] (BS.unpack bytes))
       in Right (val, rest)

-- | Read and verify padding bytes (must be zeros)
readPadding :: Int -> Reader ()
readPadding len = Reader $ \bs ->
  let pad = (8 - (len .&. 7)) .&. 7
   in if BS.length bs < pad
        then Left UnexpectedEof
        else
          let (padBytes, rest) = BS.splitAt pad bs
           in if BS.all (== 0) padBytes
                then Right ((), rest)
                else Left NonZeroPadding

-- | Read NAR bytes (length-prefixed with padding)
readBytes :: Reader ByteString
readBytes = do
  len <- readU64
  let lenInt = fromIntegral len
  Reader $ \bs ->
    if BS.length bs < lenInt
      then Left UnexpectedEof
      else
        let (content, rest) = BS.splitAt lenInt bs
         in Right (content, rest)
  >>= \content -> do
    readPadding (BS.length content)
    pure content

-- | Read a NAR string (length-prefixed with padding)
readStr :: Reader ByteString
readStr = readBytes

-- | Read and expect a specific string token
expect :: ByteString -> Reader ()
expect expected = do
  got <- readStr
  if got == expected
    then pure ()
    else failWith $ ExpectedToken expected got

-- =============================================================================
-- NAR structure parsing
-- =============================================================================

-- | Read and validate the NAR magic header
readMagic :: Reader ()
readMagic = do
  magic <- readStr
  if magic == narVersionMagic
    then pure ()
    else failWith $ InvalidMagic magic

-- | Read a complete node (file, directory, or symlink)
readNode :: Reader FsObject
readNode = do
  expect "("
  expect "type"
  typeName <- readStr
  case typeName of
    "regular" -> readRegularFile
    "directory" -> readDirectory
    "symlink" -> readSymlinkNode
    _ -> failWith $ InvalidNodeType typeName

-- | Read a regular file node (after "type" "regular")
readRegularFile :: Reader FsObject
readRegularFile = do
  first <- readStr
  let isExec = first == "executable"
  if isExec
    then do
      expect ""  -- empty executable marker
      expect "contents"
    else
      if first /= "contents"
        then failWith $ ExpectedToken "contents" first
        else pure ()
  contents <- readBytes
  expect ")"
  pure $ RegularFile contents isExec

-- | Read a directory node (after "type" "directory")
readDirectory :: Reader FsObject
readDirectory = do
  entries <- readEntries
  pure $ Directory entries
  where
    readEntries :: Reader [(Text, FsObject)]
    readEntries = do
      tag <- readStr
      if tag == ")"
        then pure []
        else
          if tag /= "entry"
            then failWith $ ExpectedToken "entry" tag
            else do
              expect "("
              expect "name"
              name <- readStr
              expect "node"
              child <- readNode
              expect ")"
              rest <- readEntries
              pure $ (TE.decodeUtf8 name, child) : rest

-- | Read a symlink node (after "type" "symlink")
readSymlinkNode :: Reader FsObject
readSymlinkNode = do
  expect "target"
  target <- readStr
  expect ")"
  pure $ Symlink (TE.decodeUtf8 target)

-- | Parse a complete NAR from a ByteString
parseNar :: ByteString -> Either NarError FsObject
parseNar bs = do
  ((), bs') <- runReader readMagic bs
  (obj, _) <- runReader readNode bs'
  pure obj
