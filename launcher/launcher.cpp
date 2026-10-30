// Canopus Protocol launcher: everything a player needs, from their game
// files to the running game, with no tools to install.
//
//   1. extracts the player's dump from games/ into data/ (tools/setup)
//   2. recompiles its code to C (tools/recomp) and compiles that with the
//      bundled Zig (tools/zig), linking the prebuilt runtime
//      (tools/runtime): only the first time, or when the game changes
//      (for example the update was added)
//   3. starts the game
//
// The game is built on the player's machine from their own copy: nothing
// made from it is part of the download.
//
// Linux: "canopus-protocol --install-desktop" adds a menu entry with the icon.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
constexpr const char* kZigTarget = "x86_64-windows-gnu";
constexpr const char* kExe = ".exe";
#else
constexpr const char* kZigTarget = "x86_64-linux-gnu.2.28";
constexpr const char* kExe = "";
#endif
constexpr const char* kGameName = "Canopus Protocol";

fs::path g_root;  // the folder the launcher is in
std::ofstream g_log;

// ---- output: a window on Windows, the terminal (or zenity) on Linux ----

std::mutex g_outLock;
std::function<void(const std::string&, bool)> g_sink;  // line, replaces the last line

void Say(const std::string& line, bool replace = false)
{
    std::lock_guard<std::mutex> g(g_outLock);
    if (g_log) g_log << line << "\n" << std::flush;
    if (g_sink) g_sink(line, replace);
}

std::string ReadText(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(in)), {});
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

void WriteText(const fs::path& p, const std::string& s)
{
    std::ofstream out(p, std::ios::binary);
    out << s;
}

uint32_t Crc32File(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) return 0;
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
            table[i] = c;
        }
        ready = true;
    }
    uint32_t crc = 0xFFFFFFFF;
    std::vector<char> buf(1 << 16);
    while (in) {
        in.read(buf.data(), buf.size());
        for (std::streamsize i = 0; i < in.gcount(); ++i) crc = table[(crc ^ (uint8_t)buf[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

std::string Hex(uint32_t v)
{
    char b[16];
    std::snprintf(b, sizeof b, "%08X", v);
    return b;
}

// ---- running a tool, its output line by line ----

using Args = std::vector<fs::path>;  // paths and words alike (native strings)

#ifdef _WIN32
std::wstring Quote(const std::wstring& a)
{
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++slashes;
        } else {
            if (c == L'"') q.append(slashes + 1, L'\\');
            slashes = 0;
        }
        q += c;
    }
    q.append(slashes, L'\\');
    return q + L"\"";
}
#endif

// Runs a program; each line it prints goes to onLine (a line ended by \r is
// a progress update). Returns its exit code, or -1 if it could not start.
int Run(const Args& args, const std::function<void(const std::string&, bool)>& onLine)
{
    std::string pending;
    auto feed = [&](const char* data, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            const char c = data[i];
            if (c == '\n' || c == '\r') {
                if (!pending.empty() && onLine) onLine(pending, c == '\r');
                pending.clear();
            } else {
                pending += c;
            }
        }
    };
#ifdef _WIN32
    std::wstring cmd;
    for (const auto& a : args) cmd += (cmd.empty() ? L"" : L" ") + Quote(a.native());
    SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(0);
    const BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        return -1;
    }
    char buf[4096];
    DWORD n;
    while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) feed(buf, n);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
#else
    int fds[2];
    if (pipe(fds) != 0) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    std::vector<std::string> strs;
    for (const auto& a : args) strs.push_back(a.native());
    std::vector<char*> argv;
    for (auto& s : strs) argv.push_back(s.data());
    argv.push_back(nullptr);
    pid_t pid;
    const int err = posix_spawn(&pid, argv[0], &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (err != 0) {
        close(fds[0]);
        return -1;
    }
    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof buf)) > 0) feed(buf, (size_t)n);
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    const int code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
    if (!pending.empty() && onLine) onLine(pending, false);
    return (int)code;
}

