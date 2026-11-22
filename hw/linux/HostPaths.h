// XDG paths of the Linux desktop platform: data ~/.local/share/<name> (<PREFIX>_DATA_DIR overrides it), settings
// $XDG_CONFIG_HOME/<name>/<name>.conf (default ~/.config), cache $XDG_CACHE_HOME/<name> (default ~/.cache). $HOME
// falls back to /tmp.
#pragma once

#include "Platform.h"

#include <string>

namespace platform::host
{
Paths xdgPaths();
std::string envOr(const std::string& key, const std::string& fallback); // the variable when set and non-empty
} // namespace platform::host
