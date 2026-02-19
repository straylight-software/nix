#pragma once
///@file

#include <regex>
#include <string>

namespace nix {

// URI stuff.
const static std::string pct_encoded = "(?:%[0-9a-fA-F][0-9a-fA-F])";
const static std::string unreserved_regex = "(?:[a-zA-Z0-9-._~])";
const static std::string subdelims_regex = "(?:[!$&'\"()*+,;=])";
const static std::string pchar_regex =
    "(?:" + unreserved_regex + "|" + pct_encoded + "|" + subdelims_regex + "|[:@])";
const static std::string fragment_regex = "(?:" + pchar_regex + "|[/? \"^])*";

/// A Git ref (i.e. branch or tag name).
/// \todo check that this is correct.
/// This regex incomplete. See https://git-scm.com/docs/git-check-ref-format
const static std::string ref_regex_s = "[a-zA-Z0-9@][a-zA-Z0-9_.\\/@+-]*";
extern std::regex ref_regex;

/// A Git revision (a SHA-1 commit hash).
const static std::string rev_regex_s = "[0-9a-fA-F]{40}";
extern std::regex rev_regex;

/// A ref or revision, or a ref followed by a revision.
const static std::string ref_and_or_rev_regex =
    "(?:(" + rev_regex_s + ")|(?:(" + ref_regex_s + ")(?:/(" + rev_regex_s + "))?))";

} // namespace nix
