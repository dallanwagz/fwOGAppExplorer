#pragma once

#include "core/fwTypes.h"
#include "flash/fwCpuProbe.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace fwog {

/// Binds ProbeIo to the real platform: fwog::findRpiRp2Volumes,
/// fwog::listSerialPorts, fwog::readSerialLine, fwog::copyToVolume,
/// fwog::tempDir and loadEmbeddedImage(kProbeImageId).
///
/// `identity` must already be a SNAPSHOT (DeviceModel::selected()->identity,
/// copied out by the caller) for exactly the reason makeProductionFlashIo()
/// documents: the ProbeIo this returns is handed to a worker thread and
/// DeviceModel is rebuilt once per frame by the UI thread alone.
///
/// `waitTick` sleeps and always returns true; CpuProbeController replaces it
/// with a cancellation-aware version bound to its own flag before ever running
/// this ProbeIo -- same arrangement, same reason, as FlashController.
ProbeIo makeProductionProbeIo(const CpuIdentity& identity);

enum class ProbeState {
    Idle,      ///< nothing running, nothing to report
    Running,   ///< a worker thread is executing identifyCpus()
    Finished,  ///< a result has landed -- successful or not; see result()
};

/// Owns the worker thread that runs one CPU identification at a time, and the
/// mutex-guarded queue the UI drains via poll(). Nothing here touches ImGui;
/// fwTabRecovery.cpp is the widget that reads this.
///
/// Deliberately a SEPARATE controller from FlashController rather than another
/// mode inside it. Identification is not a flash: it writes one fixed embedded
/// image, to a volume nobody has identified, under a rule that applies to that
/// image alone (see identifyCpus()'s copy site). Folding it into the flash
/// controller would put "may write to an unidentified CPU" inside the class
/// whose entire job is to never do that.
class CpuProbeController {
public:
    CpuProbeController();
    ~CpuProbeController();

    CpuProbeController(const CpuProbeController&) = delete;
    CpuProbeController& operator=(const CpuProbeController&) = delete;

    /// Starts an identification against `identity` (a snapshot -- see
    /// makeProductionProbeIo). No-op while state() == Running. Every other
    /// state starts fresh and discards the previous result.
    void begin(const CpuIdentity& identity);

    /// Requests cancellation. Cooperative, like FlashController::cancel():
    /// the flag is polled by waitTick, so a copy or a port read already in
    /// flight finishes first.
    void cancel();

    /// Returns to Idle, discarding the previous result and log. No-op while
    /// Running.
    void reset();

    /// Drains whatever the worker queued since the last call. Never blocks.
    /// Call once per UI frame regardless of state().
    void poll();

    ProbeState state() const { return m_state; }
    const std::vector<std::string>& log() const { return m_log; }
    const IdentifyResult& result() const { return m_result; }

    /// How long ago a SUCCESSFUL identification landed, or nullopt when there
    /// is none (including while one is running, and after a failure). This is
    /// the freshness input mappingStillFresh() gates on, and nullopt is the
    /// fail-closed answer there.
    std::optional<std::chrono::milliseconds> identificationAge() const;

    /// 1200-baud touch the prober's CPU back into its bootloader, so the real
    /// firmware can be written to it.
    ///
    /// This is the other half of not creating a NEW dead end: a CPU left
    /// running the prober publishes no FWOG_* product string, so the ordinary
    /// flash path cannot identify it and cannot reboot it. Without this button
    /// the recovery flow would trade one stuck state for another.
    ///
    /// No-op unless there is a successful result to act on. Returns false if
    /// there was nothing to do. Runs SYNCHRONOUSLY on the calling thread:
    /// touchPort1200() opens a port and closes it, which is a handful of
    /// milliseconds, and the alternative -- a second worker thread for one
    /// CreateFile call -- would be more machinery than the thing it manages.
    ///
    /// The CALLER must not offer this while the other volume is still mounted:
    /// touching now would put a second RPI-RP2 volume back on the bus and
    /// recreate the two-volume state the user is trying to escape. See
    /// fwTabRecovery.cpp, where that gate lives, and note that it is a
    /// sequencing gate rather than a safety one -- nothing unsafe happens if
    /// it is ignored, the user simply has to start over.
    bool returnProberToBootsel();

    // --- Applied directly by poll(); exercised directly by tests so the state
    // machine is verifiable without a real worker thread. ---
    void onProgress(const std::string& line);
    void onFinished(const IdentifyResult& result);

private:
    struct QueueEntry {
        bool           isFinal = false;
        std::string    line;
        IdentifyResult result;
    };

    void startWorker();
    void enqueueProgress(const std::string& line);
    void enqueueResult(const IdentifyResult& r);

    ProbeState               m_state = ProbeState::Idle;
    IdentifyResult           m_result;
    std::vector<std::string> m_log;
    /// When a SUCCESSFUL result was applied. Reset whenever the result is not
    /// a success, so identificationAge() cannot report an age for a mapping
    /// that does not exist.
    std::optional<std::chrono::steady_clock::time_point> m_succeededAt;

    CpuIdentity m_identity;

    std::atomic<bool> m_cancelRequested{ false };
    std::thread       m_worker;

    std::mutex              m_queueMutex;
    std::vector<QueueEntry> m_queue;
};

} // namespace fwog
