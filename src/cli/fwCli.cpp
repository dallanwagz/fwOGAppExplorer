// FreeWili OG command-line flasher.
//
// A thin console front-end over the EXACT same flash engine the GUI drives:
// identifyBoardNow() + quietDisplayBeforeMainWrite() + runFlashPlan() +
// makeProductionFlashIo(). Nothing here re-implements touching, waiting,
// volume disambiguation, the DISPLAY quieting or the wrong-CPU guards -- a
// scripted flash gets byte-for-byte the same behaviour as the desktop app,
// minus SDL/ImGui.
//
// Multiple boards: Fw::find_all() returns ONE FreeWiliDevice per physically
// connected board, each with a topological uniqueID (hub position). This tool
// identifies EACH board separately, lets you pick one, and tracks it by
// uniqueID across the BOOTSEL transitions a flash puts it through (a CPU
// dropping from firmware into its bootrom loses its serial port and its USB
// serial, but not its position on the board's own hub).
//
// Commands:
//   fwogcli list                             every connected board and its CPUs
//   fwogcli flash <file.uf2> [opts]          flash a UF2 (default target: main)
//     --cpu main|display                     which CPU (default main)
//   fwogcli install <slug> [opts]            run an embedded entry's full plan
//                                            (see `fwogcli entries`)
//   fwogcli entries                          the embedded entries and their plans
//   fwogcli info <file.uf2>                  what a UF2 says about itself
//   fwogcli bootsel main|display [--device]  reboot a running CPU into BOOTSEL
//                                            (the same 1200-baud touch a flash uses)
//   common options for flash/install:
//     --device <which>                       which board: its number in `list`,
//                                            or a substring of its serial / chip id
//                                            (required when more than one is connected)
//     --keep-display                         do not quiet the DISPLAY before a
//                                            MAIN install (see fwFlashPrep.h)
//     --yes                                  supply the typed CPU confirmation
//                                            the engine asks for on a drive it
//                                            cannot place by hub position
//
// Exit codes: 0 success; 1 flash failed; 2 usage/selection; 3 needs --yes.

#include "catalog/fwCatalogEmbedded.h"
#include "catalog/fwOgAppInfo.h"
#include "catalog/fwUf2Header.h"
#include "core/fwTypes.h"
#include "device/fwBoardIdentify.h"
#include "device/fwCpuIdentify.h"
#include "device/fwDeviceModel.h"      // BoardFingerprint, describeIdentity
#include "device/fwDeviceRecords.h"
#include "flash/fwFlashController.h"   // makeProductionFlashIo
#include "flash/fwFlashEngine.h"       // runFlashPlan
#include "flash/fwFlashPlan.h"         // buildFlashPlan, dropRedundantErases, planWarnings
#include "flash/fwFlashPrep.h"         // quietDisplayBeforeMainWrite
#include "platform/fwSerialTouch.h"    // touchPort1200

#include <fwfinder.hpp>

#include <chrono>
#include <thread>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace fwog;

const char* cpuName(TargetCpu c) { return c == TargetCpu::Main ? "main" : "display"; }

struct OgDevice {
    uint64_t         uniqueID = 0;   ///< hub-position handle; survives firmware<->bootrom
    std::string      serial;         ///< fwfinder's board serial ("Unknown" on an OG under OG firmware)
    std::string      name;
    CpuIdentity      id;             ///< this board's CPUs, identified in isolation
    BoardFingerprint print;          ///< what the board says about itself
};

bool deviceHasCpu(const CpuIdentity& id)
{
    return id.mainPort || id.mainVolume || id.displayPort || id.displayVolume;
}

// One scan -> one identity PER board. Each FreeWiliDevice's ports are flattened
// and identified on their own, so two connected boards never contaminate each
// other's identity.
std::vector<OgDevice> enumerateDevices()
{
    std::vector<OgDevice> out;
    if (auto r = Fw::find_all(); r.has_value()) {
        for (const auto& dev : *r) {
            CpuIdentity id = identifyCpus(toCpuPortRecords(dev));
            if (!deviceHasCpu(id)) continue;
            OgDevice d{ dev.uniqueID, dev.serial, dev.name, std::move(id), {} };
            d.print = fingerprintOf(d.serial, d.id);
            out.push_back(std::move(d));
        }
    }
    return out;
}

