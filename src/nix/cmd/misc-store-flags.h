#include "nix/store/content-address.h"
#include "nix/util/args.h"

namespace nix::flag {

Args::flag_t hashAlgo(std::string&& longName, hash_algorithm_t* ha);

static inline Args::flag_t hashAlgo(hash_algorithm_t* ha) {
  return hashAlgo("hash-algo", ha);
}

Args::flag_t hashAlgoOpt(std::string&& longName, std::optional<hash_algorithm_t>* oha);
Args::flag_t hashFormatWithDefault(std::string&& longName, hash_format_t* hf);
Args::flag_t hashFormatOpt(std::string&& longName, std::optional<hash_format_t>* ohf);

static inline Args::flag_t hashAlgoOpt(std::optional<hash_algorithm_t>* oha) {
  return hashAlgoOpt("hash-algo", oha);
}

Args::flag_t fileIngestionMethod(file_ingestion_method_t* method);
Args::flag_t contentAddressMethod(ContentAddressMethod* method);

} // namespace nix::flag
