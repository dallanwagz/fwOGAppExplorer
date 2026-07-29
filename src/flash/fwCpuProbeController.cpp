#include "flash/fwCpuProbeController.h"

#include "catalog/fwCatalogEmbedded.h"
#include "platform/fwPaths.h"
#include "platform/fwSerialPorts.h"
#include "platform/fwSerialTouch.h"
#include "platform/fwVolume.h"

#include <filesystem>
#include <fstream>
#include <span>
#include <utility>

namespace fwog {
namespace {

std::expected<std::filesystem::path, std::string>
stageProbeFile(std::span<const uint8_t> bytes, const std::string& filename)
{
    const auto path = tempDir() / filename;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return std::unexpected("could not create " + path.string());
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out)
        return std::unexpected("could not write " + path.string());
    return path;
}

} // namespace

ProbeIo makeProductionProbeIo(const CpuIdentity& identity)
{
    ProbeIo io;
    io.findVolumes = findRpiRp2Volumes;
    io.listPorts   = listSerialPorts;
    // The one place the platform's USB identities meet the prober's. The
    // knowledge of WHAT the prober enumerates as lives in fwCpuProbe.h next to
    // the rest of the prober's documentation, and the knowledge of how to ask
    // the OS lives in fwSerialPorts.cpp; this binds them and nothing else.
    io.listProberPorts = [] {
        std::vector<std::string> ports;
        for (const auto& info : listSerialPortInfo())
            if (looksLikeProberUsbId(info.usbId)) ports.push_back(info.port);
        return ports;
    };
    io.identify    = [identity] { return identity; };
    io.loadProbeImage = [] { return loadEmbeddedImage(kProbeImageId); };
    io.stageFile   = stageProbeFile;
    // Removes the staged temp file afterwards, exactly as
    // makeProductionFlashIo() does and for the same reason: copy_file() has
    // already produced an independent copy at the destination by the time it
    // returns, success or failure, and nothing else ever deletes the source.
    io.copyToVolume = [](const std::filesystem::path& src, const std::string& volume)
        -> std::expected<void, std::string> {
            auto result = copyToVolume(src, volume);
            std::error_code ec;
            std::filesystem::remove(src, ec);   // best-effort; result is what matters
            return result;
        };
    io.readLine = readSerialLine;
    io.waitTick = [](int intervalMs) {
        if (intervalMs > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
        return true;
    };
    return io;
}

CpuProbeController::CpuProbeController() = default;

CpuProbeController::~CpuProbeController()
{
    // Same trade as FlashController's destructor: signal, then join. Detaching
    // would leave a worker calling back into a destroyed `this`.
    m_cancelRequested.store(true, std::memory_order_relaxed);
    if (m_worker.joinable()) m_worker.join();
}

void CpuProbeController::begin(const CpuIdentity& identity)
{
    if (m_state == ProbeState::Running) return;   // one at a time
    if (m_worker.joinable()) m_worker.join();     // see FlashController::begin()

    m_identity = identity;
    m_log.clear();
    m_result = IdentifyResult{};
    m_succeededAt.reset();
    m_cancelRequested.store(false, std::memory_order_relaxed);
    m_state = ProbeState::Running;

    startWorker();
}

void CpuProbeController::startWorker()
{
    ProbeIo io = makeProductionProbeIo(m_identity);
    io.waitTick = [this](int intervalMs) {
        if (intervalMs > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
        return !m_cancelRequested.load(std::memory_order_relaxed);
    };

    m_worker = std::thread([this, io = std::move(io)]() mutable {
        auto result = identifyCpus(io, [this](const std::string& line) { enqueueProgress(line); });
        enqueueResult(result);
    });
}

void CpuProbeController::cancel()
{
    if (m_state == ProbeState::Running)
        m_cancelRequested.store(true, std::memory_order_relaxed);
}

void CpuProbeController::reset()
{
    if (m_state == ProbeState::Running) return;
    if (m_worker.joinable()) m_worker.join();
    m_state = ProbeState::Idle;
    m_result = IdentifyResult{};
    m_log.clear();
    m_succeededAt.reset();
}

void CpuProbeController::enqueueProgress(const std::string& line)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_queue.push_back(QueueEntry{ false, line, IdentifyResult{} });
}

void CpuProbeController::enqueueResult(const IdentifyResult& r)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_queue.push_back(QueueEntry{ true, std::string{}, r });
}

void CpuProbeController::poll()
{
    std::vector<QueueEntry> drained;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        drained.swap(m_queue);
    }
    for (const auto& entry : drained) {
        if (entry.isFinal) onFinished(entry.result);
        else               onProgress(entry.line);
    }
}

void CpuProbeController::onProgress(const std::string& line)
{
    m_log.push_back(line);
}

void CpuProbeController::onFinished(const IdentifyResult& result)
{
    m_result = result;
    m_state  = ProbeState::Finished;
    // The clock starts when the answer lands, and ONLY for an answer. A
    // failure leaves this empty, which is what makes identificationAge()
    // report nullopt and mappingStillFresh() refuse -- rather than there being
    // a separate "is it a success" test somewhere that could disagree.
    if (result.outcome == IdentifyOutcome::Success)
        m_succeededAt = std::chrono::steady_clock::now();
    else
        m_succeededAt.reset();

    if (!result.message.empty() && (m_log.empty() || m_log.back() != result.message))
        m_log.push_back(result.message);
}

std::optional<std::chrono::milliseconds> CpuProbeController::identificationAge() const
{
    if (!m_succeededAt.has_value()) return std::nullopt;
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - *m_succeededAt);
}

bool CpuProbeController::returnProberToBootsel()
{
    if (m_state != ProbeState::Finished) return false;
    if (m_result.outcome != IdentifyOutcome::Success) return false;
    if (m_result.proberPort.empty()) return false;

    touchPort1200(m_result.proberPort);
    // touchPort1200 reports nothing by design (see fwSerialTouch.h: the open
    // FAILS on Windows precisely when it worked), so the honest answer here is
    // "the touch was performed", not "the CPU rebooted". The user finds out by
    // watching for the volume, exactly as the flash engine does.
    m_log.push_back("touched " + m_result.proberPort + " at 1200 baud to return the " +
                    std::string(probeCpuName(m_result.proberCpu)) +
                    " CPU to its bootloader; watch for its RPI-RP2 volume to appear");
    return true;
}

} // namespace fwog
