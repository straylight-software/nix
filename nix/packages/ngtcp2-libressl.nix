# nix/packages/ngtcp2-libressl.nix
#
# ngtcp2 QUIC library built with LibreSSL instead of OpenSSL.
# LibreSSL 4.x has native QUIC support (SSL_provide_quic_data).
#
{
  stdenv,
  fetchurl,
  cmake,
  brotli,
  libev,
  nghttp3,
  libressl,
  lib,
}:

stdenv.mkDerivation {
  pname = "ngtcp2";
  version = "1.18.0";

  src = fetchurl {
    url = "https://github.com/ngtcp2/ngtcp2/releases/download/v1.18.0/ngtcp2-1.18.0.tar.bz2";
    hash = "sha256-E7r7bFCdv2pw2WBaLIkuE/WuuTZnOZWHeKhXvHDOH6c=";
  };

  outputs = [
    "out"
    "dev"
  ];

  nativeBuildInputs = [ cmake ];

  buildInputs = [
    brotli
    libev
    nghttp3
    libressl
  ];

  cmakeFlags = [
    "-DENABLE_OPENSSL=ON" # auto-detects LibreSSL via LIBRESSL_VERSION_NUMBER
    "-DENABLE_SHARED_LIB=ON"
    "-DENABLE_STATIC_LIB=OFF"
    "-DENABLE_LIB_ONLY=ON" # skip examples with Linux-specific APIs
  ];

  doCheck = true;

  meta = {
    description = "ngtcp2 QUIC library built with LibreSSL";
    homepage = "https://github.com/ngtcp2/ngtcp2";
    license = lib.licenses.mit;
  };
}
