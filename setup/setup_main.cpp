// ds2rb_setup: prepares game data for the Devil Survivor 2 Record Breaker port.
//
// Drop a decrypted .3ds, .cci, .cia or .cxi of your own copy into the
// "games" folder next to this program and run it. It extracts code.bin and
// the RomFS into the "data" folder. If the 1.1 update (.cia) is there too,
// its code replaces the original's; the DLC (.cia) goes into data/dlc.
// Nothing from the game ships with the port.
//
//   ds2rb_setup [games_folder] [data_folder]

#include "ctr.h"
#include "sha256.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <map>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

// Game versions the decompilation supports, by SHA-256 of the decompressed code.bin.
const std::map<std::string, std::string> kSupportedCode = {
    {"4b2d68fafc4ff9715b69a4be7637a487f7c38ad74654fd83d895300f64f58458", "release used for the decompilation"},
#ifdef DS2RB_TEST_CODE_HASH
    {DS2RB_TEST_CODE_HASH, "test build"},
#endif
};

// The 1.1 update's code, by SHA-256 of the decompressed code.bin.
const std::map<std::string, std::string> kSupportedUpdate = {
    {"fac5ce8f1244c978ed9cce049f4c988d93bbc6401aeab4729caec3b196be3487", "1.1"},
};

constexpr uint64_t kGameTitle = 0x0004000000159500ull;
constexpr uint64_t kUpdateTitle = 0x0004000E00159500ull;
constexpr uint64_t kDlcTitle = 0x0004008C00159500ull;

std::string hex64(uint64_t v)
{
    char b[17];
    std::snprintf(b, sizeof b, "%016llX", static_cast<unsigned long long>(v));
    return b;
}

bool isGameFile(const fs::path& p)
{
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext == ".3ds" || ext == ".cci" || ext == ".cia" || ext == ".cxi";
}

std::string fingerprint(const fs::path& p)
{
    const auto t = fs::last_write_time(p).time_since_epoch().count();
    return p.filename().u8string() + "|" + std::to_string(fs::file_size(p)) + "|" + std::to_string(static_cast<long long>(t));
}