void printCpuLine(const char* label, const std::optional<std::string>& port,
                  const std::optional<std::string>& vol, const std::string& chip)
{
    std::printf("    %-8s", label);
    if (port)     std::printf(" running, port=%s", port->c_str());
    else if (vol) std::printf(" BOOTSEL, drive=%s", vol->c_str());
    else          std::printf(" not detected");
    if (!chip.empty()) std::printf("  (chip %s)", chip.c_str());
    std::printf("\n");
}

void printDevice(size_t index, const OgDevice& d)
{
    std::printf("%zu: %s%s\n", index + 1, describeFingerprint(d.print).c_str(),
                d.name.empty() ? "" : ("  [" + d.name + "]").c_str());
    printCpuLine("main", d.id.mainPort, d.id.mainVolume, d.id.mainChipSerial);
    printCpuLine("display", d.id.displayPort, d.id.displayVolume, d.id.displayChipSerial);
    switch (ogBootloaderState(d.id)) {
        case OgBootloaderState::Present: std::printf("    display bootloader: present\n"); break;
        case OgBootloaderState::Missing: std::printf("    display bootloader: MISSING (OG apps cannot run)\n"); break;
        case OgBootloaderState::Unknown: break;
    }
}

int cmdList()
{
    const auto devs = enumerateDevices();
    if (devs.empty()) { std::fprintf(stderr, "no FreeWili device found on USB\n"); return 2; }
    for (size_t i = 0; i < devs.size(); ++i) printDevice(i, devs[i]);
    return 0;
}

// Pick the board to act on. --device is the board's 1-based number in `list`,
// or a case-sensitive substring of its serial or either chip id, and must be
// unambiguous; with no --device, exactly one connected board is used and more
// than one is an error that lists the choices.
std::optional<OgDevice> selectDevice(std::string_view want)
{
    auto devs = enumerateDevices();
    if (devs.empty()) { std::fprintf(stderr, "no FreeWili device found on USB\n"); return std::nullopt; }

    if (want.empty()) {
        if (devs.size() == 1) return devs.front();
        std::fprintf(stderr,
            "%zu boards connected -- pick one with --device <number|serial|chip>:\n", devs.size());
        for (size_t i = 0; i < devs.size(); ++i)
            std::fprintf(stderr, "  %zu: %s\n", i + 1, describeFingerprint(devs[i].print).c_str());
        return std::nullopt;
    }

    // A plain number is a list index.
    if (!want.empty() && want.find_first_not_of("0123456789") == std::string_view::npos) {
        const size_t n = std::stoul(std::string(want));
        if (n >= 1 && n <= devs.size()) return devs[n - 1];
        std::fprintf(stderr, "--device %.*s: only %zu board(s) connected\n",
                     (int)want.size(), want.data(), devs.size());
        return std::nullopt;
    }

    std::vector<const OgDevice*> hits;
    for (const auto& d : devs) {
        const auto has = [&](const std::string& s) { return s.find(want) != std::string::npos; };
        if (has(d.print.serial) || has(d.print.mainChip) || has(d.print.displayChip) || has(d.name))
            hits.push_back(&d);
    }
    if (hits.empty()) {
        std::fprintf(stderr, "no connected board matches --device %.*s\n",
                     (int)want.size(), want.data());
        return std::nullopt;
    }
    if (hits.size() > 1) {
        std::fprintf(stderr, "--device %.*s matches %zu boards; be more specific:\n",
                     (int)want.size(), want.data(), hits.size());
        for (const auto* d : hits) std::fprintf(stderr, "  %s\n", describeFingerprint(d->print).c_str());
        return std::nullopt;
    }
    return *hits.front();
}

void printProgress(const FlashProgress& p)
{
    if (p.isRefresh) return;   // skip the periodic "still waiting" refreshes
    if (p.phase == FlashPhase::Preparing) {
        std::printf("[prep      ] %s\n", p.message.c_str());
    } else {
        std::printf("[%zu/%zu %-7s] %-13s %s\n",
                    p.stepIndex + 1, p.stepCount, cpuName(p.cpu),
                    flashPhaseLabel(p.phase), p.message.c_str());
    }
    std::fflush(stdout);
}

