#pragma once

#include "core/fwTypes.h"

#include <vector>

namespace fwog {

/// Merge the three catalog sources into one display list.
///
/// Precedence: embedded > local > remote. The embedded images are the recovery
/// path, so nothing downloaded may silently replace them. Within a source, the
/// first entry for a slug wins. Result is sorted by name, case-insensitively.
std::vector<CatalogEntry> mergeCatalogs(std::vector<CatalogEntry> embedded,
                                        std::vector<CatalogEntry> local,
                                        std::vector<CatalogEntry> remote);

} // namespace fwog
