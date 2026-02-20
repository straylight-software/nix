#!/usr/bin/env python3
"""
Enhanced Nix daemon MITM capture tool.

Captures complete request/response pairs including:
- Handshake (client_hello, server_hello, continuation)
- Operations with full stderr protocol handling
- Saves individual .bin files for each message type

Usage:
    # Terminal 1: Start proxy
    python3 capture_ops.py

    # Terminal 2: Trigger various operations
    NIX_DAEMON_SOCKET_PATH=/tmp/nix-proxy.sock nix path-info /nix/store/...
    NIX_DAEMON_SOCKET_PATH=/tmp/nix-proxy.sock nix build nixpkgs#hello --dry-run
    NIX_DAEMON_SOCKET_PATH=/tmp/nix-proxy.sock nix build nixpkgs#hello
"""

import os
import socket
import struct
import sys
import select
from dataclasses import dataclass, field
from datetime import datetime
from enum import IntEnum
from pathlib import Path
from typing import Optional

# Paths
REAL_SOCKET = "/nix/var/nix/daemon-socket/socket"
PROXY_SOCKET = "/tmp/nix-proxy.sock"
CAPTURE_DIR = Path(__file__).parent / "captures"

# Protocol constants
WORKER_MAGIC_1 = 0x6e697863  # "nixc" client->server
WORKER_MAGIC_2 = 0x6478696f  # "dxio" server->client

# Stderr protocol
STDERR_NEXT = 0x6f6c6d67   # "gmlo" - log message
STDERR_READ = 0x64617461   # "data" - daemon wants data
STDERR_WRITE = 0x64617416  # write data to client
STDERR_LAST = 0x616c7473   # "stla" - success, response follows
STDERR_ERROR = 0x63787470  # "ptxc" - error
STDERR_START_ACTIVITY = 0x53545254  # "STRT"
STDERR_STOP_ACTIVITY = 0x53544f50   # "STOP"
STDERR_RESULT = 0x52534c54          # "RSLT"

STDERR_NAMES = {
    STDERR_NEXT: "STDERR_NEXT",
    STDERR_READ: "STDERR_READ",
    STDERR_WRITE: "STDERR_WRITE",
    STDERR_LAST: "STDERR_LAST",
    STDERR_ERROR: "STDERR_ERROR",
    STDERR_START_ACTIVITY: "STDERR_START_ACTIVITY",
    STDERR_STOP_ACTIVITY: "STDERR_STOP_ACTIVITY",
    STDERR_RESULT: "STDERR_RESULT",
}


class Op(IntEnum):
    IsValidPath = 1
    HasSubstitutes = 3
    QueryPathHash = 4
    QueryReferences = 5
    QueryReferrers = 6
    AddToStore = 7
    AddTextToStore = 8
    BuildPaths = 9
    EnsurePath = 10
    AddTempRoot = 11
    AddIndirectRoot = 12
    SyncWithGC = 13
    FindRoots = 14
    ExportPath = 16
    QueryDeriver = 18
    SetOptions = 19
    CollectGarbage = 20
    QuerySubstitutablePathInfo = 21
    QueryDerivationOutputs = 22
    QueryAllValidPaths = 23
    QueryFailedPaths = 24
    ClearFailedPaths = 25
    QueryPathInfo = 26
    ImportPaths = 27
    QueryDerivationOutputNames = 28
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


def read_u64(data: bytes, offset: int = 0) -> int:
    return struct.unpack("<Q", data[offset:offset+8])[0]


def read_nix_string(data: bytes, offset: int = 0) -> tuple[str, int]:
    """Read a nix string, return (string, bytes_consumed)."""
    length = read_u64(data, offset)
    string_data = data[offset+8:offset+8+length]
    padding = (8 - (length % 8)) % 8
    total = 8 + length + padding
    return string_data.decode('utf-8', errors='replace'), total


@dataclass
class CapturedOp:
    """A complete captured operation with request and response."""
    op: Op
    request: bytes
    response: bytes
    stderr_messages: list[tuple[int, bytes]] = field(default_factory=list)
    timestamp: datetime = field(default_factory=datetime.now)

    def save(self, capture_dir: Path):
        """Save this operation to files."""
        name = self.op.name.lower()
        
        # Save request
        req_path = capture_dir / f"{name}_request.bin"
        with open(req_path, "wb") as f:
            f.write(self.request)
        print(f"    Saved: {req_path.name} ({len(self.request)} bytes)")
        
        # Save response (if any)
        if self.response:
            resp_path = capture_dir / f"{name}_response.bin"
            with open(resp_path, "wb") as f:
                f.write(self.response)
            print(f"    Saved: {resp_path.name} ({len(self.response)} bytes)")
        
        # Save stderr sequence (for debugging)
        if self.stderr_messages:
            stderr_path = capture_dir / f"{name}_stderr.txt"
            with open(stderr_path, "w") as f:
                for msg_type, data in self.stderr_messages:
                    name_str = STDERR_NAMES.get(msg_type, f"0x{msg_type:08x}")
                    f.write(f"{name_str}: {len(data)} bytes\n")
                    f.write(f"  {data[:64].hex()}\n")
            print(f"    Saved: {stderr_path.name} ({len(self.stderr_messages)} messages)")


