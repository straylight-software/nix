#include "nix/store/content-address.h"
#include "nix/util/args.h"

namespace nix::flag {

args_t::flag_t hash_algo(std::string&& long_name, hash_algorithm_t* ha);

static inline args_t::flag_t hash_algo(hash_algorithm_t* ha) {
  return hash_algo("hash-algo", ha);
}

args_t::flag_t hash_algo_opt(std::string&& long_name, std::optional<hash_algorithm_t>* oha);
args_t::flag_t hash_format_with_default(std::string&& long_name, hash_format_t* hf);
args_t::flag_t hash_format_opt(std::string&& long_name, std::optional<hash_format_t>* ohf);

static inline args_t::flag_t hash_algo_opt(std::optional<hash_algorithm_t>* oha) {
  return hash_algo_opt("hash-algo", oha);
}

args_t::flag_t file_ingestion_method(file_ingestion_method_t* method);
args_t::flag_t content_address_method(content_address_method_t* method);

} // namespace nix::flag
