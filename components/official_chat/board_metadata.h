#pragma once

#include <string>

namespace official_chat {

class BoardMetadata {
 public:
  std::string type;
  std::string name;
  bool has_display = false;
  bool display_monochrome = false;
  int display_width = 0;
  int display_height = 0;
};

BoardMetadata LoadCurrentBoardMetadata();

}  // namespace official_chat
