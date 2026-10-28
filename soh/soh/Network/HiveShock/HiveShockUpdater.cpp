#include "HiveShockUpdater.h"

#if defined(__SWITCH__) || defined(__WIIU__)

namespace HiveShockUpdater {
Snapshot GetSnapshot() {
    Snapshot snapshot;
    snapshot.state = State::Error;
    snapshot.message = "Updates are not supported on this platform";
    return snapshot;
}
bool CanInstall() {
    return false;
}
void CheckOnce() {
}
void CheckNow() {
}
void Download() {
}
void InstallAndRestart() {
}
} // namespace HiveShockUpdater

#else

#ifdef _WIN32
#include <windows.h>
#endif

#include <ship/Context.h>
#include <ship/window/Window.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" unsigned char gGitCommitTag[]; // tag of the release this build was made from, empty otherwise

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace HiveShockUpdater {
namespace {

constexpr const char* kApiUrl = "https://api.github.com/repos/Lexharden/Shipwright-HiveShock/releases/latest";
// Only files served from this fork's releases are ever downloaded, whatever the API answers.
constexpr const char* kDownloadPrefix = "https://github.com/Lexharden/Shipwright-HiveShock/releases/download/";
constexpr const char* kReleasePagePrefix = "https://github.com/Lexharden/Shipwright-HiveShock/releases/";

#if defined(_WIN32)
constexpr const char* kCurl = "curl.exe";
constexpr const char* kAssetSuffix = "-win64.zip";
#elif defined(__APPLE__)
constexpr const char* kCurl = "curl";
constexpr const char* kAssetSuffix = ".dmg";
#else
constexpr const char* kCurl = "curl";
constexpr const char* kAssetSuffix = ".appimage";
#endif

struct Shared {
    std::mutex mutex;
    Snapshot snapshot;
    std::string assetName;
    std::string assetUrl;
    long long assetSize = 0;
    fs::path partFile;
    fs::path stagingDir;
    bool checkedOnce = false;
};

// Never destroyed: a worker still running while the game exits must not touch a destructed object.
Shared& S() {
    static Shared* shared = new Shared();
    return *shared;
}

void SetState(State state, const std::string& message) {
    Shared& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.snapshot.state = state;
    s.snapshot.message = message;
}

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return text;
}

bool EndsWith(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string CurrentTag() {
    return std::string(reinterpret_cast<const char*>(gGitCommitTag));
}

// "v1.0.12" -> {1, 0, 12}
std::vector<int> ParseVersion(const std::string& text) {
    std::vector<int> parts;
    int value = 0;
    bool inNumber = false;
    for (char c : text) {
        if (c >= '0' && c <= '9') {
            value = std::min(value * 10 + (c - '0'), 1000000);
            inNumber = true;
        } else if (inNumber) {
            parts.push_back(value);
            value = 0;
            inNumber = false;
        }
    }
    if (inNumber) {
        parts.push_back(value);
    }
    return parts;
}

bool IsNewer(const std::string& latest, const std::string& current) {
    std::vector<int> a = ParseVersion(latest);
    std::vector<int> b = ParseVersion(current);
    if (a.empty()) {
        return false;
    }
    const size_t count = std::max(a.size(), b.size());
    a.resize(count, 0);
    b.resize(count, 0);
    return a > b;
}

fs::path WorkDir() {
    std::error_code ec;
#ifdef _WIN32
    fs::path dir = fs::temp_directory_path(ec) / "hiveshock_update";
#else
    // Downloaded files are for the user to open, so prefer a folder they know.
    fs::path dir;
    if (const char* home = std::getenv("HOME")) {
        dir = fs::path(home) / "Downloads";
        if (!fs::is_directory(dir, ec)) {
            dir.clear();
        }
    }
    if (dir.empty()) {
        dir = fs::temp_directory_path(ec);
    }
    dir /= "hiveshock_update";
#endif
    fs::create_directories(dir, ec);
    return dir;
}

#ifdef _WIN32
std::wstring ToWide(const std::string& text) {
    if (text.empty()) {
        return std::wstring();
    }
    int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
    std::wstring wide(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), wide.data(), size);
    return wide;
}

std::wstring QuoteArg(const std::wstring& arg) {
    std::wstring quoted = L"\"";
    for (wchar_t c : arg) {
        if (c == L'"') {
            quoted += L'\\';
        }
        quoted += c;
    }
    return quoted + L"\"";
}

// Runs a program with no console window. Returns its exit code, or -1 if it could not be started.
int RunProgram(const std::vector<std::string>& argv, bool wait, const fs::path& cwd = fs::path()) {
    std::wstring commandLine;
    for (const std::string& arg : argv) {
        if (!commandLine.empty()) {
            commandLine += L' ';
        }
        commandLine += QuoteArg(ToWide(arg));
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    DWORD flags = CREATE_NO_WINDOW | (wait ? 0 : (DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP));
    if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, flags, nullptr,
                        cwd.empty() ? nullptr : cwd.c_str(), &startup, &process)) {
        return -1;
    }
    int exitCode = 0;
    if (wait) {
        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(process.hProcess, &code);
        exitCode = (int)code;
    }
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    return exitCode;
}

