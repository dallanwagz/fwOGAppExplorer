#include "catalog/fwCatalogRemote.h"

#include "catalog/fwCatalogJson.h"
#include "platform/fwHttp.h"
#include "platform/fwPaths.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace fwog {
namespace {

constexpr const char* kCacheFile = "apps-cache.json";

std::string readFile(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

RemoteCatalog::RemoteCatalog() : RemoteCatalog(httpGet) {}

RemoteCatalog::RemoteCatalog(FetchFn fetch) : m_fetch(std::move(fetch)) {}

// start()/shutdown() own m_worker and are only ever called from the single
// controller (UI) thread, never from the worker itself and never
// concurrently with one another -- so joining it here races with nothing.
// A fetch in flight is joined, not detached: the destructor blocks until the
// worker actually finishes rather than risk it touching a dead `this`.
RemoteCatalog::~RemoteCatalog()
{
    shutdown();
}

void RemoteCatalog::loadCache()
{
    const auto path = userDataDir() / kCacheFile;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return;

    auto parsed = parseCatalogJson(readFile(path), CatalogSource::Remote);
    if (!parsed) return;   // a corrupt cache is no worse than an empty one,
                            // but must not clobber whatever the caller already
                            // has -- same ordering rule as a failed fetch

    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries = std::move(*parsed);
}

void RemoteCatalog::start(const std::string& url)
{
    // exchange(true) is the whole guard: if another fetch is already in
    // flight this flips nothing and returns, so a second start() is a no-op
    // rather than a second worker racing the first over m_worker.
    if (m_fetching.exchange(true)) return;

    // The previous worker (if any) is guaranteed finished by now -- run()
    // only clears m_fetching as its last act -- so this join is immediate.
    // It is still required: assigning to a std::thread that still holds a
    // joinable OS thread calls std::terminate.
    if (m_worker.joinable()) m_worker.join();
    m_worker = std::thread(&RemoteCatalog::run, this, url);
}

bool RemoteCatalog::fetching() const
{
    return m_fetching.load();
}

std::vector<CatalogEntry> RemoteCatalog::entries() const
{
    // Every read and write of m_entries goes through m_mutex -- this is the
    // one lock that covers both m_entries and m_status, taken here, in
    // status(), and in every branch of run()/loadCache() that touches them.
    // Returning a copy (not a reference) means the lock never needs to
    // outlive this call.
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entries;
}

std::string RemoteCatalog::status() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

void RemoteCatalog::shutdown()
{
    // joinable() is false for a default-constructed thread and for one
    // already joined, so a second call -- or a call when start() was never
    // used -- is simply a no-op, not a double-join crash.
    if (m_worker.joinable()) m_worker.join();
}

void RemoteCatalog::run(std::string url)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status = "updating...";
    }

    auto body = m_fetch(url);
    if (!body) {
        // Ordering rule that matters: m_entries is untouched here. A failed
        // refresh leaves the user looking at the catalog they already had.
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status = body.error();
        m_fetching = false;
        return;
    }

    auto parsed = parseCatalogJson(*body, CatalogSource::Remote);
    if (!parsed) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status = parsed.error();
        m_fetching = false;
        return;
    }

    // Cache the raw body so a later offline launch still has this catalog.
    // Caching is best-effort: a write failure here must not turn a
    // successful fetch into a reported failure.
    std::error_code ec;
    const auto dir = userDataDir();
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir / kCacheFile, std::ios::binary);
    if (out) out << *body;

    // A successful fetch that parses to zero apps still replaces m_entries,
    // including when that means going from N entries to 0. "Never clear on
    // failure" is about failure -- a transport error or a document that
    // doesn't parse -- not about the *content* of a document that parsed
    // fine. An empty "apps" array is a valid, deliberate server state (e.g.
    // mid-rebuild), and by the time control reaches here httpGet and
    // parseCatalogJson have both already succeeded, so there is no signal
    // left to call this an error on. Treating "empty" as a third kind of
    // failure would need an arbitrary threshold this code has no basis for.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries = std::move(*parsed);   // only replace after a parse succeeds
        m_status.clear();
    }
    m_fetching = false;
}

} // namespace fwog
