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
  virtual string_set_t completePrefix(const std::string& prefix) = 0;
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
  virtual bool getLine(std::string& input, ReplPromptType promptType) = 0;
  virtual ~ReplInteracter() {};
};

class ReadlineLikeInteracter : public virtual ReplInteracter {
  std::string historyFile;

public:
  ReadlineLikeInteracter(std::string historyFile) : historyFile(historyFile) {}

  virtual Guard init(detail::ReplCompleterMixin* repl) override;
  virtual bool getLine(std::string& input, ReplPromptType promptType) override;
  virtual ~ReadlineLikeInteracter() override;
};

}; // namespace nix
