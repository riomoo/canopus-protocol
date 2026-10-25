// am:app: the add-on content (DLC) the player owns. ds2rb_setup extracts the
// DLC from the player's own copy into data/dlc/<content index>/ and lists the
// contents in data/dlc/contents.txt ("index type id size" per line); this
// service reports those as installed and owned. The game then opens each
// content's RomFS through fs (archive 0x2345678A, svc_fs.cpp).
#include "ipc.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace rt {
namespace ipc {

namespace {

constexpr u64 kDlcTitle = 0x0004008C00159500ull;

struct Content {
    u16 index = 0, type = 0;
    u32 id = 0;
    u64 size = 0;
};

const std::vector<Content>& Contents()
{
    static const std::vector<Content> list = [] {
        std::vector<Content> v;
        std::ifstream in(g_dataDir + "/dlc/contents.txt");
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream s(line);
            Content c;
            unsigned index, type;
            unsigned long long size;
            s >> std::dec >> index >> std::hex >> type >> c.id >> std::dec >> size;
            if (!s) continue;
            c.index = (u16)index;
            c.type = (u16)type;
            c.size = size;
            v.push_back(c);
        }
        if (!v.empty()) Log(LOG_INFO, "am", "DLC: %zu contents", v.size());
        return v;
    }();
    return list;
}

// The 0x18-byte content info: index, type, id, size, state (installed | owned).
void WriteInfo(u32 va, const Content& c)
{
    mem::W16(va, c.index);
    mem::W16(va + 2, c.type);
    mem::W32(va + 4, c.id);
    mem::W32(va + 8, (u32)c.size);
    mem::W32(va + 12, (u32)(c.size >> 32));
    mem::W8(va + 16, 3);
    for (u32 i = 17; i < 0x18; ++i) mem::W8(va + i, 0);
}

struct Am : Service {
    Am() : Service("am:app") {}
    void Handle(Request& r) override
    {
        auto& c = r.cmd;
        auto title = [&](int at) { return (u64)c[at + 1] << 32 | c[at]; };
        const auto& list = Contents();
        Log(LOG_DEBUG, "am", "request %04X", r.id());
        switch (r.id()) {
        case 0x1001: {  // GetDLCContentInfoCount(media, title) -> count
            const u32 n = title(2) == kDlcTitle ? (u32)list.size() : 0;
            r.Reply(2, 0);
            c[2] = n;
            return;
        }
        case 0x1002: {  // FindDLCContentInfos(media, title, count, indices[], infos[])
            const u32 count = c[4], indices = c[6], out = c[8];
            for (u32 i = 0; i < count; ++i) {
                const u16 want = mem::R16(indices + 2 * i);
                Content found;
                found.index = want;
                for (const auto& e : list)
                    if (e.index == want) found = e;
                WriteInfo(out + 0x18 * i, found);
            }
            r.Reply(1, 4);
            c[2] = (count * 2) << 4 | 0xA;
            c[3] = indices;
            c[4] = (count * 0x18) << 4 | 0xC;
            c[5] = out;
            return;
        }
        case 0x1003: {  // ListDLCContentInfos(count, media, title, offset, infos[]) -> returned
            const u32 count = c[1], offset = c[5], out = c[7];
            u32 n = 0;
            if (title(3) == kDlcTitle)
                for (u32 i = offset; i < list.size() && n < count; ++i, ++n) WriteInfo(out + 0x18 * n, list[i]);
            r.Reply(2, 2);
            c[2] = n;
            c[3] = (count * 0x18) << 4 | 0xC;
            c[4] = out;
            return;
        }
        case 0x1007: {  // ListDLCTicketInfos(count, title, offset, infos[]) -> returned
            const u32 count = c[1], offset = c[4], out = c[6];
            u32 n = 0;
            if (title(2) == kDlcTitle && !list.empty() && offset == 0 && count > 0) {
                mem::W32(out, (u32)kDlcTitle);
                mem::W32(out + 4, (u32)(kDlcTitle >> 32));
                mem::W32(out + 8, 1);  // ticket id
                mem::W32(out + 12, 0);
                mem::W16(out + 16, 0);  // version
                mem::W16(out + 18, 0);
                mem::W32(out + 20, 0);
                n = 1;
            }
            r.Reply(2, 2);
            c[2] = n;
            c[3] = (count * 0x18) << 4 | 0xC;
            c[4] = out;
            return;
        }
        case 0x1009:  // IsDataTitleInUse(media, title) -> no
            r.Reply(2, 0);
            c[2] = 0;
            return;
        default:
            Log(LOG_INFO, "am", "command %04X (header %08X): success", r.id(), r.header());
            r.Reply(1, 0);
            return;
        }
    }
};

}  // namespace

void RegisterAm() { Register("am:app", std::make_shared<Am>()); }

}  // namespace ipc
}  // namespace rt
