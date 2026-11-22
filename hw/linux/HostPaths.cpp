#include "linux/HostPaths.h"

#include "ProjectIdentity.h"

#include <cstdlib>

namespace platform::host
{
std::string envOr(const std::string& key, const std::string& fallback)
{
    const char* v = std::getenv(key.c_str());
    return v != nullptr && *v != 0 ? std::string(v) : fallback;
}

Paths xdgPaths()
{
    const std::string home = envOr("HOME", "/tmp");
    const std::string name = identity::name;
    Paths p;
    p.dataDir = envOr(std::string(identity::envPrefix) + "_DATA_DIR", home + "/.local/share/" + name);
    p.settingsFile = envOr("XDG_CONFIG_HOME", home + "/.config") + "/" + name + "/" + name + ".conf";
    p.cacheDir = envOr("XDG_CACHE_HOME", home + "/.cache") + "/" + name;
    return p;
}
} // namespace platform::host
