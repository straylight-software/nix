{- dhall/package.dhall

   straylight/nix build definitions.
   
   Import sensenet types and define project-specific targets.
-}

let sensenet = https://raw.githubusercontent.com/straylight-software/sensenet/dev/dhall/package.dhall
  sha256:0000000000000000000000000000000000000000000000000000000000000000

-- Re-export sensenet types
let Build = sensenet.Build
let Resource = sensenet.Resource
let Toolchain = sensenet.Toolchain
let Triple = sensenet.Triple
let CFlags = sensenet.CFlags
let LDFlags = sensenet.LDFlags

-- Project-specific toolchain: C++23 with straylight defaults
let straylight-toolchain
    : Toolchain.Toolchain
    = Toolchain.clang
        { version = "19"
        , host = Triple.x86_64-linux-gnu
        , target = Triple.x86_64-linux-gnu
        , cflags = 
            [ CFlags.std.cxx23
            , CFlags.opt.O2
            , CFlags.warn.all
            , CFlags.warn.extra
            ]
        , ldflags = [] : List LDFlags.LDFlag
        }

-- nix-util library
let util = Build.cxx-library
  { name = "util"
  , srcs = 
      [ "src/nix/util/*.cpp"
      , "src/nix/util/unix/*.cpp"
      , "src/nix/util/signature/*.cpp"
      ]
  , deps =
      [ Build.dep.nixpkgs "boost"
      , Build.dep.nixpkgs "nlohmann_json"
      , Build.dep.nixpkgs "libressl"
      , Build.dep.nixpkgs "libblake3"
      , Build.dep.nixpkgs "brotli"
      , Build.dep.nixpkgs "libsodium"
      , Build.dep.nixpkgs "libarchive"
      ]
  , toolchain = straylight-toolchain
  , requires = Resource.pure
  }

-- nix-store library
let store = Build.cxx-library
  { name = "store"
  , srcs = [ "src/nix/store/*.cpp" ]
  , deps =
      [ Build.dep.local ":util"
      , Build.dep.nixpkgs "sqlite"
      , Build.dep.nixpkgs "curl"
      ]
  , toolchain = straylight-toolchain
  , requires = Resource.filesystem "/nix/store"
  }

-- nix-fetchers library
let fetchers = Build.cxx-library
  { name = "fetchers"
  , srcs = [ "src/nix/fetchers/*.cpp" ]
  , deps =
      [ Build.dep.local ":util"
      , Build.dep.local ":store"
      , Build.dep.nixpkgs "libgit2"
      ]
  , toolchain = straylight-toolchain
  , requires = Resource.network
  }

-- nix-expr library  
let expr = Build.cxx-library
  { name = "expr"
  , srcs = [ "src/nix/expr/*.cpp" ]
  , deps =
      [ Build.dep.local ":util"
      , Build.dep.local ":store"
      , Build.dep.local ":fetchers"
      , Build.dep.nixpkgs "boehmgc"
      , Build.dep.nixpkgs "toml11"
      ]
  , toolchain = straylight-toolchain
  , requires = Resource.pure
  }

-- nix-flake library
let flake = Build.cxx-library
  { name = "flake"
  , srcs = [ "src/nix/flake/*.cpp" ]
  , deps =
      [ Build.dep.local ":util"
      , Build.dep.local ":store"
      , Build.dep.local ":fetchers"
      , Build.dep.local ":expr"
      ]
  , toolchain = straylight-toolchain
  , requires = Resource.network
  }

-- nix-main library
let main = Build.cxx-library
  { name = "main"
  , srcs = [ "src/nix/main/*.cpp" ]
  , deps =
      [ Build.dep.local ":util"
      , Build.dep.local ":store"
      , Build.dep.nixpkgs "editline"
      , Build.dep.nixpkgs "lowdown"
      ]
  , toolchain = straylight-toolchain
  , requires = Resource.pure
  }

-- nix-cmd library
let cmd = Build.cxx-library
  { name = "cmd"
  , srcs = [ "src/nix/cmd/*.cpp" ]
  , deps =
      [ Build.dep.local ":util"
      , Build.dep.local ":store"
      , Build.dep.local ":fetchers"
      , Build.dep.local ":expr"
      , Build.dep.local ":flake"
      , Build.dep.local ":main"
      ]
  , toolchain = straylight-toolchain
  , requires = Resource.pure
  }

-- nix CLI binary
let cli = Build.cxx-binary
  { name = "nix"
  , srcs = [ "src/nix/cli/*.cpp" ]
  , deps =
      [ Build.dep.local ":util"
      , Build.dep.local ":store"
      , Build.dep.local ":fetchers"
      , Build.dep.local ":expr"
      , Build.dep.local ":flake"
      , Build.dep.local ":main"
      , Build.dep.local ":cmd"
      ]
  , toolchain = straylight-toolchain
  , requires = Resource.pure
  }

-- straylight::language - Nix → WASM compiler
let language = Build.cxx-library
  { name = "language"
  , srcs = [ "src/straylight/language/**/*.cpp" ]
  , deps =
      [ Build.dep.nixpkgs "pegtl"
      , Build.dep.nixpkgs "binaryen"
      ]
  , toolchain = straylight-toolchain
  , requires = Resource.pure
  }

-- straylight::protocol - Formal protocol specs
let protocol = Build.cxx-library
  { name = "protocol"
  , srcs = [ "src/straylight/protocol/*.cpp" ]
  , deps = [] : List Build.Dep
  , toolchain = straylight-toolchain
  , requires = Resource.pure
  }

-- straylight::evring - Deterministic async I/O
let evring = Build.cxx-library
  { name = "evring"
  , srcs = [ "src/straylight/evring/*.cpp" ]
  , deps =
      [ Build.dep.nixpkgs "liburing"
      , Build.dep.nixpkgs "nghttp2"
      , Build.dep.nixpkgs "nghttp3"
      , Build.dep.nixpkgs "llhttp"
      , Build.dep.flake ".#ngtcp2-libressl"
      ]
  , toolchain = straylight-toolchain
  , requires = Resource.pure
  }

-- straylight::nix::primitives - Modern utility replacements
let primitives = Build.cxx-library
  { name = "primitives"
  , srcs = [ "src/straylight/nix/primitives/*.cpp" ]
  , deps =
      [ Build.dep.flake ".#stringzilla"
      , Build.dep.flake ".#zpp_bits"
      , Build.dep.nixpkgs "rapidfuzz-cpp"
      , Build.dep.nixpkgs "taskflow"
      ]
  , toolchain = straylight-toolchain
  , requires = Resource.pure
  }

-- All targets
let targets =
  { -- core nix
    util
  , store
  , fetchers
  , expr
  , flake
  , main
  , cmd
  , cli
  -- straylight
  , language
  , protocol
  , evring
  , primitives
  }

in  { -- Re-export sensenet
      Build
    , Resource
    , Toolchain
    , Triple
    , CFlags
    , LDFlags
    
    -- Project toolchain
    , straylight-toolchain
    
    -- Targets
    , targets
    }
