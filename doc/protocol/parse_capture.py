#!/usr/bin/env python3
"""
Quick parser for Nix daemon protocol captures.
Validates the wire format against our understanding.
"""

import struct
import sys
from dataclasses import dataclass
from enum import IntEnum
from typing import Optional

class Op(IntEnum):
    IsValidPath = 1
    HasSubstitutes = 3
    QueryReferrers = 6
    AddToStore = 7
    BuildPaths = 9
    EnsurePath = 10
    AddTempRoot = 11
    AddIndirectRoot = 12
    SyncWithGC = 13
    FindRoots = 14
    SetOptions = 19
    CollectGarbage = 20
    QuerySubstitutablePathInfo = 21
    QueryAllValidPaths = 23
    QueryFailedPaths = 24
    ClearFailedPaths = 25
    QueryPathInfo = 26
    QueryPathFromHashPart = 29
    QuerySubstitutablePathInfos = 30
    QueryValidPaths = 31
    QuerySubstitutablePaths = 32
    QueryValidDerivers = 33
    OptimiseStore = 34
    VerifyStore = 35
    BuildDerivation = 36
    AddSignatures = 37
    NarFromPath = 38
    AddToStoreNar = 39
    QueryMissing = 40
    QueryDerivationOutputMap = 41
    RegisterDrvOutput = 42
    QueryRealisation = 43
    AddMultipleToStore = 44
    AddBuildLog = 45
    BuildPathsWithResults = 46
    AddPermRoot = 47
    QueryActiveBuilds = 48

# Stderr message types
STDERR_NEXT = 0x6f6c6d67
STDERR_READ = 0x64617461
STDERR_WRITE = 0x64617416
STDERR_LAST = 0x616c7473
STDERR_ERROR = 0x63787470
STDERR_START_ACTIVITY = 0x53545254  # "TRTS" backwards
STDERR_STOP_ACTIVITY = 0x53544f50   # "POTS" backwards
STDERR_RESULT = 0x52534c54

WORKER_MAGIC_1 = 0x6e697863  # "nixc"
WORKER_MAGIC_2 = 0x6478696f  # "dxio"

class Parser:
    def __init__(self, data: bytes):
        self.data = data
        self.pos = 0
        self.protocol_version = 0
    
    def remaining(self) -> int:
        return len(self.data) - self.pos
    
    def read_u64(self) -> int:
        if self.remaining() < 8:
            raise ValueError(f"Not enough data for u64 at pos {self.pos}")
        val = struct.unpack('<Q', self.data[self.pos:self.pos+8])[0]
        self.pos += 8
        return val
    
    def read_u8(self) -> int:
        if self.remaining() < 1:
            raise ValueError(f"Not enough data for u8 at pos {self.pos}")
        val = self.data[self.pos]
        self.pos += 1
        return val
    
    def read_string(self) -> str:
        length = self.read_u64()
        if self.remaining() < length:
            raise ValueError(f"Not enough data for string of length {length}")
        s = self.data[self.pos:self.pos+length].decode('utf-8', errors='replace')
        self.pos += length
        # Padding to 8-byte boundary
        padding = (8 - (length % 8)) % 8
        self.pos += padding
        return s
    
    def read_string_list(self) -> list:
        count = self.read_u64()
        return [self.read_string() for _ in range(count)]

def parse_handshake_client(p: Parser):
    print("=== Client Hello ===")
    magic = p.read_u64()
    assert magic == WORKER_MAGIC_1, f"Bad magic: {magic:#x}"
    print(f"  Magic: {magic:#x} (WORKER_MAGIC_1)")
    
    version = p.read_u64()
    major = (version >> 8) & 0xff
    minor = version & 0xff
    print(f"  Version: {major}.{minor} (raw: {version:#x})")
    return version

def parse_handshake_server(p: Parser):
    print("=== Server Hello ===")
    magic = p.read_u64()
    assert magic == WORKER_MAGIC_2, f"Bad magic: {magic:#x}"
    print(f"  Magic: {magic:#x} (WORKER_MAGIC_2)")
    
    version = p.read_u64()
    major = (version >> 8) & 0xff
    minor = version & 0xff
    print(f"  Version: {major}.{minor} (raw: {version:#x})")
    return version

def parse_client_continuation(p: Parser, version: int):
    minor = version & 0xff
    print("=== Client Continuation ===")
    
    if minor >= 14:
        cpu_affinity_tag = p.read_u64()
        print(f"  CPU affinity tag: {cpu_affinity_tag}")
        if cpu_affinity_tag != 0:
            cpu_affinity = p.read_u64()
            print(f"  CPU affinity: {cpu_affinity}")
    
    if minor >= 11:
        reserve_space = p.read_u64()
        print(f"  Reserve space: {reserve_space}")

