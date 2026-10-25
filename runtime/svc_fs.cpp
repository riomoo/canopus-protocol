// fs:USER, the file system: the game's RomFS (archive 3, SelfNCCH) and its
// save data (archive 4), kept in a folder next to the setup output.
#include "ipc.h"
#include "romfs.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <filesystem>
#include <map>

namespace fs = std::filesystem;

namespace rt {
namespace ipc {

namespace {

constexpr u32 RESULT_NOT_FOUND = 0xC8804478;
constexpr u32 RESULT_EXISTS = 0xC82044BE;
constexpr u32 RESULT_BAD_ARCHIVE = 0xC8804465;

RomFs s_romfs;
bool s_romfsReady = false;

std::string SaveRoot() { return g_dataDir + "/save"; }

// System data archives a console has in its own storage, which a game dump
// does not include; stand-ins are generated under <data>/sysdata.
//   000400DB00010302: the NG-word (profanity) list, checked when the player
//   types a name: version.dat and one word list per language, "%d.txt",
//   UTF-16 with a byte-order mark, a word per line. Ours blocks one word
//   nobody can type.
const RomFs* SystemArchive(u64 titleId)
{
    static std::map<u64, std::unique_ptr<RomFs>> built;
    auto it = built.find(titleId);
    if (it != built.end()) return it->second.get();
    char name[32];
    std::snprintf(name, sizeof name, "%016llx", (unsigned long long)titleId);
    const fs::path dir = fs::path(g_dataDir) / "sysdata" / name;
    std::error_code ec;
    if (titleId == 0x000400DB00010302ull) {
        fs::create_directories(dir, ec);
        auto put = [&](const std::string& file, const std::vector<u8>& data) {
            if (fs::exists(dir / file, ec)) return;
            if (FILE* f = std::fopen((dir / file).string().c_str(), "wb")) {
                std::fwrite(data.data(), 1, data.size(), f);
                std::fclose(f);
            }
        };
        put("version.dat", {1, 0, 0, 0});
        const std::vector<u8> list = {0xFF, 0xFE, 0x00, 0xE0, 0x01, 0xE0, 0x02, 0xE0, '\n', 0};
        for (int i = 0; i < 16; ++i) put(std::to_string(i) + ".txt", list);
    }
    if (!fs::is_directory(dir, ec)) {
        Log(LOG_WARN, "fs", "system archive %016llX is not available", (unsigned long long)titleId);
        built[titleId] = nullptr;
        return nullptr;
    }
    auto image = std::make_unique<RomFs>();
    if (!image->Build(dir.string(), dir.string() + ".index")) image.reset();
    Log(LOG_INFO, "fs", "system archive %016llX from %s", (unsigned long long)titleId, dir.string().c_str());
    return (built[titleId] = std::move(image)).get();
}



// A path from a request: type 1 empty, 2 binary, 3 ASCII, 4 UTF-16.
struct Path {
    u32 type = 1;
    std::vector<u8> bytes;
    std::string Text() const
    {
        std::string s;
        if (type == 3) {
            for (u8 b : bytes)
                if (b) s += (char)b;
        } else if (type == 4) {
            for (size_t i = 0; i + 1 < bytes.size(); i += 2) {
                const u32 c = bytes[i] | bytes[i + 1] << 8;
                if (!c) break;
                if (c < 0x80) s += (char)c;
                else if (c < 0x800) { s += (char)(0xC0 | c >> 6); s += (char)(0x80 | (c & 0x3F)); }
                else { s += (char)(0xE0 | c >> 12); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
            }
        }
        return s;
    }
    std::string Describe() const
    {
        if (type == 3 || type == 4) return "\"" + Text() + "\"";
        if (type == 1) return "(empty)";
        std::string s = "binary";
        char b[4];
        for (u8 x : bytes) { std::snprintf(b, sizeof b, " %02X", x); s += b; }
        return s;
    }
};

// The title id in an NCCH archive path (archive 0x2345678A).
// Add-on content (DLC): content `index` of the player's DLC, extracted by
// ds2rb_setup into data/dlc/<index>/.
const RomFs* DlcContent(u32 index)
{
    static std::map<u32, std::unique_ptr<RomFs>> built;
    auto it = built.find(index);
    if (it != built.end()) return it->second.get();
    const fs::path dir = fs::path(g_dataDir) / "dlc" / std::to_string(index);
    std::error_code ec;
    std::unique_ptr<RomFs> image;
    if (fs::is_directory(dir, ec)) {
        image = std::make_unique<RomFs>();
        if (!image->Build(dir.string(), dir.string() + ".index")) image.reset();
    }
    Log(image ? LOG_INFO : LOG_WARN, "fs", "DLC content %u %s", index, image ? "opened" : "is not installed");
    return (built[index] = std::move(image)).get();
}

u64 TitleOf(const Path& archivePath)
{
    u64 id = 0;
    if (archivePath.bytes.size() >= 8) std::memcpy(&id, archivePath.bytes.data(), 8);
    return id;
}

Path ReadPath(u32 type, u32 size, u32 ptr)
{
    Path p;
    p.type = type;
    if (ptr && size) p.bytes.assign(mem::Ptr(ptr), mem::Ptr(ptr) + size);
    return p;
}

// Host path for a path inside an archive root; refuses ".." and absolute parts.
bool HostPath(const std::string& root, const Path& p, fs::path* out)
{
    std::string t = p.Text();
    fs::path r(root);
    const fs::path tp(t);
    for (const auto& part : tp) {
        const std::string s = part.string();
        if (s == "/" || s.empty() || s == ".") continue;
        if (s == ".." || s.find(':') != std::string::npos || s.find('\\') != std::string::npos) return false;
        r /= part;
    }
    *out = r;
    return true;
}

// An open file: RomFS, or a host file in the save folder.
struct File : Service {
    File(std::string n) : Service(std::move(n)) {}
    const RomFs* romfs = nullptr;  // a RomFS image, or else...
    std::string host;              // ...a host file
    void Handle(Request& r) override
    {
        switch (r.id()) {
        case 0x0802: {  // Read(offset u64, size) -> bytes read
            const u64 off = (u64)r.cmd[2] << 32 | r.cmd[1];
            const u32 size = r.cmd[3], buf = r.cmd[5];
            u32 got = 0;
            {
                Unlocked unlocked(r);  // disk reads can be slow (virus scanners)
                if (romfs) {
                    got = romfs->Read(off, size, mem::Ptr(buf));
                } else if (FILE* f = std::fopen(host.c_str(), "rb")) {
                    std::fseek(f, (long)off, SEEK_SET);
                    got = (u32)std::fread(mem::Ptr(buf), 1, size, f);
                    std::fclose(f);
                }
            }
            Log(LOG_TRACE, "fs", "%s read %llX +%X -> %X at %08X", name.c_str(), (unsigned long long)off, size, got, buf);
            // DS2RB_WATCH=addr[:len]: once a read lands on addr, report who writes there after.
            static const char* watch = std::getenv("DS2RB_WATCH");
            if (watch) {
                const u32 wa = (u32)std::strtoul(watch, nullptr, 16);
                const char* colon = std::strchr(watch, ':');
                const u32 wl = colon ? (u32)std::strtoul(colon + 1, nullptr, 16) : 4;
                if (wa >= buf && wa < buf + got) {
                    WatchRange(wa, wa + wl);
                    WatchArm(wa, "a file was read there");
                }
            }
            r.Reply(2, 2);
            r.cmd[2] = got;
            r.cmd[3] = size << 4 | 0xC;
            r.cmd[4] = buf;
            return;
        }
        case 0x0803: {  // Write(offset u64, size, flush) -> bytes written
            const u64 off = (u64)r.cmd[2] << 32 | r.cmd[1];
            const u32 size = r.cmd[3], buf = r.cmd[6];
            u32 put = 0;
            if (!romfs) {
                Unlocked unlocked(r);
                FILE* f = std::fopen(host.c_str(), "r+b");
                if (!f) f = std::fopen(host.c_str(), "w+b");
                if (f) {
                    std::fseek(f, (long)off, SEEK_SET);
                    put = (u32)std::fwrite(mem::Ptr(buf), 1, size, f);
                    std::fclose(f);
                }
            }
            r.Reply(2, 2);
            r.cmd[2] = put;
            r.cmd[3] = size << 4 | 0xA;
            r.cmd[4] = buf;
            return;
        }
        case 0x0804: {  // GetSize
            u64 size = romfs ? romfs->Size() : 0;
            std::error_code ec;
            if (!romfs) size = fs::file_size(host, ec);
            r.Reply(3, 0);
            r.cmd[2] = (u32)size;
            r.cmd[3] = (u32)(size >> 32);
            return;
        }
        case 0x0805: {  // SetSize
            std::error_code ec;
            if (!romfs) fs::resize_file(host, (u64)r.cmd[2] << 32 | r.cmd[1], ec);
            r.Reply(1, 0);
            return;
        }
        case 0x0808:  // Close
        case 0x0809:  // Flush
        case 0x080A:  // SetPriority
            r.Reply(1, 0);
            return;
        case 0x080B:  // GetPriority
            r.Reply(2, 0);
            r.cmd[2] = 0;
            return;
        case 0x080C: {  // OpenLinkFile -> a second session on the same file
            auto f = std::make_shared<File>(name);
            f->romfs = romfs;
            f->host = host;
            r.Reply(1, 2);
            r.cmd[2] = 0x10;  // one handle, moved
            r.cmd[3] = kernel::NewHandle(std::make_shared<Session>(f));
            return;
        }
        }
        Unhandled(r);
    }
};

u32 OpenFileSession(const std::string& what, const RomFs* romfs, const std::string& host)
{
    auto f = std::make_shared<File>(what);
    f->romfs = romfs;
    f->host = host;
    return kernel::NewHandle(std::make_shared<Session>(f));
}

struct Archive {
    u32 id;
    std::string root;  // host folder (save data); empty for RomFS
    u64 titleId = 0;   // NCCH archives
};

struct Fs : Service {
    Fs() : Service("fs:USER") {}
    std::map<u64, Archive> archives;
    u64 nextArchive = 1;

