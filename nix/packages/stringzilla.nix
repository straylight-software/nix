# nix/packages/stringzilla.nix
#
# StringZilla C++ headers (header-only library)
# The nixpkgs version is Python-only, so we package the C++ headers separately.
#
{ stdenv, fetchFromGitHub }:

stdenv.mkDerivation {
  pname = "stringzilla";
  version = "4.5.1";

  src = fetchFromGitHub {
    owner = "ashvardanian";
    repo = "StringZilla";
    rev = "v4.5.1";
    hash = "sha256-0T8hQ+P6gZnIX52jkRcpF1Ofxy45+B7K/feEQr5Phf0=";
  };

  dontBuild = true;

  installPhase = ''
    mkdir -p $out/include
    cp -r include/stringzilla $out/include/
  '';

  meta = {
    description = "SIMD-accelerated string operations for C++";
    homepage = "https://github.com/ashvardanian/StringZilla";
  };
}