fs::path ExecutablePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        DWORD length = GetModuleFileNameW(nullptr, buffer.data(), (DWORD)buffer.size());
        if (length == 0) {
            return fs::path();
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return fs::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
}
#else
int RunProgram(const std::vector<std::string>& argv, bool wait, const fs::path& = fs::path()) {
    std::string command;
    for (const std::string& arg : argv) {
        std::string quoted = "'";
        for (char c : arg) {
            if (c == '\'') {
                quoted += "'\\''";
            } else {
                quoted += c;
            }
        }
        command += quoted + "' ";
    }
    command += "> /dev/null 2>&1";
    if (!wait) {
        command += " &";
    }
    return std::system(command.c_str());
}
#endif

void CheckWorker() {
    Shared& s = S();
    SetState(State::Checking, "Checking for updates...");

    const fs::path jsonFile = WorkDir() / "latest.json";
    std::error_code ec;
    fs::remove(jsonFile, ec);

    const int code =
        RunProgram({ kCurl, "-sSfL", "--max-time", "20", "-H", "Accept: application/vnd.github+json", "-H",
                     "User-Agent: Shipwright-HiveShock-Updater", kApiUrl, "-o", jsonFile.string() },
                   true);
    if (code != 0) {
        SetState(State::Error, "Could not reach GitHub (curl exit " + std::to_string(code) + ")");
        return;
    }

    json release;
    try {
        std::ifstream in(jsonFile);
        release = json::parse(in);
    } catch (const std::exception& e) {
        SPDLOG_WARN("[HiveShock] updater: bad release JSON: {}", e.what());
        SetState(State::Error, "GitHub sent an unreadable answer");
        return;
    }
    fs::remove(jsonFile, ec);

    const std::string tag = release.value("tag_name", "");
    std::string pageUrl = release.value("html_url", "");
    if (tag.empty()) {
        SetState(State::Error, "No release found");
        return;
    }
    if (pageUrl.rfind(kReleasePagePrefix, 0) != 0) {
        pageUrl = std::string(kReleasePagePrefix) + "latest";
    }

    std::string assetName;
    std::string assetUrl;
    long long assetSize = 0;
    if (release.contains("assets") && release["assets"].is_array()) {
        for (const json& asset : release["assets"]) {
            const std::string name = asset.value("name", "");
            const std::string url = asset.value("browser_download_url", "");
            if (EndsWith(Lower(name), kAssetSuffix) && url.rfind(kDownloadPrefix, 0) == 0 &&
                name.find_first_of("/\\") == std::string::npos) {
                assetName = name;
                assetUrl = url;
                assetSize = asset.value("size", 0LL);
                break;
            }
        }
    }

    const std::string current = CurrentTag();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.snapshot.latest = tag;
    s.snapshot.releaseUrl = pageUrl;
    s.assetName = assetName;
    s.assetUrl = assetUrl;
    s.assetSize = assetSize;
    if (current.empty()) {
        s.snapshot.state = State::UpToDate;
        s.snapshot.message = "Development build (no release tag). Latest release: " + tag;
    } else if (!IsNewer(tag, current)) {
        s.snapshot.state = State::UpToDate;
        s.snapshot.message = "You have the latest version";
    } else if (assetUrl.empty()) {
        s.snapshot.state = State::Error;
        s.snapshot.message = tag + " is out, but it has no file for this platform";
    } else {
        s.snapshot.state = State::Available;
        s.snapshot.message = "New version available: " + tag;
    }
}

void DownloadWorker() {
    Shared& s = S();
    std::string url;
    std::string name;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        url = s.assetUrl;
        name = s.assetName;
    }
    if (url.empty() || name.empty()) {
        SetState(State::Error, "Nothing to download");
        return;
    }

    const fs::path dir = WorkDir();
    const fs::path finalFile = dir / name;
    const fs::path partFile = dir / (name + ".part");
    std::error_code ec;
    fs::remove(finalFile, ec);
    fs::remove(partFile, ec);
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.partFile = partFile;
        s.snapshot.percent = 0;
    }
    SetState(State::Downloading, "Downloading " + name + "...");

    const int code = RunProgram({ kCurl, "-sSfL", "--retry", "2", "--max-time", "1800", url, "-o", partFile.string() },
                                true);
    if (code != 0) {
        fs::remove(partFile, ec);
        SetState(State::Error, "Download failed (curl exit " + std::to_string(code) + ")");
        return;
    }
    fs::rename(partFile, finalFile, ec);
    if (ec) {
        SetState(State::Error, "Could not save the download: " + ec.message());
        return;
    }