class ProtocolState:
    """Track protocol state during capture."""
    
    def __init__(self):
        self.handshake_complete = False
        self.protocol_version = 0
        self.client_version = 0
        self.server_version = 0
        self.current_op: Optional[Op] = None
        self.captures: list[CapturedOp] = []
        self.pending_request: bytes = b""
        self.pending_stderr: list[tuple[int, bytes]] = []
        self.pending_response: bytes = b""
        
    def negotiated_version(self) -> int:
        return min(self.client_version, self.server_version) & 0xff


class MITMProxy:
    """MITM proxy that captures complete operation exchanges."""
    
    def __init__(self):
        self.state = ProtocolState()
        CAPTURE_DIR.mkdir(exist_ok=True)
        
    def handle_connection(self, client: socket.socket):
        """Handle a single client connection with synchronous capture."""
        print("\n" + "="*60)
        print("New connection")
        print("="*60)
        
        # Connect to real daemon
        daemon = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            daemon.connect(REAL_SOCKET)
        except Exception as e:
            print(f"Failed to connect to daemon: {e}")
            client.close()
            return
        
        client.setblocking(False)
        daemon.setblocking(False)
        
        client_buf = b""
        daemon_buf = b""
        
        try:
            while True:
                readable, _, _ = select.select([client, daemon], [], [], 1.0)
                
                for sock in readable:
                    try:
                        data = sock.recv(65536)
                    except BlockingIOError:
                        continue
                    
                    if not data:
                        raise ConnectionError("Socket closed")
                    
                    if sock is client:
                        # Client -> Server
                        client_buf += data
                        client_buf = self.process_client_data(client_buf)
                        daemon.sendall(data)
                    else:
                        # Server -> Client  
                        daemon_buf += data
                        daemon_buf = self.process_server_data(daemon_buf)
                        client.sendall(data)
                        
        except (ConnectionError, BrokenPipeError):
            pass
        except Exception as e:
            print(f"Error: {e}")
        finally:
            client.close()
            daemon.close()
        
        print(f"\nConnection closed. Captured {len(self.state.captures)} operations.")
        self.save_captures()
        
    def process_client_data(self, buf: bytes) -> bytes:
        """Process client->server data, return unconsumed buffer."""
        if len(buf) < 8:
            return buf
        
        if not self.state.handshake_complete:
            # Expecting client hello
            if len(buf) >= 16:
                magic = read_u64(buf, 0)
                if magic == WORKER_MAGIC_1:
                    version = read_u64(buf, 8)
                    self.state.client_version = version
                    print(f"Client Hello: version {(version >> 8) & 0xff}.{version & 0xff}")
                    
                    # Save client hello
                    with open(CAPTURE_DIR / "client_hello.bin", "wb") as f:
                        f.write(buf[:16])
                    
                    return buf[16:]
            return buf
        
        # After handshake, expect operations
        op_val = read_u64(buf, 0)
        try:
            op = Op(op_val)
            self.state.current_op = op
            self.state.pending_request = buf  # Capture full request
            self.state.pending_stderr = []
            self.state.pending_response = b""
            print(f"\n>>> {op.name} (op {op_val})")
            
            # For now, consume all data as request (simplified)
            return b""
        except ValueError:
            # Not an op code, might be continuation data
            return b""
    
    def process_server_data(self, buf: bytes) -> bytes:
        """Process server->client data, return unconsumed buffer."""
        if len(buf) < 8:
            return buf
        
        if not self.state.handshake_complete:
            # Expecting server hello
            magic = read_u64(buf, 0)
            if magic == WORKER_MAGIC_2:
                if len(buf) >= 16:
                    version = read_u64(buf, 8)
                    self.state.server_version = version
                    print(f"Server Hello: version {(version >> 8) & 0xff}.{version & 0xff}")
                    print(f"Negotiated: 1.{self.state.negotiated_version()}")
                    self.state.handshake_complete = True
                    
                    # Save server hello
                    with open(CAPTURE_DIR / "server_hello.bin", "wb") as f:
                        f.write(buf[:16])
                    
                    # Remaining is handshake continuation (daemon version, trust, etc)
                    return buf[16:]
            return buf
        
        # Process stderr protocol
        while len(buf) >= 8:
            msg_type = read_u64(buf, 0)
            
            if msg_type == STDERR_LAST:
                # Success - response follows
                print(f"    STDERR_LAST")
                self.state.pending_stderr.append((msg_type, b""))
                buf = buf[8:]
                
                # Everything after is the response
                if buf:
                    self.state.pending_response = buf
                    self.finalize_capture()
                    return b""
                break
                
            elif msg_type == STDERR_ERROR:
                print(f"    STDERR_ERROR")
                self.state.pending_stderr.append((msg_type, buf[8:]))
                self.finalize_capture()
                return b""
                
            elif msg_type == STDERR_NEXT:
                # Log message: u64 type + nix_string
                if len(buf) < 16:
                    break
                str_len = read_u64(buf, 8)
                padding = (8 - (str_len % 8)) % 8
                total = 8 + 8 + str_len + padding
                if len(buf) < total:
                    break
                msg_str, _ = read_nix_string(buf, 8)
                print(f"    STDERR_NEXT: {msg_str[:60]}...")
                self.state.pending_stderr.append((msg_type, buf[8:total]))
                buf = buf[total:]
                
            elif msg_type == STDERR_START_ACTIVITY:
                # Complex message, skip for now
                print(f"    STDERR_START_ACTIVITY")
                # Rough estimate - consume what we can
                self.state.pending_stderr.append((msg_type, buf[8:]))
                return b""
                
            elif msg_type == STDERR_STOP_ACTIVITY:
                if len(buf) < 16:
                    break
                print(f"    STDERR_STOP_ACTIVITY")
                self.state.pending_stderr.append((msg_type, buf[8:16]))
                buf = buf[16:]
                
            elif msg_type == STDERR_RESULT:
                print(f"    STDERR_RESULT")
                self.state.pending_stderr.append((msg_type, buf[8:]))
                return b""
                
            else:
                # Unknown or response data
                # If we have a pending op and this isn't a stderr msg, it's likely response
                if self.state.current_op:
                    self.state.pending_response = buf
                    self.finalize_capture()
                    return b""
                break
        
        return buf
    
    def finalize_capture(self):
        """Finalize current operation capture."""
        if self.state.current_op and self.state.pending_request:
            cap = CapturedOp(
                op=self.state.current_op,
                request=self.state.pending_request,
                response=self.state.pending_response,
                stderr_messages=self.state.pending_stderr.copy()
            )
            self.state.captures.append(cap)
            print(f"    Captured: {cap.op.name}")
        
        self.state.current_op = None
        self.state.pending_request = b""
        self.state.pending_stderr = []
        self.state.pending_response = b""
    
    def save_captures(self):
        """Save all captures to disk."""
        print("\nSaving captures...")
        seen_ops = set()
        
        for cap in self.state.captures:
            if cap.op not in seen_ops:
                cap.save(CAPTURE_DIR)
                seen_ops.add(cap.op)