std::string readText(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void writeFile(const fs::path& p, const void* data, size_t n)
{
    std::ofstream out(p, std::ios::binary);
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
    if (!out.good())
        throw ctr::Error("cannot write " + p.u8string());
}

const char* kEncryptedHelp =
    "  This dump is encrypted. The port can only read decrypted dumps.\n"
    "  Make one from your own console with GodMode9, or decrypt this file\n"
    "  with a tool that sets the NoCrypto flag, then drop the result here.\n";

// Returns true if this file was the game and setup succeeded.
bool processGame(const fs::path& file, const fs::path& dataDir)
{
    std::cout << "\n" << file.filename().u8string() << " (" << fs::file_size(file) / (1024 * 1024) << " MB)\n";
    ctr::File f(file);
    const ctr::Container c = ctr::open(f);

    if (c.kind == ctr::ContainerKind::CIA) {
        std::cout << "  CIA, title " << hex64(c.titleId) << "\n";
        if (c.ciaContentEncrypted) {
            std::cout << kEncryptedHelp;
            return false;
        }
    }

    const ctr::NcchInfo& n = c.main;
    std::cout << "  " << (n.productCode.empty() ? "(no product code)" : n.productCode)
              << ", program " << hex64(n.programId) << "\n";
    if (n.encrypted) {
        std::cout << kEncryptedHelp;
        return false;
    }

    const fs::path marker = dataDir / "setup_done.txt";
    if (fs::exists(marker) && readText(marker) == fingerprint(file) && fs::exists(dataDir / "code.bin")) {
        std::cout << "  Already set up from this file; nothing to do.\n";
        return true;
    }

    std::cout << "  Reading code... " << std::flush;
    const std::vector<uint8_t> code = ctr::readCode(f, n);
    Sha256 sha;
    sha.update(code.data(), code.size());
    const std::string digest = sha.hex();
    const auto known = kSupportedCode.find(digest);
    if (known == kSupportedCode.end()) {
        std::cout << "\n  This version of the game isn't supported (code fingerprint " << digest.substr(0, 16) << "...).\n"
                  << "  It may be a different region or have an update applied.\n";
        return false;
    }
    std::cout << "OK (" << known->second << ")\n";

    fs::create_directories(dataDir);
    fs::remove(marker);
    writeFile(dataDir / "code.bin", code.data(), code.size());
    if (!n.exheader.empty())
        writeFile(dataDir / "exheader.bin", n.exheader.data(), n.exheader.size());
    std::ofstream(dataDir / "version.txt", std::ios::binary) << "1.0";
    fs::remove(dataDir / "update_done.txt");

    const fs::path romfsDir = dataDir / "romfs";
    if (fs::exists(romfsDir))
        fs::remove_all(romfsDir);

    std::cout << "  Extracting game data...\n";
    int lastPercent = -1;
    const auto start = std::chrono::steady_clock::now();
    const size_t files = ctr::extractRomfs(f, n, romfsDir, [&](uint64_t done, uint64_t total) {
        const int percent = total ? int(done * 100 / total) : 100;
        if (percent != lastPercent) {
            lastPercent = percent;
            std::cout << "\r  " << percent << "%   " << std::flush;
        }
    });
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << "\r  " << files << " files extracted in " << int(secs) << "s\n";

    std::ofstream(marker, std::ios::binary) << fingerprint(file);
    return true;
}

// The update: its code replaces the original game's (the RomFS stays).
bool processUpdate(const fs::path& file, ctr::File& f, const ctr::Container& c, const fs::path& dataDir)
{
    std::cout << "\n" << file.filename().u8string() << ": update\n";
    if (c.ciaContentEncrypted || c.main.encrypted) {
        std::cout << kEncryptedHelp;
        return false;
    }
    if (!fs::exists(dataDir / "code.bin")) {
        std::cout << "  Set up the game itself first (put its .3ds or .cia in the same folder).\n";
        return false;
    }
    const fs::path marker = dataDir / "update_done.txt";
    if (fs::exists(marker) && readText(marker) == fingerprint(file) && readText(dataDir / "version.txt") != "1.0") {
        std::cout << "  Already installed; nothing to do.\n";
        return true;
    }
    std::cout << "  Reading code... " << std::flush;
    const std::vector<uint8_t> code = ctr::readCode(f, c.main);
    Sha256 sha;
    sha.update(code.data(), code.size());
    const auto known = kSupportedUpdate.find(sha.hex());
    if (known == kSupportedUpdate.end()) {
        std::cout << "\n  This update isn't supported (code fingerprint " << sha.hex().substr(0, 16) << "...).\n";
        return false;
    }
    std::cout << "OK (version " << known->second << ")\n";
    writeFile(dataDir / "code.bin", code.data(), code.size());
    writeFile(dataDir / "exheader.bin", c.main.exheader.data(), c.main.exheader.size());
    std::ofstream(dataDir / "version.txt", std::ios::binary) << known->second;
    std::ofstream(marker, std::ios::binary) << fingerprint(file);
    std::cout << "  The game will now run as version " << known->second << ". Run 'make recomp' again.\n";
    return true;
}

// The DLC: each content's RomFS goes into data/dlc/<content index>/, and
// data/dlc/contents.txt lists them for the port's am:app service.
bool processDlc(const fs::path& file, ctr::File& f, const ctr::Container& c, const fs::path& dataDir)
{
    std::cout << "\n" << file.filename().u8string() << ": DLC, " << c.contents.size() << " contents\n";
    const fs::path dlcDir = dataDir / "dlc";
    const fs::path marker = dataDir / "dlc_done.txt";
    if (fs::exists(marker) && readText(marker) == fingerprint(file) && fs::exists(dlcDir / "contents.txt")) {
        std::cout << "  Already installed; nothing to do.\n";
        return true;
    }
    fs::remove(marker);
    if (fs::exists(dlcDir))
        fs::remove_all(dlcDir);
    fs::create_directories(dlcDir);
    std::ofstream list(dlcDir / "contents.txt", std::ios::binary);
    list << "# index type id size\n";
    size_t done = 0;
    for (const auto& cc : c.contents) {
        if (cc.encrypted) {
            std::cout << "  Content " << cc.index << " is encrypted; skipped.\n" << kEncryptedHelp;
            continue;
        }
        const ctr::NcchInfo n = ctr::parseNcch(f, cc.offset);
        if (n.encrypted) {
            std::cout << "  Content " << cc.index << " is encrypted; skipped.\n" << kEncryptedHelp;
            continue;
        }
        if (n.romfsSize)
            ctr::extractRomfs(f, n, dlcDir / std::to_string(cc.index), [](uint64_t, uint64_t) {});
        char line[96];
        std::snprintf(line, sizeof line, "%u %04x %08x %llu\n", cc.index, cc.type, cc.id, (unsigned long long)cc.size);
        list << line;
        ++done;
    }
    list.close();
    std::cout << "  " << done << " contents installed.\n";
    if (done)
        std::ofstream(marker, std::ios::binary) << fingerprint(file);
    return done > 0;
}

}  // namespace