    // Opens a file in an archive; flags 1 read, 2 write, 4 create.
    u32 Open(u32 archiveId, const std::string& root, const Path& path, u32 flags, u32* handle, u64 titleId = 0)
    {
        if (archiveId == 0x2345678A && (titleId >> 32) == 0x0004008C) {  // DLC: the file path names the content
            u32 index = 0;
            if (path.bytes.size() >= 8) std::memcpy(&index, path.bytes.data() + 4, 4);
            const RomFs* image = DlcContent(index);
            if (!image) return RESULT_NOT_FOUND;
            *handle = OpenFileSession("DLC content", image, "");
            return 0;
        }
        if (archiveId == 0x2345678A) {  // another title's content: system data
            const RomFs* image = SystemArchive(titleId);
            if (!image) return RESULT_NOT_FOUND;
            *handle = OpenFileSession("system archive", image, "");
            return 0;
        }
        if (archiveId == 3) {  // SelfNCCH: the RomFS
            if (!s_romfsReady) s_romfsReady = s_romfs.Build(g_dataDir + "/romfs", g_dataDir + "/romfs_index.bin");
            if (!s_romfsReady) return RESULT_NOT_FOUND;
            *handle = OpenFileSession("RomFS", &s_romfs, "");
            return 0;
        }
        fs::path host;
        if (root.empty() || !HostPath(root, path, &host)) return RESULT_NOT_FOUND;
        std::error_code ec;
        if (!fs::exists(host, ec)) {
            if (!(flags & 4)) return RESULT_NOT_FOUND;
            fs::create_directories(host.parent_path(), ec);
            if (FILE* f = std::fopen(host.string().c_str(), "wb")) std::fclose(f);
        }
        *handle = OpenFileSession(path.Text(), nullptr, host.string());
        return 0;
    }