void SetEnv(const char* name, const fs::path& value)
{
#ifdef _WIN32
    SetEnvironmentVariableW(fs::path(name).wstring().c_str(), value.wstring().c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

// ---- the steps ----

struct Paths {
    fs::path games, data, cache, gen, obj, tools, zig, recomp, setup, tables, runtimeLib, res, game, logFile;
};

Paths MakePaths()
{
    Paths p;
    p.games = g_root / "games";
    p.data = g_root / "data";
    p.cache = g_root / "cache";
    p.gen = p.cache / "gen";
    p.obj = p.cache / "obj";
    p.tools = g_root / "tools";
    p.zig = p.tools / "zig" / (std::string("zig") + kExe);
    p.recomp = p.tools / (std::string("recomp") + kExe);
    p.setup = p.tools / (std::string("setup") + kExe);
    p.tables = p.tools / "tables";
    p.runtimeLib = p.tools / "runtime" / "libcanopus_rt.a";
    p.res = p.tools / "runtime" / "canopus.res";
    p.game = p.cache / (std::string("canopus-protocol-game") + kExe);
    p.logFile = g_root / "canopus-setup.log";
    return p;
}

bool HasGameFiles(const fs::path& dir)
{
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::string ext = e.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".3ds" || ext == ".cci" || ext == ".cia" || ext == ".cxi") return true;
    }
    return false;
}

// The game files in games/ (names, sizes, dates): a change means setup has work.
std::string GamesFingerprint(const fs::path& dir)
{
    std::vector<std::string> items;
    std::error_code ec;
    if (fs::is_directory(dir, ec))
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (!e.is_regular_file(ec)) continue;
            const auto t = fs::last_write_time(e.path(), ec).time_since_epoch().count();
            items.push_back(e.path().filename().u8string() + "|" + std::to_string(e.file_size(ec)) + "|" + std::to_string((long long)t));
        }
    std::sort(items.begin(), items.end());
    std::string all;
    for (const auto& i : items) all += i + "\n";
    return all;
}

// Up to date: nothing to build, the game can start at once.
struct Keys {
    std::string gen, link;
};

Keys CurrentKeys(const Paths& p)
{
    const std::string version = ReadText(p.data / "version.txt");
    Keys k;
    k.gen = "version " + (version.empty() ? std::string("1.0") : version) + ", code " + Hex(Crc32File(p.data / "code.bin")) +
            ", recompiler " + Hex(Crc32File(p.recomp)) + ", tables " + Hex(Crc32File(p.tables / "functions_v11.csv") ^ Crc32File(p.tables / "functions.csv"));
    k.link = k.gen + ", runtime " + Hex(Crc32File(p.runtimeLib));
    return k;
}

// Nothing changed since the last start: no setup, no build.
bool UpToDate(bool force)
{
    if (force) return false;
    const Paths p = MakePaths();
    std::error_code ec;
    if (!fs::exists(p.game, ec) || ReadText(p.cache / "games.key") != GamesFingerprint(p.games)) return false;
    const Keys k = CurrentKeys(p);
    return ReadText(p.cache / "gen.key") == k.gen && ReadText(p.cache / "link.key") == k.link;
}

bool Setup(const Paths& p)
{
    Say("Reading your game files from the games folder...");
    const int code = Run({p.setup, p.games, p.data}, [](const std::string& l, bool progress) { Say("  " + l, progress); });
    if (code != 0) {
        Say("");
        Say(code < 0 ? "Could not start tools/setup." : "Setting up the game data did not work (see above).");
        return false;
    }
    return true;
}

bool Recompile(const Paths& p)
{
    std::error_code ec;
    fs::remove_all(p.gen, ec);
    fs::remove_all(p.obj, ec);
    fs::create_directories(p.gen);
    fs::create_directories(p.obj);
    const bool v11 = ReadText(p.data / "version.txt") == "1.1";
    Say(std::string("Recompiling the game code (version ") + (v11 ? "1.1" : "1.0") + ")...");
    std::vector<std::string> errors;
    const int code = Run({p.recomp, "-code", p.data / "code.bin", "-exheader", p.data / "exheader.bin", "-funcs",
                          p.tables / (v11 ? "functions_v11.csv" : "functions.csv"), "-hints", p.tables / (v11 ? "hints_v11.txt" : "hints.txt"),
                          "-out", p.gen},
                         [&](const std::string& l, bool) { errors.push_back(l); });
    if (code != 0) {
        for (const auto& e : errors) Say("  " + e);
        Say("Recompiling did not work.");
        return false;
    }
    return true;
}

