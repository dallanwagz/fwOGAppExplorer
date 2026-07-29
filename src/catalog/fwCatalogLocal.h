#pragma once

#include "catalog/fwUf2Header.h"
#include "core/fwTypes.h"

#include <filesystem>
#include <vector>

namespace fwog {

/// Describe a UF2 that catalog.json says nothing about, from its filename and
/// its parsed header. Nothing in the directory is hidden from the user.
CatalogEntry unlistedEntryFor(const std::filesystem::path& file, const Uf2Info& info);

/// Scan `dir` for catalog.json and loose .uf2 files. Described files come from
/// the JSON; the rest become Unlisted entries. A directory that does not exist
/// is not an error -- it just yields nothing.
///
/// NOT cheap: every loose .uf2 is read into memory IN FULL and every one of
/// its 512-byte blocks is walked by parseUf2(). A real display image is 16.4 MB
/// / 32,079 blocks, and dropping your own UF2 into `catalog/` is the documented
/// way to flash it -- so a UI that calls this once per frame reads and parses
/// tens of megabytes at frame rate. Callers that redraw continuously must go
/// through LocalCatalogCache below rather than calling this directly.
std::vector<CatalogEntry> loadLocalCatalog(const std::filesystem::path& dir);

/// loadLocalCatalog() behind a directory fingerprint, for callers that ask
/// every frame.
///
/// The App Explorer tab is redrawn at frame rate and its entry list is rebuilt
/// from scratch every frame -- deliberately, so that a UF2 dropped into
/// `catalog/` while the app is running appears without a restart. That is worth
/// keeping; what is not worth keeping is re-READING and re-PARSING every file
/// to find out whether anything changed.
///
/// So the expensive half is cached and only the cheap half repeats: entries()
/// stats the directory (name, size and last-write-time of each regular file,
/// which is metadata the OS already has) and re-runs loadLocalCatalog() only
/// when that fingerprint differs from the previous call's. Adding, removing,
/// touching or overwriting a file changes it, so a new file still shows up on
/// the very next frame -- no throttle, no "restart to see your file", and no
/// window in which the list is stale.
///
/// Deliberately NOT a time-based throttle: a throttle trades correctness for
/// cheapness in both directions -- it still rescans when nothing changed, and
/// it delays a change that did happen.
class LocalCatalogCache {
public:
    /// The catalog for `dir`, rescanning only if the directory's fingerprint
    /// changed since the last call. The reference is valid until the next
    /// call. Passing a DIFFERENT `dir` than last time always rescans.
    const std::vector<CatalogEntry>& entries(const std::filesystem::path& dir);

    /// How many times entries() has actually re-run loadLocalCatalog(). Exists
    /// so a test can assert the cache is doing its job -- an assertion on the
    /// returned entries alone cannot tell a cache hit from a rescan that
    /// happened to produce the same answer.
    std::size_t rescans() const { return m_rescans; }

private:
    /// One line per regular file in `dir`: name, size, last-write-time. Empty
    /// both when the directory does not exist and when it is empty; those two
    /// produce the same (empty) catalog, so they do not need telling apart.
    static std::string fingerprintOf(const std::filesystem::path& dir);

    std::filesystem::path     m_dir;
    std::string               m_fingerprint;
    std::vector<CatalogEntry> m_entries;
    bool                      m_primed = false;
    std::size_t               m_rescans = 0;
};

} // namespace fwog
