// Error reporting shared by every compiler stage.
#pragma once

#include <string>

#include "ast.h"

namespace nova {

class Diagnostics {
 public:
  void setFile(std::string file) { file_ = std::move(file); }
  void error(Loc loc, const std::string &msg);
  int errorCount() const { return errors_; }
  bool hasErrors() const { return errors_ > 0; }

 private:
  std::string file_ = "<input>";
  int errors_ = 0;
};

Diagnostics &diag();

}  // namespace nova