bool Compile(const Paths& p)
{
    std::vector<fs::path> sources;
    for (const auto& e : fs::directory_iterator(p.gen))
        if (e.path().extension() == ".c") sources.push_back(e.path());
    std::sort(sources.begin(), sources.end(), [](const fs::path& a, const fs::path& b) {
        // Biggest first: the long ones start early and finish together.
        std::error_code ec;
        return fs::file_size(a, ec) > fs::file_size(b, ec);
    });
    const unsigned jobs = std::max(1u, std::thread::hardware_concurrency());
    Say("Compiling the game (" + std::to_string(sources.size()) + " parts, " + std::to_string(jobs) +
        " at a time). This takes a few minutes, only this once...");
    std::atomic<size_t> next{0}, done{0};
    std::atomic<bool> failed{false};
    std::mutex errLock;
    std::vector<std::string> errors;
    auto worker = [&] {
        for (;;) {
            const size_t i = next++;
            if (i >= sources.size() || failed) return;
            const fs::path out = p.obj / (sources[i].stem().string() + ".o");
            std::vector<std::string> lines;
            const int code = Run({p.zig, "cc", "-target", kZigTarget, "-std=c11", "-O1", "-g0", "-ffp-contract=off", "-fno-strict-aliasing", "-w",
                                  "-I", p.gen, "-c", sources[i], "-o", out},
                                 [&](const std::string& l, bool) { lines.push_back(l); });
            if (code != 0) {
                failed = true;
                std::lock_guard<std::mutex> g(errLock);
                errors.push_back(sources[i].filename().string() + ":");
                for (auto& l : lines) errors.push_back("  " + l);
                return;
            }
            const size_t n = ++done;
            Say("  " + std::to_string(n) + " of " + std::to_string(sources.size()) + " done", true);
        }
    };
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < jobs; ++t) threads.emplace_back(worker);
    for (auto& t : threads) t.join();
    if (failed) {
        for (const auto& e : errors) Say(e);
        Say("Compiling did not work.");
        return false;
    }
    return true;
}

bool Link(const Paths& p)
{
    Say("Linking...");
    Args args = {p.zig, "c++", "-target", kZigTarget, "-s", "-o", p.game};
    std::vector<fs::path> objs;
    for (const auto& e : fs::directory_iterator(p.obj))
        if (e.path().extension() == ".o") objs.push_back(e.path());
    std::sort(objs.begin(), objs.end());
    args.insert(args.end(), objs.begin(), objs.end());
    args.push_back(p.runtimeLib);
#ifdef _WIN32
    for (const char* a : {"-lopengl32", "-lgdi32", "-luser32", "-lwinmm", "-Wl,--stack,67108864", "-Wl,--subsystem,windows"}) args.push_back(a);
    args.push_back(p.res);
#else
    for (const char* a : {"-lm", "-lpthread", "-ldl"}) args.push_back(a);
#endif
    std::vector<std::string> lines;
    const int code = Run(args, [&](const std::string& l, bool) { lines.push_back(l); });
    if (code != 0) {
        for (auto& l : lines) Say("  " + l);
        Say("Linking did not work.");
        return false;
    }
    return true;
}

