{- dhall/deps.dhall

   Third-party dependencies for straylight/nix.
   
   These map to flake refs that sensenet resolves at build time.
   No more .buckconfig.local magic - just declare what you need.
-}

-- Core util deps
let util-deps =
  [ "nixpkgs#boost"
  , "nixpkgs#nlohmann_json"
  , "nixpkgs#libressl"        -- LibreSSL, not OpenSSL
  , "nixpkgs#libblake3"
  , "nixpkgs#brotli"
  , "nixpkgs#libsodium"
  , "nixpkgs#libarchive"
  , "nixpkgs#ada"             -- WHATWG URL parser
  , "nixpkgs#re2"             -- Fast regex
  ]

-- Store deps
let store-deps =
  [ "nixpkgs#sqlite"
  , "nixpkgs#curl"
  ]

-- Fetchers deps
let fetchers-deps =
  [ "nixpkgs#libgit2"
  ]

-- Expr deps
let expr-deps =
  [ "nixpkgs#boehmgc"
  , "nixpkgs#toml11"
  ]

-- Main deps
let main-deps =
  [ "nixpkgs#editline"
  , "nixpkgs#lowdown"
  ]

-- Straylight primitives deps
let primitives-deps =
  [ ".#stringzilla"           -- SIMD strings (custom package)
  , ".#zpp_bits"              -- Binary serialization (custom package)
  , "nixpkgs#rapidfuzz-cpp"   -- Fuzzy matching
  , "nixpkgs#taskflow"        -- Parallel tasks
  ]

-- libevring deps (async I/O)
let evring-deps =
  [ "nixpkgs#nghttp2"         -- HTTP/2
  , ".#ngtcp2-libressl"       -- QUIC with LibreSSL (custom package)
  , "nixpkgs#nghttp3"         -- HTTP/3
  , "nixpkgs#liburing"        -- io_uring
  , "nixpkgs#llhttp"          -- HTTP/1.x parser
  ]

-- nix-language deps (WASM)
let language-deps =
  [ "nixpkgs#pegtl"           -- Parser combinators
  , "nixpkgs#binaryen"        -- WASM codegen
  ]

-- Test deps
let test-deps =
  [ "nixpkgs#catch2_3"
  , "nixpkgs#rapidcheck"
  ]

-- Benchmark deps
let bench-deps =
  [ "nixpkgs#nanobench"
  ]

-- All deps (for devshell)
let all-deps =
      util-deps
    # store-deps
    # fetchers-deps
    # expr-deps
    # main-deps
    # primitives-deps
    # evring-deps
    # language-deps
    # test-deps
    # bench-deps

in  { util-deps
    , store-deps
    , fetchers-deps
    , expr-deps
    , main-deps
    , primitives-deps
    , evring-deps
    , language-deps
    , test-deps
    , bench-deps
    , all-deps
    }
