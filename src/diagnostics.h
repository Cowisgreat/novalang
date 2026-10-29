// Error reporting shared by every compiler stage.
#pragma once

#include <string>
#include <vector>

#include "ast.h"

namespace nova {

class Diagnostics {
 public:
  void setFile(std::string file) { file_ = std::move(file); }
  // Source lines, used to print the offending line under each error.
  void setSource(const std::string &text);
  void error(Loc loc, const std::string &msg);
  void note(Loc loc, const std::string &msg);
  int errorCount() const { return errors_; }
  bool hasErrors() const { return errors_ > 0; }

 private:
  void report(const char *severity, Loc loc, const std::string &msg);

  std::string file_ = "<input>";
  std::vector<std::string> lines_;
  int errors_ = 0;
};

Diagnostics &diag();

}  // namespace nova