struct RunOptions {
    bool             assumeYes = false;
    bool             keepDisplay = false;
    std::string_view deviceSel;
};

// Run `plan` against the selected board through the shared engine: live
// identity by uniqueID, the same DISPLAY quieting the GUI does before a MAIN
// install (unless --keep-display), then runFlashPlan().
int runPlan(std::vector<FlashStep> plan, const std::string& what, const RunOptions& opt,
            const std::vector<std::string>& warnings)
{
    const auto dev = selectDevice(opt.deviceSel);
    if (!dev) return 2;
    std::printf("board %s\n", describeFingerprint(dev->print).c_str());
    std::printf("  %s\n", describeIdentity(dev->id).c_str());
    for (const auto& w : warnings) std::printf("note: %s\n", w.c_str());

    plan = dropRedundantErases(std::move(plan), dev->id);
    if (plan.empty()) { std::fprintf(stderr, "nothing to flash: the plan is empty\n"); return 2; }
    for (size_t i = 0; i < plan.size(); ++i)
        std::printf("plan %zu/%zu: %s -> %s\n", i + 1, plan.size(), cpuName(plan[i].cpu),
                    plan[i].description.c_str());
    std::fflush(stdout);

    // Live identity by hub position -- exactly what the GUI's worker uses.
    const FlashIo io = makeProductionFlashIo(dev->id, dev->uniqueID);

    if (!opt.keepDisplay) {
        const PrepResult prep = quietDisplayBeforeMainWrite(io, plan, printProgress);
        if (prep.outcome == PrepOutcome::Cancelled) { std::fprintf(stderr, "cancelled\n"); return 1; }
    }

    // --yes supplies the typed confirmation the engine asks for when a drive
    // it must write cannot be placed by hub position (or a probe). It is
    // supplied for whichever CPU asks; with hub position resolving the drives,
    // as it does on this hardware, it is never consulted.
    FlashResult res = runFlashPlan(io, plan, std::string_view{}, printProgress);
    if (res.outcome == FlashOutcome::NeedsConfirmation && opt.assumeYes && res.resumable
        && res.confirmationCpu) {
        const std::string_view confirm = *res.confirmationCpu == TargetCpu::Main ? "MAIN" : "DISPLAY";
        std::printf("--yes: confirming that drive is the %.*s CPU\n", (int)confirm.size(), confirm.data());
        res = runFlashPlan(io, plan, confirm, printProgress, res.stepsCompleted);
    }

    switch (res.outcome) {
        case FlashOutcome::Success:
            std::printf("OK: %s\n", what.c_str());
            return 0;
        case FlashOutcome::NeedsConfirmation:
            std::fprintf(stderr,
                "%s\nThe %s CPU's drive could not be placed by hub position. Re-run with --yes "
                "to confirm it.\n", res.message.c_str(),
                res.confirmationCpu ? cpuName(*res.confirmationCpu) : "target");
            return 3;
        default:
            std::fprintf(stderr, "flash failed: %s\n", res.message.c_str());
            return 1;
    }
}

int cmdFlash(const std::string& uf2, TargetCpu cpu, const RunOptions& opt)
{
    std::error_code ec;
    if (!std::filesystem::exists(uf2, ec)) {
        std::fprintf(stderr, "no such file: %s\n", uf2.c_str());
        return 2;
    }
    FlashStep step;
    step.image.localPath = uf2;
    step.cpu             = cpu;
    step.action          = StepAction::Write;
    step.description      = std::string("flash ")
        + std::filesystem::path(uf2).filename().string() + " to " + cpuName(cpu);
    return runPlan({ step }, uf2 + " -> " + cpuName(cpu) + " CPU", opt, {});
}

const CatalogEntry* findEmbedded(const std::vector<CatalogEntry>& entries, std::string_view slug)
{
    for (const auto& e : entries) if (e.slug == slug) return &e;
    return nullptr;
}

int cmdEntries()
{
    const auto entries = embeddedEntries();
    if (entries.empty()) { std::printf("(no embedded entries in this build)\n"); return 0; }
    for (const auto& e : entries) {
        std::printf("%s\n    %s\n", e.slug.c_str(), e.name.c_str());
        const auto plan = buildFlashPlan(e);
        for (size_t i = 0; i < plan.size(); ++i)
            std::printf("    %zu. %s -> %s\n", i + 1, cpuName(plan[i].cpu), plan[i].description.c_str());
    }
    return 0;
}

