#pragma once

#ifdef __cplusplus

#include <string>

// Release updater for the HiveShock fork. It asks the GitHub releases API for the latest release (through the
// system `curl`, so the game does not need its own TLS stack), downloads the asset for this platform and, on
// Windows, swaps the game files once the game has closed. Everything runs on a worker thread.
namespace HiveShockUpdater {

enum class State {
    Idle,        // nothing checked yet
    Checking,    // asking GitHub
    UpToDate,    // this build is the latest (or is a development build)
    Available,   // a newer release exists
    Downloading, // downloading / unpacking it
    Ready,       // downloaded; Windows can install it now
    Error,
};

struct Snapshot {
    State state = State::Idle;
    std::string current;  // tag of this build, empty on development builds
    std::string latest;   // tag of the newest release, empty until checked
    std::string message;  // short status line for the menu
    std::string location; // folder where the download was saved (Ready)
    std::string releaseUrl; // page of the newest release on GitHub
    int percent = 0;      // 0-100 while Downloading
};

Snapshot GetSnapshot();

// Whether this platform can replace the game by itself (Windows). Elsewhere the file is only downloaded.
bool CanInstall();

// Checks the first time it is called in a session; later calls do nothing. Use CheckNow for the manual button.
void CheckOnce();
void CheckNow();
void Download();

// Windows only, state Ready: starts the helper that copies the new files once the game has exited and closes the
// game. The helper reopens it afterwards.
void InstallAndRestart();

} // namespace HiveShockUpdater

#endif // __cplusplus
