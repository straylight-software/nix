# Build aws-lc static libraries from aws-lc-sys vendored source.
# These are needed by aws-lc-rs for cryptographic operations in Firecracker.
#
# The library names must match what aws-lc-sys expects:
#   - libaws_lc_0_35_0_crypto.a
#   - libaws_lc_0_35_0_ssl.a (optional - only crypto is strictly needed)
#
# This package builds for MUSL (static linking) by default.
# Use `pkgs.callPackage ./aws-lc-sys.nix {}` for musl
# or override stdenv for glibc if needed.
#
# The isospinRustVendor argument is required to avoid pure evaluation issues
# with the symlinked vendor directory.
#
{
  pkgs,
  isospinRustVendor ? null,
}:
let
  version = "0.35.0";
  versionUnderscored = builtins.replaceStrings [ "." ] [ "_" ] version;

  # The entire aws-lc-sys crate directory (contains both aws-lc and generated-include)
  # We need to avoid string interpolation of store paths at eval time for pure mode.
  # When isospinRustVendor is provided, use it as a build input and reference at build time.
  useVendorInput = isospinRustVendor != null;

  # For local path reference (impure mode fallback)
  localCratePath = ../../vendor/isospin/third-party/rust/vendor/aws-lc-sys-${version};

  # Use musl stdenv - we disable libssl (C++) so we only need C compiler
  muslStdenv = pkgs.pkgsMusl.stdenv;
  llvmPackages = pkgs.pkgsMusl.llvmPackages_19;
  muslPkgs = pkgs.pkgsMusl;
in
muslStdenv.mkDerivation {
  pname = "aws-lc-sys";
  inherit version;

  # Use local path as src, but override in unpackPhase if using vendor input
  src = if useVendorInput then pkgs.emptyDirectory else localCratePath;

  # Pass vendor as a build input so we can reference it at build time
  nativeBuildInputs' = pkgs.lib.optionals useVendorInput [ isospinRustVendor ];

  # Custom unpack phase when using vendor input
  unpackPhase =
    if useVendorInput then
      ''
        runHook preUnpack
        cp -rL ${isospinRustVendor}/aws-lc-sys-${version} source
        chmod -R u+w source
        cd source/aws-lc
        runHook postUnpack
      ''
    else
      null;

  # We only need to build the aws-lc subdirectory (when not using custom unpack)
  sourceRoot = if useVendorInput then null else "aws-lc-sys-${version}/aws-lc";

  nativeBuildInputs = [
    pkgs.cmake # Build tools can use host pkgs
    pkgs.go
    pkgs.perl
    pkgs.ninja
    llvmPackages.clang
  ];

  # musl needs explicit pthread
  buildInputs = [
    muslPkgs.musl
  ];

  # Use clang with musl target
  CC = "${llvmPackages.clang}/bin/clang";
  CXX = "${llvmPackages.clang}/bin/clang++";

  cmakeFlags = [
    "-GNinja"
    "-DBUILD_SHARED_LIBS=OFF"
    "-DBUILD_TESTING=OFF"
    "-DBUILD_TOOL=OFF"
    "-DBUILD_LIBSSL=OFF" # Disable libssl (C++) - only need libcrypto (C)
    "-DDISABLE_GO=ON" # Don't need Go - we use pre-generated prefix headers
    "-DCMAKE_BUILD_TYPE=Release"
    "-DCMAKE_C_COMPILER=${llvmPackages.clang}/bin/clang"
    "-DCMAKE_CXX_COMPILER=${llvmPackages.clang}/bin/clang++"
    # Disable fortify source which causes issues with musl
    "-DCMAKE_C_FLAGS=-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0"
    # Help CMake find pthread in musl
    "-DCMAKE_THREAD_LIBS_INIT=-pthread"
    "-DTHREADS_PREFER_PTHREAD_FLAG=ON"
  ];

  # Add the pre-generated prefix headers to includes
  # These define macros that rename all symbols to aws_lc_0_35_0_*
  # Also disable -Wunused-command-line-argument error (musl injects -pie which upsets -Werror)
  # When using vendor input, the header path is determined at build time via preConfigure
  NIX_CFLAGS_COMPILE =
    if useVendorInput then
      "-DBORINGSSL_PREFIX=aws_lc_${versionUnderscored} -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -Wno-unused-command-line-argument"
    else
      "-include ${localCratePath}/generated-include/openssl/boringssl_prefix_symbols.h -DBORINGSSL_PREFIX=aws_lc_${versionUnderscored} -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -Wno-unused-command-line-argument";

  # When using vendor input, add the include flag at build time
  preConfigure =
    if useVendorInput then
      ''
        export NIX_CFLAGS_COMPILE="-include ${isospinRustVendor}/aws-lc-sys-${version}/generated-include/openssl/boringssl_prefix_symbols.h $NIX_CFLAGS_COMPILE"
      ''
    else
      null;

  # Disable hardening which adds -pie flag
  hardeningDisable = [
    "pie"
    "format"
  ];

  # Rename the output libraries to match what aws-lc-sys expects
  postInstall = ''
    mkdir -p $out/lib
    if [ -f $out/lib/libcrypto.a ]; then
      mv $out/lib/libcrypto.a $out/lib/libaws_lc_${versionUnderscored}_crypto.a
    fi
    if [ -f $out/lib/libssl.a ]; then
      mv $out/lib/libssl.a $out/lib/libaws_lc_${versionUnderscored}_ssl.a
    fi
  '';

  meta = {
    description = "AWS-LC crypto/SSL libraries for aws-lc-sys";
    license = pkgs.lib.licenses.asl20;
  };
}
