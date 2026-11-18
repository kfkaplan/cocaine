#pragma once
// cocaine's menu bar logo (see art.cpp).

#include <string>

// Makes the copy of Leopard's art file with the Apple logos swapped for
// cocaine's, unless COCAINE_MENU_LOGO=apple. Run as each session starts.
void prepare_menu_logo(const std::string& sysroot);
// Where that copy is kept, in the system root.
std::string menu_logo_file(const std::string& sysroot);
