#pragma once

namespace fwog {

/// Which real refusal or board-failure state a Recovery section documents.
/// The flash engine's refusals (Task 19) carry one of these so a user who
/// just hit a refusal can be taken straight to the matching explanation via
/// TabRecovery::scrollTo() instead of hunting for it.
///
/// Every fwog::FlashOutcome (src/flash/fwFlashEngine.h) that a user can hit
/// must map to one of these, or deliberately to none -- see that enum's
/// comment for the full list. Two of these anchors are CPU-specific
/// (DisplayNotIdentified / MainNotIdentified) because RefusedUnidentified
/// and Timeout are produced identically for either CPU: the caller mapping
/// FlashOutcome -> RecoveryAnchor must pick the one that matches which CPU
/// actually failed. Attaching DisplayNotIdentified to a MAIN-side failure
/// tells the user things that are not true of their situation -- no
/// BOOTSEL button, 1200-baud reboot, FWOG_DIAG -- none of which applies to
/// the MAIN CPU, which has a reachable BOOTSEL button instead.
enum class RecoveryAnchor {
    TwoVolumes,
    DisplayNotIdentified,
    MainNotIdentified,
    BootloaderMissing,
    WrongImageOnWrongCpu,
    ImageRejected,
    WriteInterrupted,
    PartialLegacyFlash,
    NothingEnumerates,
    /// Task 22. NOT produced by recoveryAnchorFor()/any FlashOutcome -- this
    /// state is caught earlier, by flashDisabledReason() disabling the Flash
    /// button before a flash can even be attempted (see its header comment,
    /// fwCatalogFilter.h), so it is reached only by a user reading that
    /// button's disabled-reason text or browsing this tab directly. Distinct
    /// from DisplayNotIdentified/MainNotIdentified: those are about a CPU's
    /// own RP2040 serial port not being found; this is about the BOARD's own
    /// identifying serial (read from a separate FTDI chip) not being found,
    /// which does not stop either CPU's port from being identified.
    BoardSerialUnidentified,
};

/// Title of the BoardSerialUnidentified section, shared with
/// flashDisabledReason()'s (src/catalog/fwCatalogFilter.cpp) disabled-reason
/// message so the two can never drift apart -- same reasoning as
/// kEraseMainCpuSlug (fwFlashPlan.h): one source of truth for a string
/// otherwise typed out twice. fwCatalogFilter.cpp includes this header for
/// exactly this constant; src/flash/fwFlashController.h already sets the
/// precedent that this header -- content only, no ImGui dependency -- is
/// fair game to include outside src/ui/.
inline constexpr const char* kBoardSerialUnidentifiedTitle =
    "The board's serial number could not be read";

struct RecoverySection {
    RecoveryAnchor anchor;
    const char*    title;
    const char*    body;
};

/// Defined in fwTabRecovery.cpp -- the Recovery tab is the only consumer of
/// the prose itself, so it is kept next to the code that renders it rather
/// than split into a separate translation unit. This header carries only the
/// contract: the anchor enum Task 19 needs, and the shape of a section.
extern const RecoverySection kRecoverySections[];
extern const int             kRecoverySectionCount;

} // namespace fwog