    std::string RootOf(u32 archiveId)
    {
        switch (archiveId) {
        case 4: return SaveRoot();                  // save data
        case 6: case 7: return g_dataDir + "/extdata";
        case 9: return g_dataDir + "/sdmc";
        }
        return "";
    }

    void Handle(Request& r) override
    {
        auto& c = r.cmd;
        switch (r.id()) {
        case 0x0863:  // GetPriority
            r.Reply(2, 0);
            c[2] = 0;
            return;
        case 0x0861:  // InitializeWithSdkVersion
        case 0x0862:  // SetPriority
        case 0x0801:  // Dummy1
            r.Reply(1, 0);
            return;
        case 0x0803: {  // OpenFileDirectly
            const Path ap = ReadPath(c[3], c[4], c[10]);
            const Path fp = ReadPath(c[5], c[6], c[12]);
            u32 h = 0;
            const u32 res = Open(c[2], RootOf(c[2]), fp, c[7], &h, TitleOf(ap));
            Log(LOG_INFO, "fs", "OpenFileDirectly archive %X %s file %s flags %X -> %08X", c[2], ap.Describe().c_str(),
                fp.Describe().c_str(), c[7], res);
            r.Reply(1, 2, res);
            c[2] = 0x10;
            c[3] = h;
            return;
        }
        case 0x080C: {  // OpenArchive(id, path type, size) -> handle u64
            const Path ap = ReadPath(c[2], c[3], c[5]);
            const u32 id = c[1];
            const std::string root = id == 3 ? "" : RootOf(id);
            Log(LOG_INFO, "fs", "OpenArchive %X %s", id, ap.Describe().c_str());
            if (id != 3 && id != 0x2345678A && root.empty()) {
                r.Reply(1, 0, RESULT_BAD_ARCHIVE);
                return;
            }
            if (!root.empty()) {
                std::error_code ec;
                fs::create_directories(root, ec);
            }
            const u64 h = nextArchive++;
            archives[h] = {id, root, TitleOf(ap)};
            r.Reply(3, 0);
            c[2] = (u32)h;
            c[3] = (u32)(h >> 32);
            return;
        }
        case 0x080E:  // CloseArchive
            archives.erase((u64)c[2] << 32 | c[1]);
            r.Reply(1, 0);
            return;
        case 0x0802: {  // OpenFile(transaction, archive u64, path type, size, flags, attrs)
            auto it = archives.find((u64)c[3] << 32 | c[2]);
            const Path fp = ReadPath(c[4], c[5], c[9]);
            u32 h = 0, res = RESULT_BAD_ARCHIVE;
            if (it != archives.end()) res = Open(it->second.id, it->second.root, fp, c[6], &h, it->second.titleId);
            Log(LOG_INFO, "fs", "OpenFile %s flags %X -> %08X", fp.Describe().c_str(), c[6], res);
            r.Reply(1, 2, res);
            c[2] = 0x10;
            c[3] = h;
            return;
        }
        case 0x0808: {  // CreateFile(transaction, archive u64, path type, size, attrs, file size u64)
            auto it = archives.find((u64)c[3] << 32 | c[2]);
            const Path fp = ReadPath(c[4], c[5], c[10]);
            fs::path host;
            u32 res = RESULT_BAD_ARCHIVE;
            if (it != archives.end() && HostPath(it->second.root, fp, &host)) {
                std::error_code ec;
                if (fs::exists(host, ec)) {
                    res = RESULT_EXISTS;
                } else {
                    fs::create_directories(host.parent_path(), ec);
                    if (FILE* f = std::fopen(host.string().c_str(), "wb")) std::fclose(f);
                    fs::resize_file(host, (u64)c[8] << 32 | c[7], ec);
                    res = 0;
                }
            }
            Log(LOG_INFO, "fs", "CreateFile %s -> %08X", fp.Describe().c_str(), res);
            r.Reply(1, 0, res);
            return;
        }
        case 0x0804: {  // DeleteFile(transaction, archive u64, path type, size)
            auto it = archives.find((u64)c[3] << 32 | c[2]);
            const Path fp = ReadPath(c[4], c[5], c[7]);
            fs::path host;
            std::error_code ec;
            const bool ok = it != archives.end() && HostPath(it->second.root, fp, &host) && fs::remove(host, ec);
            r.Reply(1, 0, ok ? 0 : RESULT_NOT_FOUND);
            return;
        }
        case 0x0809: {  // CreateDirectory
            auto it = archives.find((u64)c[3] << 32 | c[2]);
            const Path fp = ReadPath(c[4], c[5], c[8]);
            fs::path host;
            std::error_code ec;
            if (it != archives.end() && HostPath(it->second.root, fp, &host)) fs::create_directories(host, ec);
            r.Reply(1, 0);
            return;
        }
        case 0x080D:  // ControlArchive (commit save data)
            r.Reply(1, 0);
            return;
        case 0x0817:  // IsSdmcDetected
        case 0x0818:  // IsSdmcWritable
            r.Reply(2, 0);
            c[2] = 0;
            return;
        case 0x084C:  // FormatSaveData
            r.Reply(1, 0);
            return;
        }
        Unhandled(r);
    }
};

}  // namespace

void RegisterFs() { Register("fs:USER", std::make_shared<Fs>()); }

}  // namespace ipc
}  // namespace rt
