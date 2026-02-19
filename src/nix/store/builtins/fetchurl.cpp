#include "nix/store/builtins.h"
#include "nix/store/filetransfer.h"
#include "nix/store/globals.h"
#include "nix/store/store-api.h"
#include "nix/util/archive.h"
#include "nix/util/compression.h"

namespace nix {

static void builtin_fetchurl(const BuiltinBuilderContext& ctx) {
  /* Make the host's netrc data available. Too bad curl requires
     this to be stored in a file. It would be nice if we could just
     pass a pointer to the data. */
  if (ctx.netrcData != "") {
    settings.netrcFile = "netrc";
    write_file(settings.netrcFile, ctx.netrcData, 0600);
  }

  settings.ca_file = "ca-certificates.crt";
  write_file(settings.ca_file, ctx.caFileData, 0600);

  auto out = get(ctx.drv.outputs, "out");
  if (!out)
    throw Error("'builtin:fetchurl' requires an 'out' output");

  if (!(ctx.drv.type().isFixed() || ctx.drv.type().is_impure()))
    throw Error("'builtin:fetchurl' must be a fixed-output or impure derivation");

  auto store_path = ctx.outputs.at("out");
  auto main_url = ctx.drv.env.at("url");
  bool unpack = get_or(ctx.drv.env, "unpack", "") == "1";

  /* Note: have to use a fresh file_transfer here because we're in
     a forked process. */
  debug("[pid=%d] builtin:fetchurl creating fresh FileTransfer instance", getpid());
  auto file_transfer = make_file_transfer();

  auto fetch = [&](const std::string& url) {
    auto source = sink_to_source([&](Sink& sink) {
      FileTransferRequest request(verbatim_url_t{url});
      request.decompress = false;

#if NIX_WITH_AWS_AUTH
      // Use pre-resolved credentials if available
      if (ctx.awsCredentials && request.uri.scheme() == "s3") {
        debug("[pid=%d] Using pre-resolved AWS credentials from parent process", getpid());
        request.usernameAuth = UsernameAuth{
            .username = ctx.awsCredentials->accessKeyId,
            .password = ctx.awsCredentials->secretAccessKey,
        };
        request.preResolvedAwsSessionToken = ctx.awsCredentials->sessionToken;
      }
#endif

      auto decompressor =
          make_decompression_sink(unpack && has_suffix(main_url, ".xz") ? "xz" : "none", sink);
      file_transfer->download(std::move(request), *decompressor);
      decompressor->finish();
    });

    if (unpack)
      restore_path(store_path, *source);
    else
      write_file(store_path, *source);

    auto executable = ctx.drv.env.find("executable");
    if (executable != ctx.drv.env.end() && executable->second == "1") {
      if (chmod(store_path.c_str(), 0755) == -1)
        throw sys_error_t("making '%1%' executable", store_path);
    }
  };

  /* Try the hashed mirrors first. */
  auto dof = std::get_if<DerivationOutput::CAFixed>(&out->raw);
  if (dof && dof->ca.method.getFileIngestionMethod() == file_ingestion_method_t::flat)
    for (auto hashedMirror : settings.hashedMirrors.get())
      try {
        if (!has_suffix(hashedMirror, "/"))
          hashedMirror += '/';
        fetch(hashedMirror + print_hash_algo(dof->ca.hash.algo) + "/" +
              dof->ca.hash.to_string(hash_format_t::base16, false));
        return;
      } catch (Error& e) {
        debug(e.what());
      }

  /* Otherwise try the specified URL. */
  fetch(main_url);
}

static RegisterBuiltinBuilder register_fetchurl("fetchurl", builtin_fetchurl);

} // namespace nix