namespace {

// When double-clicked there's no console left afterwards, so wait for Enter.
int finish(int code, bool interactive)
{
    if (interactive) {
        std::cout << "\nPress Enter to close.";
        std::string line;
        std::getline(std::cin, line);
    }
    return code;
}

}  // namespace

int main(int argc, char** argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    const bool interactive = argc == 1;
    const fs::path exeDir   = fs::absolute(argv[0]).parent_path();
    const fs::path gamesDir = argc > 1 ? fs::path(argv[1]) : exeDir / "games";
    const fs::path dataDir  = argc > 2 ? fs::path(argv[2]) : exeDir / "data";

    std::cout << "Devil Survivor 2 Record Breaker - game data setup\n";
    if (!fs::exists(gamesDir)) {
        fs::create_directories(gamesDir);
        std::cout << "Created " << gamesDir.u8string() << ".\n"
                  << "Put a decrypted .3ds or .cia of your copy in it and run this again.\n";
        return finish(1, interactive);
    }

    // The game first, then its update, then the DLC.
    struct Item {
        fs::path path;
        int order;  // 0 game, 1 update, 2 DLC
    };
    std::vector<Item> items;
    int seen = 0;
    for (const auto& entry : fs::directory_iterator(gamesDir)) {
        if (!entry.is_regular_file() || !isGameFile(entry.path()))
            continue;
        ++seen;
        int order = 0;
        try {
            ctr::File f(entry.path());
            const ctr::Container c = ctr::open(f);
            if (c.titleId == kUpdateTitle) order = 1;
            else if (c.titleId == kDlcTitle) order = 2;
            else if ((c.titleId >> 32) == 0x0004000E || (c.titleId >> 32) == 0x0004008C) order = 3;  // another game's
        } catch (const std::exception&) {
        }
        items.push_back({entry.path(), order});
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.order < b.order; });

    bool ok = false;
    for (const auto& item : items) {
        try {
            if (item.order == 0) {
                if (!ok) ok = processGame(item.path, dataDir);
            } else if (item.order == 3) {
                std::cout << "\n" << item.path.filename().u8string() << ": an update or DLC for another game; skipped.\n";
            } else {
                ctr::File f(item.path);
                const ctr::Container c = ctr::open(f);
                if (item.order == 1) processUpdate(item.path, f, c, dataDir);
                else processDlc(item.path, f, c, dataDir);
            }
        } catch (const std::exception& e) {
            std::cout << "  Error: " << e.what() << "\n";
        }
    }
    (void)kGameTitle;

    if (seen == 0)
        std::cout << "No .3ds, .cci, .cia or .cxi files in " << gamesDir.u8string() << ".\n";
    // The game itself may have been set up on an earlier run (only the update
    // or DLC is in the folder this time).
    if (!ok && fs::exists(dataDir / "setup_done.txt") && fs::exists(dataDir / "romfs"))
        ok = true;
    else if (!ok && fs::exists(dataDir / "code.bin"))
        std::cout << "\nThe game itself isn't set up yet: put your game dump in " << gamesDir.u8string() << " too.\n";
    std::cout << (ok ? "\nSetup complete.\n" : "\nSetup did not finish.\n");
    return finish(ok ? 0 : 1, interactive);
}
