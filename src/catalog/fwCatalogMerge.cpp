#include "catalog/fwCatalogMerge.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace fwog {
namespace {

std::string lower(std::string s)
{
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

void take(std::vector<CatalogEntry>& out,
          std::unordered_set<std::string>& seen,
          std::vector<CatalogEntry>&& src)
{
    for (auto& e : src)
        if (seen.insert(e.slug).second)
            out.push_back(std::move(e));
}

} // namespace

std::vector<CatalogEntry> mergeCatalogs(std::vector<CatalogEntry> embedded,
                                        std::vector<CatalogEntry> local,
                                        std::vector<CatalogEntry> remote)
{
    std::vector<CatalogEntry> out;
    out.reserve(embedded.size() + local.size() + remote.size());

    std::unordered_set<std::string> seen;
    take(out, seen, std::move(embedded));
    take(out, seen, std::move(local));
    take(out, seen, std::move(remote));

    // stable_sort, not sort: dedup is by slug, so two entries with different
    // slugs can share a name and reach here as ties. The input is already in
    // precedence order (embedded, then local, then remote), so stability breaks
    // name ties in that same order instead of leaving it unspecified.
    std::stable_sort(out.begin(), out.end(),
                     [](const CatalogEntry& a, const CatalogEntry& b) {
                         return lower(a.name) < lower(b.name);
                     });
    return out;
}

} // namespace fwog
