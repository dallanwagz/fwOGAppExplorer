#pragma once

namespace fwog {

/// SDL3 + Dear ImGui application shell: window, main loop, menu bar with a
/// Theme submenu, and the three-tab body ("App Explorer", "Default Firmware",
/// "Recovery"). Persists theme, last-selected tab,
/// window geometry and the remote catalog URL to
/// userDataDir()/"settings.ini" across restarts.
class App {
public:
    /// Runs the app to completion (window opened, event loop, clean
    /// shutdown). Returns a process exit code: 0 on a normal close, non-zero
    /// if SDL/ImGui failed to initialize.
    int run();
};

} // namespace fwog
