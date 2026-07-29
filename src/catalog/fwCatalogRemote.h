#pragma once

#include "core/fwTypes.h"

#include <atomic>
#include <expected>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fwog {

/// Fetches apps.json on a worker and caches it to the user data directory, so
/// the previous contents survive an offline start. A failed fetch is a status
/// line, never a modal.
class RemoteCatalog {
public:
    /// The one piece of real I/O this class performs: a blocking GET.
    /// Production leaves this at the default (real `httpGet`); tests inject
    /// a fake so the never-clear-on-failure rule can be verified without a
    /// network, matching the FlashIo injection pattern used by the flash
    /// engine.
    using FetchFn = std::function<std::expected<std::string, std::string>(const std::string& url)>;

    RemoteCatalog();
    explicit RemoteCatalog(FetchFn fetch);
    ~RemoteCatalog();

    /// Load whatever was cached last run. Cheap; call at startup.
    void loadCache();

    /// Begin a fetch on a worker thread. No-op while one is in flight.
    void start(const std::string& url);

    bool fetching() const;

    /// A snapshot of the entries. Safe to call from the UI thread each frame.
    std::vector<CatalogEntry> entries() const;

    /// Human-readable status for the UI: "", "updating...", or the error.
    std::string status() const;

    /// Join the worker. Call before process exit.
    void shutdown();

private:
    void run(std::string url);

    FetchFn m_fetch;

    mutable std::mutex        m_mutex;
    std::vector<CatalogEntry> m_entries;
    std::string               m_status;

    std::thread       m_worker;
    std::atomic<bool> m_fetching{ false };
};

} // namespace fwog
