#include "device/fwBoardIdentify.h"

#include "device/fwCpuIdentify.h"
#include "device/fwDeviceRecords.h"

#ifndef __EMSCRIPTEN__
#include <fwfinder.hpp>
#endif

namespace fwog {

std::optional<CpuIdentity> identifyBoardNow(uint64_t uniqueID)
{
#ifdef __EMSCRIPTEN__
    (void)uniqueID;
    return std::nullopt;
#else
    auto found = Fw::find_all();
    if (!found) return std::nullopt;
    for (const auto& dev : *found)
        if (dev.uniqueID == uniqueID)
            return identifyCpus(toCpuPortRecords(dev));
    return std::nullopt;
#endif
}

CpuIdentity withProbeVolumesFrom(CpuIdentity live, const CpuIdentity& snapshot)
{
    if (!live.mainVolume && snapshot.mainVolume
        && snapshot.mainSource == IdentitySource::VerifiedProbe && !live.mainPort) {
        live.mainVolume = snapshot.mainVolume;
        live.mainSource = IdentitySource::VerifiedProbe;
    }
    if (!live.displayVolume && snapshot.displayVolume
        && snapshot.displaySource == IdentitySource::VerifiedProbe && !live.displayPort) {
        live.displayVolume = snapshot.displayVolume;
        live.displaySource = IdentitySource::VerifiedProbe;
    }
    return live;
}

} // namespace fwog