def parse_server_handshake_info(p: Parser, version: int):
    minor = version & 0xff
    print("=== Server Handshake Info ===")
    
    if minor >= 33:
        daemon_version = p.read_string()
        print(f"  Daemon version: {daemon_version}")
    
    if minor >= 35:
        trust = p.read_u64()
        trust_str = {0: "unknown", 1: "trusted", 2: "not_trusted"}.get(trust, f"?{trust}")
        print(f"  Trust: {trust_str}")

def parse_set_options(p: Parser, version: int):
    minor = version & 0xff
    print("=== SetOptions ===")
    
    print(f"  keep_failed: {p.read_u64()}")
    print(f"  keep_going: {p.read_u64()}")
    print(f"  try_fallback: {p.read_u64()}")
    print(f"  verbosity: {p.read_u64()}")
    print(f"  max_build_jobs: {p.read_u64()}")
    print(f"  max_silent_time: {p.read_u64()}")
    print(f"  use_build_hook (obsolete): {p.read_u64()}")
    print(f"  verbose_build: {p.read_u64()}")
    print(f"  log_type (obsolete): {p.read_u64()}")
    print(f"  print_build_trace (obsolete): {p.read_u64()}")
    print(f"  build_cores: {p.read_u64()}")
    print(f"  use_substitutes: {p.read_u64()}")
    
    if minor >= 12:
        count = p.read_u64()
        print(f"  Overrides ({count}):")
        for _ in range(count):
            name = p.read_string()
            value = p.read_string()
            print(f"    {name} = {value}")

def parse_stderr_message(p: Parser):
    msg_type = p.read_u64()
    
    if msg_type == STDERR_LAST:
        print("  << STDERR_LAST (success)")
        return False
    elif msg_type == STDERR_NEXT:
        msg = p.read_string()
        print(f"  << STDERR_NEXT: {msg[:50]}...")
    elif msg_type == STDERR_START_ACTIVITY:
        act_id = p.read_u64()
        verbosity = p.read_u64()
        act_type = p.read_u64()
        msg = p.read_string()
        fields_count = p.read_u64()
        # Skip fields for now
        for _ in range(fields_count):
            ftype = p.read_u64()
            if ftype == 0:  # int
                p.read_u64()
            else:  # string
                p.read_string()
        parent = p.read_u64()
        print(f"  << STDERR_START_ACTIVITY: {msg}")
    elif msg_type == STDERR_STOP_ACTIVITY:
        act_id = p.read_u64()
        print(f"  << STDERR_STOP_ACTIVITY: {act_id}")
    elif msg_type == STDERR_ERROR:
        print("  << STDERR_ERROR")
        return False
    else:
        print(f"  << Unknown stderr: {msg_type:#x}")
    
    return True

def parse_query_path_info_response(p: Parser, version: int):
    minor = version & 0xff
    print("=== QueryPathInfo Response ===")
    
    valid = p.read_u64()
    print(f"  Valid: {valid}")
    
    if valid:
        deriver = p.read_string()
        print(f"  Deriver: {deriver or '(none)'}")
        
        nar_hash = p.read_string()
        print(f"  NAR hash: {nar_hash}")
        
        refs = p.read_string_list()
        print(f"  References ({len(refs)}):")
        for r in refs:
            print(f"    {r}")
        
        reg_time = p.read_u64()
        nar_size = p.read_u64()
        print(f"  Registration time: {reg_time}")
        print(f"  NAR size: {nar_size}")
        
        if minor >= 16:
            ultimate = p.read_u64()
            print(f"  Ultimate: {ultimate}")
            
            sigs = p.read_string_list()
            print(f"  Signatures ({len(sigs)}):")
            for s in sigs:
                print(f"    {s[:60]}...")
            
            ca = p.read_string()
            print(f"  Content address: {ca or '(none)'}")

