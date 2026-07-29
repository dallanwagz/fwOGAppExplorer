#include <doctest/doctest.h>
#include "catalog/fwCatalogRemote.h"
#include "platform/fwPaths.h"

#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

using namespace fwog;

namespace {

/// start()+shutdown() back to back makes a fetch synchronous for a test:
/// shutdown() joins the worker, so by the time it returns, run() has
/// definitely finished and m_entries/m_status reflect that one fetch. This
/// is only safe because RemoteCatalog documents shutdown() as callable more
/// than once and start() as reusable afterward -- see fwCatalogRemote.cpp.
void runSync(RemoteCatalog& cat, const std::string& url = "http://example.invalid/apps.json") {
    cat.start(url);
    cat.shutdown();
}

/// Saves and restores whatever apps-cache.json actually contained before the
/// test, so tests that exercise a real fetch (which caches to disk) or a
/// corrupt-cache scenario never leave the real per-user cache file polluted
/// with fake test content for a later real run of the app to pick up.
class CacheFileGuard {
public:
    CacheFileGuard() : m_path(userDataDir() / "apps-cache.json") {
        std::ifstream in(m_path, std::ios::binary);
        if (in) {
            std::ostringstream ss;
            ss << in.rdbuf();
            m_hadContent = true;
            m_original = ss.str();
        }
    }
    ~CacheFileGuard() {
        if (m_hadContent) {
            std::ofstream out(m_path, std::ios::binary);
            out << m_original;
        } else {
            std::error_code ec;
            std::filesystem::remove(m_path, ec);
        }
    }
private:
    std::filesystem::path m_path;
    bool m_hadContent = false;
    std::string m_original;
};

} // namespace

TEST_CASE("a failed fetch leaves previously loaded entries intact and sets a non-empty status") {
    CacheFileGuard guard;
    auto callCount = std::make_shared<int>(0);

    RemoteCatalog cat([callCount](const std::string&) -> std::expected<std::string, std::string> {
        ++*callCount;
        if (*callCount == 1)
            return std::string(R"({"apps":[{"slug":"a","name":"A App"}]})");
        return std::unexpected(std::string("network unreachable"));
    });

    runSync(cat);   // call 1: succeeds, seeds one entry
    REQUIRE(cat.entries().size() == 1);
    CHECK(cat.entries()[0].slug == "a");

    runSync(cat);   // call 2: fails

    CHECK(cat.entries().size() == 1);
    CHECK(cat.entries()[0].slug == "a");
    CHECK(cat.status() == "network unreachable");
}

TEST_CASE("a fetch that returns unparseable JSON likewise leaves entries intact") {
    CacheFileGuard guard;
    auto callCount = std::make_shared<int>(0);

    RemoteCatalog cat([callCount](const std::string&) -> std::expected<std::string, std::string> {
        ++*callCount;
        if (*callCount == 1)
            return std::string(R"({"apps":[{"slug":"a","name":"A App"}]})");
        return std::string("this is not json");
    });

    runSync(cat);   // call 1: succeeds, seeds one entry
    REQUIRE(cat.entries().size() == 1);

    runSync(cat);   // call 2: transport succeeds, parse fails

    CHECK(cat.entries().size() == 1);
    CHECK(cat.entries()[0].slug == "a");
    CHECK_FALSE(cat.status().empty());
}

TEST_CASE("a successful fetch replaces the entries") {
    CacheFileGuard guard;
    auto callCount = std::make_shared<int>(0);

    RemoteCatalog cat([callCount](const std::string&) -> std::expected<std::string, std::string> {
        ++*callCount;
        if (*callCount == 1)
            return std::string(R"({"apps":[{"slug":"a","name":"A App"}]})");
        return std::string(R"({"apps":[{"slug":"b","name":"B App"},{"slug":"c","name":"C App"}]})");
    });

    runSync(cat);
    REQUIRE(cat.entries().size() == 1);
    CHECK(cat.entries()[0].slug == "a");

    runSync(cat);   // a genuinely different successful document

    auto entries = cat.entries();
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].slug == "b");
    CHECK(entries[1].slug == "c");
    CHECK(cat.status().empty());
}

TEST_CASE("a successful fetch that yields zero apps replaces the entries with an empty catalog") {
    // Design decision: "never clear on failure" is about failure -- a
    // transport error or a document that fails to parse. An empty "apps"
    // array is a document that parsed just fine; by the time control gets
    // here httpGet and parseCatalogJson have both already succeeded, so
    // there is no signal left in this code to call that an error. Treating
    // "zero entries" as a third kind of failure would need a threshold this
    // code has no basis for choosing, and it would mean a server that
    // legitimately empties its catalog (e.g. mid-rebuild) could never be
    // reflected -- the user would keep seeing stale entries forever. So an
    // empty parse result DOES replace m_entries, same as any other success.
    CacheFileGuard guard;
    auto callCount = std::make_shared<int>(0);

    RemoteCatalog cat([callCount](const std::string&) -> std::expected<std::string, std::string> {
        ++*callCount;
        if (*callCount == 1)
            return std::string(R"({"apps":[{"slug":"a","name":"A App"}]})");
        return std::string(R"({"apps":[]})");
    });

    runSync(cat);
    REQUIRE(cat.entries().size() == 1);

    runSync(cat);

    CHECK(cat.entries().empty());
    CHECK(cat.status().empty());   // an empty catalog is not an error
}

TEST_CASE("loadCache with a corrupt cache file leaves entries intact and does not throw") {
    CacheFileGuard guard;
    auto callCount = std::make_shared<int>(0);

    RemoteCatalog cat([callCount](const std::string&) -> std::expected<std::string, std::string> {
        ++*callCount;
        return std::string(R"({"apps":[{"slug":"a","name":"A App"}]})");
    });

    runSync(cat);
    REQUIRE(cat.entries().size() == 1);

    // Corrupt the real per-user cache file loadCache() reads.
    const auto cachePath = userDataDir() / "apps-cache.json";
    std::ofstream out(cachePath, std::ios::binary);
    out << "{ this is not valid json at all";
    out.close();

    CHECK_NOTHROW(cat.loadCache());

    CHECK(cat.entries().size() == 1);
    CHECK(cat.entries()[0].slug == "a");
}