int cmdInstall(std::string_view slug, const RunOptions& opt)
{
    const auto entries = embeddedEntries();
    const CatalogEntry* e = findEmbedded(entries, slug);
    if (!e) {
        std::fprintf(stderr, "no embedded entry named %.*s -- see `fwogcli entries`\n",
                     (int)slug.size(), slug.data());
        return 2;
    }
    // Warnings computed against the board's identity, exactly as the GUI's
    // preview does; the board is selected inside runPlan, so identify here too.
    const auto dev = selectDevice(opt.deviceSel);
    if (!dev) return 2;
    return runPlan(buildFlashPlan(*e), e->name, opt, planWarnings(*e, dev->id));
}

// Reboot one running CPU into BOOTSEL and wait for its drive, by hub position.
// Exactly the touch every flash step performs; exposed so a board can be put
// into a known state from a script (or so a user can reach RPI-RP2 on the
// DISPLAY CPU, which has no BOOTSEL button).
int cmdBootsel(TargetCpu cpu, const RunOptions& opt)
{
    const auto dev = selectDevice(opt.deviceSel);
    if (!dev) return 2;
    std::printf("board %s\n", describeFingerprint(dev->print).c_str());
    if (volumeForCpu(dev->id, cpu)) {
        std::printf("the %s CPU is already in BOOTSEL (%s)\n", cpuName(cpu), volumeForCpu(dev->id, cpu)->c_str());
        return 0;
    }
    const auto& port = portForCpu(dev->id, cpu);
    if (!port) {
        std::fprintf(stderr, "the %s CPU has no serial port to reboot -- it is not running, or not identified\n", cpuName(cpu));
        return 1;
    }
    std::printf("rebooting the %s CPU (%s) into BOOTSEL...\n", cpuName(cpu), port->c_str());
    std::fflush(stdout);
    touchPort1200(*port);
    for (int waited = 0; waited < kVolumeWaitMs; waited += kVolumePollMs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kVolumePollMs));
        if (auto id = identifyBoardNow(dev->uniqueID); id && volumeForCpu(*id, cpu)) {
            std::printf("OK: the %s CPU is in BOOTSEL, drive %s\n", cpuName(cpu), volumeForCpu(*id, cpu)->c_str());
            return 0;
        }
    }
    std::fprintf(stderr, "no RPI-RP2 drive appeared at the %s CPU's hub port within %d s\n",
                 cpuName(cpu), kVolumeWaitMs / 1000);
    return 1;
}

int cmdInfo(const std::string& uf2)
{
    std::ifstream in(uf2, std::ios::binary);
    if (!in) { std::fprintf(stderr, "could not open %s\n", uf2.c_str()); return 2; }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto info = parseUf2(bytes);
    if (!info) { std::fprintf(stderr, "%s: %s\n", uf2.c_str(), uf2ErrorMessage(info.error()).c_str()); return 1; }
    std::printf("%s\n  UF2: %u blocks, %llu payload bytes, first block at 0x%08X%s\n",
                uf2.c_str(), info->numBlocks, (unsigned long long)info->payloadBytes,
                info->targetAddr, info->familyPresent ? "" : " (no family id)");
    const auto records = findOgAppInfo(bytes);
    if (records.empty()) {
        std::printf("  no FwOGapp metadata record -- not an OG app image (or built without one)\n");
        return 0;
    }
    for (const auto& r : records) {
        std::printf("  %s: %s v%s\n", r.cpu == TargetCpu::Main ? "MAIN   " : "DISPLAY",
                    r.name.c_str(), formatOgAppVersion(r.version).c_str());
        if (!r.description.empty()) std::printf("           %s\n", r.description.c_str());
        if (!r.build.empty())       std::printf("           build %s\n", r.build.c_str());
    }
    return 0;
}

