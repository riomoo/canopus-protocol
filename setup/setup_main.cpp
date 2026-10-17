// ds2rb_setup: prepares game data for the Devil Survivor 2 Record Breaker port.
//
// Drop a decrypted .3ds, .cci, .cia or .cxi of your own copy into the
// "games" folder next to this program and run it. It extracts code.bin and
// the RomFS into the "data" folder. Nothing from the game ships with the port.
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
        const uint32_t high = uint32_t(c.titleId >> 32);
        std::cout << "  CIA, title " << hex64(c.titleId) << "\n";
        if (high == 0x0004008C) {
            std::cout << "  This is DLC. DLC support comes after the first playable version; skipping.\n";
            return false;
        }
        if (high == 0x0004000E) {
            std::cout << "  This is an update, not the game itself; skipping.\n";
            return false;
        }
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

    bool ok = false;
    int seen = 0;
    for (const auto& entry : fs::directory_iterator(gamesDir)) {
        if (!entry.is_regular_file() || !isGameFile(entry.path()))
            continue;
        ++seen;
        try {
            ok = processGame(entry.path(), dataDir) || ok;
        } catch (const std::exception& e) {
            std::cout << "  Error: " << e.what() << "\n";
        }
        if (ok)
            break;
    }

    if (seen == 0)
        std::cout << "No .3ds, .cci, .cia or .cxi files in " << gamesDir.u8string() << ".\n";
    std::cout << (ok ? "\nSetup complete.\n" : "\nSetup did not finish.\n");
    return finish(ok ? 0 : 1, interactive);
}
