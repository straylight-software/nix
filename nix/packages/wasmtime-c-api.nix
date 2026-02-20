# nix/packages/wasmtime-c-api.nix
#
# Pre-built wasmtime C API static library from GitHub releases.
# This fetches the official static library to avoid building Rust from source.
#
# The C API includes:
# - libwasmtime.a - Static library
# - include/ - C and C++ headers (wasmtime.h, wasmtime.hh, etc.)
#
# Static linking requires additional system libs: -lpthread -ldl -lm
#
{
  stdenv,
  fetchzip,
  lib,
}:

let
  version = "40.0.0";

  # Platform-specific download info
  # Using musl builds to avoid glibc dependency
  platforms = {
    "x86_64-linux" = {
      url = "https://github.com/bytecodealliance/wasmtime/releases/download/v${version}/wasmtime-v${version}-x86_64-musl-c-api.tar.xz";
      hash = "sha256-oCnokNbuxHLHp6kNJyCv3iVXCLypw8grwn+cING0bnM=";
    };
    "aarch64-linux" = {
      url = "https://github.com/bytecodealliance/wasmtime/releases/download/v${version}/wasmtime-v${version}-aarch64-musl-c-api.tar.xz";
      hash = "sha256-7mcBlDfAKOz6Mjy6lsl61RCMcm9IoPyrSQb2Cbj4XWQ=";
    };
    # macOS uses regular (non-musl) builds
    "x86_64-darwin" = {
      url = "https://github.com/bytecodealliance/wasmtime/releases/download/v${version}/wasmtime-v${version}-x86_64-macos-c-api.tar.xz";
      hash = "sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="; # TODO: fill in when needed
    };
    "aarch64-darwin" = {
      url = "https://github.com/bytecodealliance/wasmtime/releases/download/v${version}/wasmtime-v${version}-aarch64-macos-c-api.tar.xz";
      hash = "sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="; # TODO: fill in when needed
    };
  };

  platformInfo =
    platforms.${stdenv.hostPlatform.system}
      or (throw "Unsupported platform: ${stdenv.hostPlatform.system}");
in
stdenv.mkDerivation {
  pname = "wasmtime-c-api";
  inherit version;

  src = fetchzip {
    inherit (platformInfo) url hash;
    extension = "tar.xz";
  };

  dontBuild = true;
  dontConfigure = true;

  installPhase = ''
    runHook preInstall

    mkdir -p $out/lib $out/include

    # Install static library
    cp lib/libwasmtime.a $out/lib/

    # Install headers
    cp -r include/* $out/include/

    runHook postInstall
  '';

  meta = with lib; {
    description = "Pre-built wasmtime C API static library";
    homepage = "https://github.com/bytecodealliance/wasmtime";
    license = licenses.asl20;
    platforms = [
      "x86_64-linux"
      "aarch64-linux"
      "x86_64-darwin"
      "aarch64-darwin"
    ];
  };
}