void usage()
{
    std::printf(
        "fwogcli -- FreeWili OG command-line flasher\n"
        "\n"
        "  fwogcli list\n"
        "  fwogcli flash <file.uf2> [--cpu main|display] [--device <which>] [--keep-display] [--yes]\n"
        "  fwogcli install <slug>   [--device <which>] [--keep-display] [--yes]\n"
        "  fwogcli entries\n"
        "  fwogcli info <file.uf2>\n"
        "  fwogcli bootsel main|display [--device <which>]\n"
        "\n"
        "Reuses the App Explorer flash engine, unchanged. Each connected board is\n"
        "identified separately and tracked by its position on the USB hub, so it\n"
        "stays selected while its CPUs drop into BOOTSEL. Before a MAIN install the\n"
        "DISPLAY is rebooted into BOOTSEL first (its running app would otherwise\n"
        "disturb the write); the new MAIN firmware brings it back. Every CPU is\n"
        "written by its hub port, whatever else is mounted.\n"
        "  --device <which>   which board: its number in `list`, or a substring of its\n"
        "                     serial or chip id (required if more than one is connected)\n"
        "  --keep-display     do not quiet the DISPLAY first (may make the write unreliable)\n"
        "  --yes              confirm a drive the engine cannot place by hub position\n"
        "  --cpu <cpu>        flash only: which CPU the file goes to (default main)\n"
        "\n"
        "Exit codes: 0 ok; 1 flash failed; 2 usage/selection; 3 needs --yes.\n");
}

int parseRunOptions(std::vector<std::string_view>& args, size_t from, RunOptions& opt,
                    std::optional<TargetCpu>* cpu, std::string* positional)
{
    for (size_t i = from; i < args.size(); ++i) {
        std::string_view a = args[i];
        if (a == "--yes" || a == "-y") { opt.assumeYes = true; }
        else if (a == "--keep-display") { opt.keepDisplay = true; }
        else if (a == "--cpu") {
            if (!cpu) { std::fprintf(stderr, "--cpu is only for `flash`\n"); return 2; }
            if (++i >= args.size()) { std::fprintf(stderr, "--cpu needs a value\n"); return 2; }
            std::string_view v = args[i];
            if (v == "main")         *cpu = TargetCpu::Main;
            else if (v == "display") *cpu = TargetCpu::Display;
            else { std::fprintf(stderr, "--cpu must be main or display\n"); return 2; }
        }
        else if (a == "--device") {
            if (++i >= args.size()) { std::fprintf(stderr, "--device needs a value\n"); return 2; }
            opt.deviceSel = args[i];
        }
        else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "unknown option: %.*s\n", (int)a.size(), a.data());
            return 2;
        }
        else if (positional && positional->empty()) { positional->assign(a); }
        else { std::fprintf(stderr, "unexpected argument: %.*s\n", (int)a.size(), a.data()); return 2; }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string_view> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "-h" || args[0] == "--help") {
        usage();
        return args.empty() ? 2 : 0;
    }

    if (args[0] == "list" || args[0] == "devices") return cmdList();
    if (args[0] == "entries") return cmdEntries();

    if (args[0] == "info") {
        if (args.size() != 2) { std::fprintf(stderr, "info needs a UF2 path\n"); return 2; }
        return cmdInfo(std::string(args[1]));
    }

    if (args[0] == "flash") {
        RunOptions opt;
        std::optional<TargetCpu> cpu;
        std::string uf2;
        if (int rc = parseRunOptions(args, 1, opt, &cpu, &uf2)) return rc;
        if (uf2.empty()) { std::fprintf(stderr, "flash needs a UF2 path\n"); return 2; }
        return cmdFlash(uf2, cpu.value_or(TargetCpu::Main), opt);
    }

    if (args[0] == "bootsel") {
        RunOptions opt;
        std::string which;
        if (int rc = parseRunOptions(args, 1, opt, nullptr, &which)) return rc;
        if (which != "main" && which != "display") {
            std::fprintf(stderr, "bootsel needs main or display\n"); return 2;
        }
        return cmdBootsel(which == "main" ? TargetCpu::Main : TargetCpu::Display, opt);
    }

    if (args[0] == "install") {
        RunOptions opt;
        std::string slug;
        if (int rc = parseRunOptions(args, 1, opt, nullptr, &slug)) return rc;
        if (slug.empty()) { std::fprintf(stderr, "install needs an entry slug -- see `fwogcli entries`\n"); return 2; }
        return cmdInstall(slug, opt);
    }

    std::fprintf(stderr, "unknown command: %.*s\n\n", (int)args[0].size(), args[0].data());
    usage();
    return 2;
}