def main():
    # Remove existing proxy socket
    try:
        os.unlink(PROXY_SOCKET)
    except FileNotFoundError:
        pass
    
    # Create proxy socket
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(PROXY_SOCKET)
    server.listen(5)
    os.chmod(PROXY_SOCKET, 0o666)
    
    print("Nix Daemon MITM Capture Tool")
    print("="*60)
    print(f"Proxy socket: {PROXY_SOCKET}")
    print(f"Real daemon:  {REAL_SOCKET}")
    print(f"Captures:     {CAPTURE_DIR}")
    print()
    print("Commands to capture operations:")
    print()
    print("  # SetOptions + QueryPathInfo")
    print(f"  NIX_DAEMON_SOCKET_PATH={PROXY_SOCKET} nix path-info /nix/store/...")
    print()
    print("  # QueryMissing + BuildPaths (dry-run)")
    print(f"  NIX_DAEMON_SOCKET_PATH={PROXY_SOCKET} nix build nixpkgs#hello --dry-run")
    print()
    print("  # BuildPathsWithResults (actual build)")
    print(f"  NIX_DAEMON_SOCKET_PATH={PROXY_SOCKET} nix build nixpkgs#hello")
    print()
    print("  # AddToStore")
    print(f"  NIX_DAEMON_SOCKET_PATH={PROXY_SOCKET} nix store add-file ./somefile")
    print()
    print("  # NarFromPath")
    print(f"  NIX_DAEMON_SOCKET_PATH={PROXY_SOCKET} nix store dump-path /nix/store/... > /dev/null")
    print()
    print("  # QueryValidPaths")
    print(f"  NIX_DAEMON_SOCKET_PATH={PROXY_SOCKET} nix store ls /nix/store/...")
    print()
    print("Press Ctrl+C to stop.")
    print("="*60)
    
    proxy = MITMProxy()
    
    try:
        while True:
            client, _ = server.accept()
            proxy.handle_connection(client)
            # Reset state for next connection
            proxy.state = ProtocolState()
    except KeyboardInterrupt:
        print("\n\nShutting down...")
    finally:
        server.close()
        try:
            os.unlink(PROXY_SOCKET)
        except:
            pass


if __name__ == "__main__":
    main()