// Builds what is out of date. True when the game is ready to start.
bool Prepare(bool force)
{
    const Paths p = MakePaths();
    std::error_code ec;
    if (!fs::exists(p.games, ec)) fs::create_directories(p.games, ec);
    if (!HasGameFiles(p.games) && !fs::exists(p.data / "setup_done.txt", ec)) {
        Say(std::string("Put your decrypted game dump (.3ds, .cci or .cia) in the games folder:"));
        Say("  " + p.games.u8string());
        Say("Add the update and DLC .cia files there too if you have them, then start " + std::string(kGameName) + " again.");
        return false;
    }
    if (HasGameFiles(p.games) && !Setup(p)) return false;
    if (!fs::exists(p.data / "code.bin", ec)) {
        Say("No game data yet: the games folder needs your game dump (not only the update or DLC).");
        return false;
    }
    fs::create_directories(p.cache, ec);
    // Zig keeps its caches with the game, not in the player's home folder.
    SetEnv("ZIG_GLOBAL_CACHE_DIR", p.cache / "zig-global");
    SetEnv("ZIG_LOCAL_CACHE_DIR", p.cache / "zig-local");

    const Keys k = CurrentKeys(p);
    const bool genOk = !force && ReadText(p.cache / "gen.key") == k.gen && fs::exists(p.obj, ec) && !fs::is_empty(p.obj, ec);
    const bool linkOk = genOk && ReadText(p.cache / "link.key") == k.link && fs::exists(p.game, ec);
    WriteText(p.cache / "games.key", GamesFingerprint(p.games));
    if (linkOk) return true;
    if (!genOk) {
        fs::remove(p.cache / "gen.key", ec);
        if (!Recompile(p) || !Compile(p)) return false;
        WriteText(p.cache / "gen.key", k.gen);
    }
    if (!Link(p)) return false;
    WriteText(p.cache / "link.key", k.link);
    Say("Ready.");
    return true;
}

// Starts the game. Windows: as its own process (the launcher then exits);
// Linux: in place of the launcher.
bool StartGame(const std::vector<std::string>& extra)
{
    const Paths p = MakePaths();
#ifdef _WIN32
    std::wstring cmd = Quote(p.game.wstring()) + L" --data " + Quote(p.data.wstring());
    for (const auto& a : extra) cmd += L" " + Quote(fs::u8path(a).wstring());
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(0);
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, g_root.wstring().c_str(), &si, &pi)) {
        Say("Could not start the game (error " + std::to_string(GetLastError()) + ").");
        return false;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
#else
    if (chdir(g_root.c_str()) != 0) {}
    std::vector<std::string> strs = {p.game.native(), "--data", p.data.native()};
    strs.insert(strs.end(), extra.begin(), extra.end());
    std::vector<char*> argv;
    for (auto& s : strs) argv.push_back(s.data());
    argv.push_back(nullptr);
    execv(argv[0], argv.data());
    Say(std::string("Could not start the game: ") + std::strerror(errno));
    return false;
#endif
}

#ifndef _WIN32
// A menu entry with the icon (~/.local/share/applications).
int InstallDesktop()
{
    const char* home = std::getenv("HOME");
    if (!home) return 1;
    const fs::path dir = fs::path(home) / ".local/share/applications";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path file = dir / "canopus-protocol.desktop";
    std::ofstream out(file);
    out << "[Desktop Entry]\nType=Application\nName=" << kGameName
        << "\nComment=Unofficial PC port of Devil Survivor 2 Record Breaker\n"
        << "Exec=\"" << (g_root / "canopus-protocol").native() << "\"\n"
        << "Path=" << g_root.native() << "\n"
        << "Icon=" << (g_root / "canopus-protocol.png").native() << "\n"
        << "Terminal=false\nCategories=Game;\n";
    out.close();
    std::cout << "Added " << file.native() << "\n" << kGameName << " is now in your applications menu.\n";
    return 0;
}
#endif

}  // namespace

// ---- Windows: a small window that shows the progress ----
#ifdef _WIN32

