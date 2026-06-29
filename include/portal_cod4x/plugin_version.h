#pragma once

#include <string_view>

#ifndef PORTAL_COD4X_PLUGIN_VERSION
#define PORTAL_COD4X_PLUGIN_VERSION "0.1.0-local"
#endif

#ifndef PORTAL_COD4X_PLUGIN_VERSION_MAJOR
#define PORTAL_COD4X_PLUGIN_VERSION_MAJOR 0
#endif

#ifndef PORTAL_COD4X_PLUGIN_VERSION_MINOR
#define PORTAL_COD4X_PLUGIN_VERSION_MINOR 1
#endif

namespace portal_cod4x
{
inline constexpr int kPluginVersionMajor = PORTAL_COD4X_PLUGIN_VERSION_MAJOR;
inline constexpr int kPluginVersionMinor = PORTAL_COD4X_PLUGIN_VERSION_MINOR;
inline constexpr std::string_view kPluginSemanticVersion = PORTAL_COD4X_PLUGIN_VERSION;
}