#ifdef _WIN32
    const fs::path staging = dir / "staging";
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    SetState(State::Downloading, "Unpacking...");
    // tar.exe ships with Windows 10 and later and reads zip files.
    const int tarCode = RunProgram({ "tar.exe", "-xf", finalFile.string(), "-C", staging.string() }, true);
    if (tarCode != 0 || !fs::exists(staging / "soh.exe", ec)) {
        fs::remove_all(staging, ec);
        SetState(State::Error, "Could not unpack the download");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.stagingDir = staging;
    }
#endif

    std::lock_guard<std::mutex> lock(s.mutex);
    s.snapshot.location = dir.string();
    s.snapshot.state = State::Ready;
#ifdef _WIN32
    s.snapshot.message = "Ready to install " + s.snapshot.latest;
#else
    s.snapshot.message = "Downloaded " + name + " to " + dir.string();
#endif
}

void StartWorker(void (*worker)()) {
    std::thread(worker).detach();
}

} // namespace

Snapshot GetSnapshot() {
    Shared& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    Snapshot snapshot = s.snapshot;
    snapshot.current = CurrentTag();
    if (snapshot.state == State::Downloading && s.assetSize > 0 && !s.partFile.empty()) {
        std::error_code ec;
        const auto written = fs::file_size(s.partFile, ec);
        if (!ec) {
            snapshot.percent = (int)std::min<long long>(100, (long long)written * 100 / s.assetSize);
        }
    }
    return snapshot;
}

bool CanInstall() {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

void CheckNow() {
    Shared& s = S();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.checkedOnce = true;
        if (s.snapshot.state == State::Checking || s.snapshot.state == State::Downloading) {
            return;
        }
        s.snapshot.state = State::Checking;
        s.snapshot.message = "Checking for updates...";
    }
    StartWorker(CheckWorker);
}

void CheckOnce() {
    {
        std::lock_guard<std::mutex> lock(S().mutex);
        if (S().checkedOnce) {
            return;
        }
    }
    CheckNow();
}

void Download() {
    Shared& s = S();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.snapshot.state != State::Available && s.snapshot.state != State::Ready) {
            return;
        }
        s.snapshot.state = State::Downloading;
        s.snapshot.message = "Starting download...";
    }
    StartWorker(DownloadWorker);
}

void InstallAndRestart() {
#ifdef _WIN32
    Shared& s = S();
    fs::path staging;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.snapshot.state != State::Ready) {
            return;
        }
        staging = s.stagingDir;
    }
    std::error_code ec;
    const fs::path exe = ExecutablePath();
    if (exe.empty() || staging.empty() || !fs::exists(staging / "soh.exe", ec)) {
        SetState(State::Error, "The downloaded files are missing, download again");
        return;
    }
    const fs::path installDir = exe.parent_path();
    const fs::path work = staging.parent_path();
    const fs::path script = work / "update.bat";

    // The paths go inside batch quotes; these characters would still be interpreted there.
    for (const fs::path& p : { installDir, work, exe.filename() }) {
        if (p.string().find_first_of("%^!&") != std::string::npos) {
            SetState(State::Error, "The game folder name has characters the updater cannot handle");
            return;
        }
    }

    {
        std::ofstream out(script, std::ios::trunc);
        // robocopy retries a locked file (the running game) once per second for two minutes.
        out << "@echo off\r\n"
            << "robocopy \"" << staging.string() << "\" \"" << installDir.string()
            << "\" /E /IS /IT /R:120 /W:1 /NFL /NDL /NJH /NJS /NP > nul\r\n"
            << "cd /d \"" << installDir.string() << "\"\r\n"
            << "start \"\" \"" << exe.filename().string() << "\"\r\n"
            << "(goto) 2>nul & rd /s /q \"" << work.string() << "\"\r\n";
        if (!out) {
            SetState(State::Error, "Could not write the update script");
            return;
        }
    }

    // Started from the temp folder so the script's own folder is not in use when it deletes itself.
    if (RunProgram({ "cmd.exe", "/c", script.string() }, false, fs::temp_directory_path(ec)) < 0) {
        SetState(State::Error, "Could not start the update script");
        return;
    }
    SetState(State::Ready, "Closing the game to finish the update...");
    Ship::Context::GetRawInstance()->GetWindow()->Close();
#endif
}

} // namespace HiveShockUpdater

#endif // __SWITCH__ || __WIIU__