namespace {

HWND g_win = nullptr, g_edit = nullptr;
std::vector<std::string> g_lines;
constexpr UINT WM_TEXT = WM_APP + 1, WM_DONE = WM_APP + 2;

std::wstring Widen(const std::string& s)
{
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_SIZE:
        if (g_edit) MoveWindow(g_edit, 8, 8, LOWORD(lp) - 16, HIWORD(lp) - 16, TRUE);
        return 0;
    case WM_TEXT: {
        std::string all;
        {
            std::lock_guard<std::mutex> g(g_outLock);
            for (const auto& l : g_lines) all += l + "\r\n";
        }
        SetWindowTextW(g_edit, Widen(all).c_str());
        SendMessageW(g_edit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
        SendMessageW(g_edit, EM_SCROLLCARET, 0, 0);
        return 0;
    }
    case WM_DONE:
        if (wp) DestroyWindow(h);  // ready: the game starts
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

}  // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int)
{
    wchar_t self[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, self, MAX_PATH * 4);
    g_root = fs::path(self).parent_path();
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool force = false;
    std::vector<std::string> extra;
    for (int i = 1; i < argc; ++i) {
        const std::string a = fs::path(wargv[i]).u8string();
        if (a == "--rebuild") force = true;
        else extra.push_back(a);
    }
    g_log.open(MakePaths().logFile, std::ios::app);

    // Up to date: start at once, no window.
    if (UpToDate(force)) return StartGame(extra) ? 0 : 1;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = L"CanopusLauncher";
    RegisterClassExW(&wc);
    g_win = CreateWindowExW(0, wc.lpszClassName, L"Canopus Protocol - getting ready", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                            CW_USEDEFAULT, 760, 460, nullptr, nullptr, inst, nullptr);
    g_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 8, 8,
                             720, 400, g_win, nullptr, inst, nullptr);
    HFONT font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    SendMessageW(g_edit, WM_SETFONT, (WPARAM)font, TRUE);
    RECT rc;
    GetClientRect(g_win, &rc);
    MoveWindow(g_edit, 8, 8, rc.right - 16, rc.bottom - 16, TRUE);

    g_sink = [](const std::string& line, bool replace) {
        if (replace && !g_lines.empty() && g_lines.back().rfind("  ", 0) == 0) g_lines.back() = line;
        else g_lines.push_back(line);
        PostMessageW(g_win, WM_TEXT, 0, 0);
    };
    std::atomic<bool> ready{false};
    std::thread([&] {
        Say(std::string(kGameName) + ": unofficial PC port of Devil Survivor 2 Record Breaker.");
        Say("The game is built from your own copy, on this computer, the first time it starts.");
        Say("");
        const bool ok = Prepare(force);
        if (ok) {
            ready = true;
        } else {
            Say("");
            Say("The details are in canopus-setup.log. Close this window when you are done.");
        }
        PostMessageW(g_win, WM_DONE, ok, 0);
    }).detach();
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (ready) return StartGame(extra) ? 0 : 1;
    return 1;
}

#else  // Linux

int main(int argc, char** argv)
{
    g_root = fs::canonical("/proc/self/exe").parent_path();
    bool force = false;
    std::vector<std::string> extra;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--install-desktop") return InstallDesktop();
        if (a == "--rebuild") force = true;
        else extra.push_back(a);
    }
    g_log.open(MakePaths().logFile, std::ios::app);

    // Progress: the terminal if there is one; otherwise zenity's progress
    // window if it is installed (a launch from the applications menu).
    FILE* zenity = nullptr;
    const bool tty = isatty(1);
    if (tty) {
        g_sink = [](const std::string& line, bool replace) {
            if (replace) std::cout << "\r" << line << "\033[K" << std::flush;
            else std::cout << line << "\n";
        };
    }
    auto openZenity = [&] {
        if (tty || zenity || std::system("command -v zenity >/dev/null 2>&1") != 0) return;
        zenity = popen("zenity --progress --pulsate --no-cancel --auto-close --title='Canopus Protocol' --width=520 "
                       "--text='Getting ready...' 2>/dev/null",
                       "w");
        if (zenity)
            g_sink = [&zenity](const std::string& line, bool) {
                std::string l = line;
                std::replace(l.begin(), l.end(), '\'', ' ');
                std::fprintf(zenity, "# %s\n", l.c_str());
                std::fflush(zenity);
            };
    };
    if (UpToDate(force)) return StartGame(extra) ? 0 : 1;
    openZenity();
    if (tty) {
        Say(std::string(kGameName) + ": unofficial PC port of Devil Survivor 2 Record Breaker.");
        Say("The game is built from your own copy, on this computer, the first time it starts.");
        Say("");
    }
    const bool ok = Prepare(force);
    if (zenity) {
        std::fprintf(zenity, "100\n");
        pclose(zenity);
    }
    if (!ok) {
        if (!tty && std::system("command -v zenity >/dev/null 2>&1") == 0)
            std::system("zenity --error --title='Canopus Protocol' --text='Canopus Protocol could not get ready. "
                        "The details are in canopus-setup.log, next to the program.' 2>/dev/null");
        return 1;
    }
    return StartGame(extra) ? 0 : 1;
}

#endif
