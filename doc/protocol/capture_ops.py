#!/usr/bin/env python3
"""
Capture specific Nix daemon operations via MITM socket.

Usage:
    # Terminal 1: Start proxy
    python3 capture_ops.py
    
    # Terminal 2: Run nix command with custom socket
    NIX_DAEMON_SOCKET_PATH=/tmp/nix-proxy.sock nix build nixpkgs#hello --dry-run
    
The script will capture all traffic and save individual messages.
"""

import os
import socket
import struct
import sys
import threading
from dataclasses import dataclass
from datetime import datetime
from enum import IntEnum
from pathlib import Path

# Nix daemon socket path
REAL_SOCKET = "/nix/var/nix/daemon-socket/socket"
PROXY_SOCKET = "/tmp/nix-proxy.sock"
CAPTURE_DIR = Path(__file__).parent / "captures"

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
STDERR_LAST = 0x616c7473

WORKER_MAGIC_1 = 0x6e697863
WORKER_MAGIC_2 = 0x6478696f

@dataclass
class Message:
    """Captured message with direction and raw data."""
    direction: str  # "C->S" or "S->C"
    data: bytes
    timestamp: datetime

class Capturer:
    def __init__(self):
        self.messages: list[Message] = []
        self.capture_num = 0
        self.protocol_version = 0
        CAPTURE_DIR.mkdir(exist_ok=True)
    
    def record(self, direction: str, data: bytes):
        msg = Message(direction=direction, data=data, timestamp=datetime.now())
        self.messages.append(msg)
        print(f"  {direction} {len(data):6d} bytes: {data[:32].hex()}")
    
    def save_message(self, name: str, data: bytes):
        """Save a named message to file."""
        path = CAPTURE_DIR / f"{name}.bin"
        with open(path, "wb") as f:
            f.write(data)
        print(f"  -> Saved to {path}")
    
    def parse_client_hello(self, data: bytes) -> int:
        """Parse client hello, return version."""
        magic = struct.unpack("<Q", data[:8])[0]
        version = struct.unpack("<Q", data[8:16])[0]
        assert magic == WORKER_MAGIC_1, f"Bad client magic: {magic:#x}"
        print(f"  Client version: {(version >> 8) & 0xff}.{version & 0xff}")
        return version
    
    def parse_server_hello(self, data: bytes) -> int:
        """Parse server hello, return version."""
        magic = struct.unpack("<Q", data[:8])[0]
        version = struct.unpack("<Q", data[8:16])[0]
        assert magic == WORKER_MAGIC_2, f"Bad server magic: {magic:#x}"
        print(f"  Server version: {(version >> 8) & 0xff}.{version & 0xff}")
        return version
    
    def parse_op(self, data: bytes) -> tuple[int, str]:
        """Parse operation code, return (op, name)."""
        op = struct.unpack("<Q", data[:8])[0]
        try:
            name = Op(op).name
        except ValueError:
            name = f"Unknown({op})"
        return op, name

def forward_with_capture(src: socket.socket, dst: socket.socket, direction: str, capturer: Capturer):
    """Forward data from src to dst, recording for capture."""
    try:
        while True:
            data = src.recv(65536)
            if not data:
                break
            capturer.record(direction, data)
            dst.sendall(data)
    except Exception as e:
        print(f"Forward error ({direction}): {e}")
    finally:
        try:
            src.shutdown(socket.SHUT_RD)
        except:
            pass
        try:
            dst.shutdown(socket.SHUT_WR)
        except:
            pass

def handle_connection(client: socket.socket, capturer: Capturer):
    """Handle a single client connection."""
    print("\n=== New connection ===")
    
    # Connect to real daemon
    daemon = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        daemon.connect(REAL_SOCKET)
    except Exception as e:
        print(f"Failed to connect to daemon: {e}")
        client.close()
        return
    
    # Start forwarding threads
    c2s = threading.Thread(target=forward_with_capture, args=(client, daemon, "C->S", capturer))
    s2c = threading.Thread(target=forward_with_capture, args=(daemon, client, "S->C", capturer))
    
    c2s.start()
    s2c.start()
    
    c2s.join()
    s2c.join()
    
    client.close()
    daemon.close()
    
    print(f"=== Connection closed ({len(capturer.messages)} messages) ===\n")
    
    # Analyze captured messages
    analyze_capture(capturer)

def analyze_capture(capturer: Capturer):
    """Analyze and save interesting parts of a capture."""
    if not capturer.messages:
        return
    
    print("\nAnalyzing capture...")
    
    # Group consecutive messages by direction
    current_op = None
    request_data = b""
    response_data = b""
    
    for i, msg in enumerate(capturer.messages):
        if msg.direction == "C->S":
            # Check for operation code in client messages
            if len(msg.data) >= 8:
                op_val = struct.unpack("<Q", msg.data[:8])[0]
                try:
                    op = Op(op_val)
                    current_op = op.name
                    request_data = msg.data
                    print(f"  Found operation: {current_op}")
                    
                    # Save request
                    capturer.save_message(f"{current_op.lower()}_request", msg.data)
                except ValueError:
                    pass

def main():
    # Remove existing proxy socket
    try:
        os.unlink(PROXY_SOCKET)
    except FileNotFoundError:
        pass
    
    # Create proxy socket
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(PROXY_SOCKET)
    server.listen(1)
    os.chmod(PROXY_SOCKET, 0o666)
    
    print(f"Nix daemon MITM proxy")
    print(f"  Listening on: {PROXY_SOCKET}")
    print(f"  Forwarding to: {REAL_SOCKET}")
    print(f"  Captures saved to: {CAPTURE_DIR}")
    print(f"\nTo use:")
    print(f"  NIX_DAEMON_SOCKET_PATH={PROXY_SOCKET} nix build nixpkgs#hello --dry-run")
    print(f"\nPress Ctrl+C to stop.\n")
    
    capturer = Capturer()
    
    try:
        while True:
            client, _ = server.accept()
            handle_connection(client, capturer)
    except KeyboardInterrupt:
        print("\nShutting down...")
    finally:
        server.close()
        os.unlink(PROXY_SOCKET)

if __name__ == "__main__":
    main()
