{- dhall/pre-commit.dhall

   Pre-commit hooks configuration.
   
   Generate .pre-commit-config.yaml with:
     dhall-to-yaml --file dhall/pre-commit.dhall > .pre-commit-config.yaml
-}

let Hook =
      { Type =
          { id : Text
          , name : Text
          , entry : Text
          , language : Text
          , types : Optional (List Text)
          , types_or : Optional (List Text)
          , files : Optional Text
          , exclude : Optional Text
          , pass_filenames : Optional Bool
          , always_run : Optional Bool
          , stages : Optional (List Text)
          }
      , default =
          { types = None (List Text)
          , types_or = None (List Text)
          , files = None Text
          , exclude = None Text
          , pass_filenames = None Bool
          , always_run = None Bool
          , stages = None (List Text)
          }
      }

let Repo =
      { Type = { repo : Text, hooks : List Hook.Type }
      , default = {=}
      }

-- Local hooks (run from dev shell)
let local-hooks =
      [ -- nix fmt (treefmt)
        Hook::{
        , id = "nix-fmt"
        , name = "nix fmt"
        , entry = "nix fmt -- --fail-on-change"
        , language = "system"
        , pass_filenames = Some False
        , always_run = Some True
        }
      , -- ast-grep errors only
        Hook::{
        , id = "ast-grep"
        , name = "ast-grep (errors)"
        , entry = "ast-grep scan --config sgconfig.yml"
        , language = "system"
        , types_or = Some [ "c", "c++" ]
        , files = Some "^src/straylight/"
        }
      , -- clang-tidy (requires compile_commands.json)
        Hook::{
        , id = "clang-tidy"
        , name = "clang-tidy"
        , entry = "scripts/lint"
        , language = "system"
        , types_or = Some [ "c", "c++" ]
        , files = Some "^src/"
        , stages = Some [ "pre-push" ]
        }
      ]

-- Pre-commit config structure
let config =
      { repos = [ { repo = "local", hooks = local-hooks } ]
      , default_install_hook_types = [ "pre-commit", "pre-push" ]
      , default_stages = [ "pre-commit" ]
      }

in  config
