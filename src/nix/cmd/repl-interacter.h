#pragma once
/// @file

#include <functional>
#include <string>

#include "nix/util/finally.h"
#include "nix/util/types.h"

namespace nix {

namespace detail {
/** Provides the completion hooks for the repl, without exposing its complete
 * internals. */
struct ReplCompleterMixin {
  virtual string_set_t complete_prefix(const std::string& prefix) = 0;
};
}; // namespace detail

enum class ReplPromptType {
  ReplPrompt,
  ContinuationPrompt,
};

class ReplInteracter {
public:
  using Guard = finally_t<std::function<void()>>;

  virtual Guard init(detail::ReplCompleterMixin* repl) = 0;
  /** Returns a boolean of whether the interacter got EOF */
  virtual bool get_line(std::string& input, ReplPromptType prompt_type) = 0;
  virtual ~ReplInteracter() {};
};

class ReadlineLikeInteracter : public virtual ReplInteracter {
  std::string historyFile;

public:
  ReadlineLikeInteracter(std::string historyFile) : historyFile(historyFile) {}

  virtual Guard init(detail::ReplCompleterMixin* repl) override;
  virtual bool get_line(std::string& input, ReplPromptType prompt_type) override;
  virtual ~ReadlineLikeInteracter() override;
};

}; // namespace nix
