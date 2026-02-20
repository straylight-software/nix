# nix/packages/zpp-bits.nix
#
# zpp_bits - High-performance C++20 binary serialization (header-only)
#
{ stdenv, fetchFromGitHub }:

stdenv.mkDerivation {
  pname = "zpp_bits";
  version = "4.6";

  src = fetchFromGitHub {
    owner = "eyalz800";
    repo = "zpp_bits";
    rev = "v4.6";
    hash = "sha256-N3zT1eo3fLzN5/fJbfq01w7PGTD7pbmU9BMLjxsn33I=";
  };

  dontBuild = true;

  installPhase = ''
    mkdir -p $out/include
    cp zpp_bits.h $out/include/
  '';

  meta = {
    description = "High-performance C++20 binary serialization";
    homepage = "https://github.com/eyalz800/zpp_bits";
  };
}
