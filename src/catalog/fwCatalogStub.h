#pragma once

#include "catalog/fwOgAppInfo.h"
#include "core/fwTypes.h"

#include <expected>
#include <filesystem>
#include <span>
#include <string>

namespace fwog {

/// The JSON text for a blank catalog entry describing one loose UF2.
///
/// PURE -- it touches no disk, so the exact text can be asserted directly.
///
/// `fileName` is the image's file name as it sits in the catalog folder, and is
/// written as a RELATIVE path: catalog.json resolves relative paths against its
/// own directory (loadLocalCatalog, fwCatalogLocal.cpp), so a stub written this
/// way keeps working when the folder is moved or copied to another machine.
///
/// `records` are whatever the image itself declares (findOgAppInfo). When a MAIN
/// record is present its name/description/version seed the entry, because an
/// image that already states what it is should not make the user retype it. The
/// point of the stub is the fields the image CANNOT know -- author, tags,
/// category, a human-facing name -- which are emitted empty for editing.
std::string blankCatalogEntryJson(const std::string& fileName,
                                  std::span<const OgAppInfo> records);

/// Write `entryJson` into `dir`/catalog.json, creating the file if it does not
/// exist and APPENDING to the existing "apps" array if it does.
///
/// Returns the path written on success.
///
/// REFUSES rather than overwrites in every ambiguous case: an existing
/// catalog.json that cannot be parsed, or that already describes this file, is
/// an error and not something to resolve by guessing. A user's hand-written
/// catalog is not this function's to rewrite.
std::expected<std::filesystem::path, std::string>
addBlankCatalogEntry(const std::filesystem::path& dir, const std::string& fileName,
                     std::span<const OgAppInfo> records);

} // namespace fwog