def main():
    # From our capture - QueryPathInfo
    # Client -> Server: handshake + SetOptions + QueryValidPaths + QueryPathInfo
    # Server -> Client: handshake info + STDERR messages + response
    
    # Let's manually decode the key parts from our capture
    
    print("=" * 60)
    print("Decoding QueryPathInfo capture")
    print("=" * 60)
    
    # Client hello
    client_hello = bytes.fromhex("63 78 69 6e 00 00 00 00 26 01 00 00 00 00 00 00".replace(" ", ""))
    p = Parser(client_hello)
    client_version = parse_handshake_client(p)
    
    print()
    
    # Server hello  
    server_hello = bytes.fromhex("6f 69 78 64 00 00 00 00 26 01 00 00 00 00 00 00".replace(" ", ""))
    p = Parser(server_hello)
    server_version = parse_handshake_server(p)
    
    negotiated = min(client_version, server_version)
    print(f"\nNegotiated version: {(negotiated >> 8) & 0xff}.{negotiated & 0xff}")
    
    print()
    
    # Server handshake info (version string + trust)
    server_info = bytes.fromhex("""
        08 00 00 00 00 00 00 00 32 2e 33 31 2e 32 2b 31
        01 00 00 00 00 00 00 00
    """.replace(" ", "").replace("\n", ""))
    p = Parser(server_info)
    parse_server_handshake_info(p, negotiated)
    
    print()
    
    # QueryPathInfo request
    print("=== QueryPathInfo Request ===")
    query_req = bytes.fromhex("""
        1a 00 00 00 00 00 00 00 38 00 00 00 00 00 00 00
        2f 6e 69 78 2f 73 74 6f 72 65 2f 32 62 63 76 39
        31 69 38 66 61 68 71 67 68 6e 38 64 6d 79 72 37
        39 31 69 61 79 63 62 73 6a 64 64 2d 68 65 6c 6c
        6f 2d 32 2e 31 32 2e 32
    """.replace(" ", "").replace("\n", ""))
    p = Parser(query_req)
    op = p.read_u64()
    print(f"  Op: {op} ({Op(op).name})")
    path = p.read_string()
    print(f"  Path: {path}")
    
    print()
    
    # Response (after STDERR_LAST)
    print("=== QueryPathInfo Response ===")
    response = bytes.fromhex("""
        01 00 00 00 00 00 00 00
        3c 00 00 00 00 00 00 00 2f 6e 69 78 2f 73 74 6f
        72 65 2f 37 62 37 34 72 7a 33 38 36 39 35 6d 61
        71 77 64 62 32 6d 71 30 30 34 76 6b 39 32 31 69
        63 64 33 2d 68 65 6c 6c 6f 2d 32 2e 31 32 2e 32
        2e 64 72 76 00 00 00 00 40 00 00 00 00 00 00 00
        30 34 35 63 61 36 62 64 62 32 36 31 33 62 66 30
        39 62 66 65 33 33 30 37 38 37 66 66 35 61 33 34
        63 65 30 32 32 64 34 34 62 30 33 32 33 39 39 30
        33 61 37 33 36 63 38 31 39 64 61 32 61 61 36 36
        02 00 00 00 00 00 00 00 38 00 00 00 00 00 00 00
        2f 6e 69 78 2f 73 74 6f 72 65 2f 32 62 63 76 39
        31 69 38 66 61 68 71 67 68 6e 38 64 6d 79 72 37
        39 31 69 61 79 63 62 73 6a 64 64 2d 68 65 6c 6c
        6f 2d 32 2e 31 32 2e 32 39 00 00 00 00 00 00 00
        2f 6e 69 78 2f 73 74 6f 72 65 2f 78 78 37 63 6d
        37 32 71 79 32 63 30 36 34 33 63 6d 31 69 70 6e
        67 64 38 37 61 71 77 6b 63 64 70 2d 67 6c 69 62
        63 2d 32 2e 34 30 2d 36 36 00 00 00 00 00 00 00
        eb 38 96 69 00 00 00 00 88 30 04 00 00 00 00 00
        00 00 00 00 00 00 00 00 01 00 00 00 00 00 00 00
        6a 00 00 00 00 00 00 00 63 61 63 68 65 2e 6e 69
        78 6f 73 2e 6f 72 67 2d 31 3a 5a 4f 72 77 74 47
        54 7a 63 32 6b 55 74 73 50 62 61 47 46 42 4a 4a
        77 6e 38 68 69 46 2b 77 6b 78 77 6a 45 67 6f 6b
        59 69 55 2b 77 53 51 50 37 79 67 2b 51 53 32 6b
        45 59 54 45 59 5a 69 62 48 2f 46 39 37 36 57 45
        70 64 43 6c 44 43 58 6c 37 47 59 32 34 72 42 51
        3d 3d 00 00 00 00 00 00 00 00 00 00 00 00 00 00
    """.replace(" ", "").replace("\n", ""))
    p = Parser(response)
    parse_query_path_info_response(p, negotiated)
    
    print()
    print("=" * 60)
    print("Parse successful!")
    print("=" * 60)

if __name__ == "__main__":
    main()
