#pragma once

#include <filesystem>

namespace fwog {

/// Directory containing the running executable.
///
/// On Linux this is /proc/self/exe with the target's symlinks already resolved
/// (so an install that puts a symlink in /usr/bin pointing into /opt reports
/// /opt, where the app's own files actually are). A truncated read is never
/// passed off as a complete one.
///
/// When that link cannot be read -- no /proc, or an executable path longer
/// than PATH_MAX, which the kernel refuses outright -- this returns the
/// CURRENT WORKING DIRECTORY, which is very probably the wrong directory.
/// There is no error channel and no way for a caller to tell the two apart.
/// The consequence is that catalogDir() then points somewhere with no catalog
/// in it and the app shows an empty one; fwPaths.cpp states why that was
/// judged better than the alternatives and what would change the judgement.
std::filesystem::path exeDir();

/// Per-user writable directory, created on first call.
/// Windows: %LOCALAPPDATA%/fwOGAppExplorer
/// Linux:   $XDG_DATA_HOME/fwOGAppExplorer, else ~/.local/share/fwOGAppExplorer
///
/// On POSIX, $XDG_DATA_HOME and $HOME are ignored unless they are ABSOLUTE, as
/// the XDG Base Directory specification requires -- an empty or relative value
/// is treated as if the variable were unset, not honoured as a path relative
/// to wherever the app was started. If neither is usable the account database
/// (getpwuid_r) supplies the home directory before exeDir() is used as a last
/// resort. Windows reads %LOCALAPPDATA% and is unchanged.
std::filesystem::path userDataDir();

/// System temp directory. Inflated embedded images land here.
///
/// Falls back to userDataDir() when std::filesystem cannot supply one. On
/// POSIX it additionally refuses a temp directory that comes back RELATIVE --
/// which a $TMPDIR holding an existing relative path does, with no error
/// reported -- so staged firmware images cannot end up under the working
/// directory. That extra check is deliberately NOT applied on Windows, where
/// the equivalent could not be reproduced or even compiled; see fwPaths.cpp.
std::filesystem::path tempDir();

/// The local catalog directory: `catalog/` beside the executable.
std::filesystem::path catalogDir();

} // namespace fwog
