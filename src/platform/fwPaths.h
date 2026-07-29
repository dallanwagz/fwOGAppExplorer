#pragma once

#include <filesystem>

namespace fwog {

/// Directory containing the running executable.
std::filesystem::path exeDir();

/// Per-user writable directory, created on first call.
/// Windows: %LOCALAPPDATA%/fwOGAppExplorer
/// Linux:   $XDG_DATA_HOME/fwOGAppExplorer, else ~/.local/share/fwOGAppExplorer
std::filesystem::path userDataDir();

/// System temp directory. Inflated embedded images land here.
std::filesystem::path tempDir();

/// The local catalog directory: `catalog/` beside the executable.
std::filesystem::path catalogDir();

} // namespace fwog
