#pragma once

#include "core/fwTypes.h"

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace fwog {

/// Parse an apps.json / catalog.json document.
///
/// Never throws. A malformed document is an error; a malformed *entry* inside
/// a well-formed document is skipped, because one bad entry must not cost the
/// user the whole catalog.
std::expected<std::vector<CatalogEntry>, std::string>
parseCatalogJson(std::string_view json, CatalogSource source);

} // namespace fwog
