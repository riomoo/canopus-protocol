#include "ctr.h"

#include <algorithm>
#include <cstring>

namespace ctr {

namespace {

constexpr uint64_t kMediaUnit = 0x200;

uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t le64(const uint8_t* p) { return uint64_t(le32(p)) | uint64_t(le32(p + 4)) << 32; }
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t be64(const uint8_t* p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }
uint64_t align(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

void require(bool ok, const std::string& what)
{
    if (!ok)
        throw Error(what);
}


// Size of a TMD/ticket signature block (signature + padding) by type.
uint64_t signatureBlockSize(uint32_t type)
{
    switch (type) {
    case 0x010000: case 0x010003: return 4 + 0x200 + 0x3C;
    case 0x010001: case 0x010004: return 4 + 0x100 + 0x3C;
    case 0x010002: case 0x010005: return 4 + 0x3C + 0x40;
    default: throw Error("unknown signature type in CIA metadata");
    }
}

// Validates a name read from the RomFS before using it as a path component.
std::filesystem::path safeName(const std::u16string& name)
{
    require(!name.empty() && name != u"." && name != u"..", "invalid name in RomFS");
    for (char16_t c : name)
        require(c != u'/' && c != u'\\' && c != u':' && c >= 0x20, "invalid character in RomFS name");
    return std::filesystem::path(name);
}

}  // namespace

NcchInfo parseNcch(File& f, uint64_t base)
{
    const auto h = f.readVec(base, 0x200);
    require(std::memcmp(&h[0x100], "NCCH", 4) == 0, "no NCCH header where one was expected");

    NcchInfo n;
    n.offset    = base;
    n.programId = le64(&h[0x118]);
    n.productCode.assign(reinterpret_cast<const char*>(&h[0x150]), 16);
    n.productCode = n.productCode.c_str();  // trim at the first NUL

    const uint8_t flags7 = h[0x18F];
    n.encrypted = (flags7 & 0x04) == 0;     // NoCrypto

    const uint32_t exhSize = le32(&h[0x180]);
    if (exhSize >= 0x400) {
        n.exheader = f.readVec(base + 0x200, 0x400);
        n.codeCompressed = !n.encrypted && (n.exheader[0x0D] & 1) != 0;
    }
    n.exefsOffset = base + uint64_t(le32(&h[0x1A0])) * kMediaUnit;
    n.exefsSize   = uint64_t(le32(&h[0x1A4])) * kMediaUnit;
    n.romfsOffset = base + uint64_t(le32(&h[0x1B0])) * kMediaUnit;
    n.romfsSize   = uint64_t(le32(&h[0x1B4])) * kMediaUnit;
    return n;
}

File::File(const std::filesystem::path& p) : in_(p, std::ios::binary)
{
    require(in_.good(), "cannot open " + p.u8string());
    in_.seekg(0, std::ios::end);
    size_ = static_cast<uint64_t>(in_.tellg());
}

void File::read(uint64_t offset, void* dst, size_t n)
{
    require(offset <= size_ && n <= size_ - offset, "read past end of file (truncated dump?)");
    in_.clear();
    in_.seekg(static_cast<std::streamoff>(offset));
    in_.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
    require(static_cast<size_t>(in_.gcount()) == n, "short read");
}

std::vector<uint8_t> File::readVec(uint64_t offset, size_t n)
{
    std::vector<uint8_t> v(n);
    if (n) read(offset, v.data(), n);
    return v;
}

Container open(File& f)
{
    require(f.size() >= 0x400, "file is too small to be a 3DS game");
    const auto head = f.readVec(0, 0x200);

    Container c{};
    if (std::memcmp(&head[0x100], "NCSD", 4) == 0) {
        c.kind = ContainerKind::NCSD;
        const uint64_t off  = uint64_t(le32(&head[0x120])) * kMediaUnit;  // partition 0
        const uint64_t size = uint64_t(le32(&head[0x124])) * kMediaUnit;
        require(size > 0, "NCSD has no game partition");
        c.main = parseNcch(f, off);
        return c;
    }

    if (std::memcmp(&head[0x100], "NCCH", 4) == 0) {
        c.kind = ContainerKind::NCCH;
        c.main = parseNcch(f, 0);
        return c;
    }

    if (le32(&head[0]) == 0x2020) {
        c.kind = ContainerKind::CIA;
        const uint64_t certSize   = le32(&head[0x08]);
        const uint64_t ticketSize = le32(&head[0x0C]);
        const uint64_t tmdSize    = le32(&head[0x10]);
        const uint64_t certOff    = align(0x2020, 64);
        const uint64_t ticketOff  = align(certOff + certSize, 64);
        const uint64_t tmdOff     = align(ticketOff + ticketSize, 64);
        const uint64_t contentOff = align(tmdOff + tmdSize, 64);

        const auto tmd = f.readVec(tmdOff, tmdSize);
        require(tmd.size() >= 4, "CIA has no TMD");
        const uint64_t hdr = signatureBlockSize(be32(&tmd[0]));
        require(tmd.size() >= hdr + 0xC4 + 0x900 + 0x30, "CIA TMD is truncated");
        c.titleId = be64(&tmd[hdr + 0x4C]);
        const uint16_t contentCount = uint16_t(tmd[hdr + 0x9E] << 8 | tmd[hdr + 0x9F]);
        require(contentCount > 0, "CIA has no contents");

        c.titleVersion = uint16_t(tmd[hdr + 0x9C] << 8 | tmd[hdr + 0x9D]);
        require(tmd.size() >= hdr + 0xC4 + 0x900 + 0x30 * uint64_t(contentCount), "CIA TMD is truncated");

        // Content chunk records follow the header and the 64 info records;
        // the contents are stored in that order, each 64-byte aligned.
        uint64_t at = contentOff;
        for (uint16_t i = 0; i < contentCount; ++i) {
            const uint8_t* ch = &tmd[hdr + 0xC4 + 0x900 + 0x30 * i];
            CiaContent cc;
            cc.id = be32(ch);
            cc.index = uint16_t(ch[4] << 8 | ch[5]);
            cc.type = uint16_t(ch[6] << 8 | ch[7]);
            cc.size = be64(ch + 8);
            cc.offset = at;
            cc.encrypted = (cc.type & 1) != 0;
            c.contents.push_back(cc);
            at = align(at + cc.size, 64);
        }
        const uint16_t type = c.contents[0].type;
        c.ciaContentEncrypted = (type & 1) != 0;
        if (c.ciaContentEncrypted)
            return c;  // caller reports it; the NCCH can't be read
        c.main = parseNcch(f, contentOff);
        return c;
    }

    throw Error("not a 3DS game file (.3ds, .cci, .cia or .cxi)");
}

std::vector<uint8_t> blzDecompress(const std::vector<uint8_t>& in)
{
    require(in.size() >= 8, "compressed code is too small");
    const uint32_t topAndBottom = le32(&in[in.size() - 8]);
    const uint32_t extra        = le32(&in[in.size() - 4]);
    const size_t headerLen      = topAndBottom >> 24;
    const size_t compressedLen  = topAndBottom & 0xFFFFFF;
    require(headerLen >= 8 && headerLen <= in.size() && compressedLen <= in.size(), "bad compressed code footer");

    std::vector<uint8_t> out(in.size() + extra);
    std::copy(in.begin(), in.end(), out.begin());

    size_t src = in.size() - headerLen;      // read cursor, moves down
    size_t dst = out.size();                 // write cursor, moves down
    const size_t stop = in.size() - compressedLen;

    while (src > stop) {
        const uint8_t flags = in[--src];
        for (int bit = 0; bit < 8 && src > stop; ++bit) {
            if ((flags << bit) & 0x80) {
                require(src >= stop + 2, "corrupt compressed code");
                const uint8_t hi = in[--src];
                const uint8_t lo = in[--src];
                const size_t length = (hi >> 4) + 3;
                const size_t offset = (size_t(hi & 0x0F) << 8 | lo) + 3;
                for (size_t j = 0; j < length; ++j) {
                    require(dst > 0 && dst - 1 + offset < out.size(), "corrupt compressed code");
                    --dst;
                    out[dst] = out[dst + offset];
                }
            } else {
                require(dst > 0, "corrupt compressed code");
                out[--dst] = in[--src];
            }
        }
    }
    return out;
}

std::vector<uint8_t> readCode(File& f, const NcchInfo& n)
{
    require(n.exefsSize >= 0x200, "game has no ExeFS");
    const auto h = f.readVec(n.exefsOffset, 0x200);
    for (int i = 0; i < 10; ++i) {
        const uint8_t* e = &h[i * 16];
        if (std::memcmp(e, ".code\0\0\0", 8) != 0)
            continue;
        const uint32_t off = le32(e + 8), size = le32(e + 12);
        require(0x200 + uint64_t(off) + size <= n.exefsSize, "ExeFS .code entry is out of range");
        auto code = f.readVec(n.exefsOffset + 0x200 + off, size);
        return n.codeCompressed ? blzDecompress(code) : code;
    }
    throw Error("ExeFS has no .code");
}

size_t extractRomfs(File& f, const NcchInfo& n, const std::filesystem::path& outDir,
                    const std::function<void(uint64_t, uint64_t)>& progress)
{
    require(n.romfsSize >= 0x60, "game has no RomFS");
    const auto ivfc = f.readVec(n.romfsOffset, 0x60);
    require(std::memcmp(ivfc.data(), "IVFC", 4) == 0, "RomFS is missing its IVFC header");
    const uint32_t masterHashSize = le32(&ivfc[0x08]);
    const uint32_t l3BlockLog2    = le32(&ivfc[0x4C]);
    require(l3BlockLog2 < 32, "bad RomFS block size");
    const uint64_t l3 = n.romfsOffset + align(0x60 + uint64_t(masterHashSize), uint64_t(1) << l3BlockLog2);

    const auto hdr = f.readVec(l3, 0x28);
    require(le32(&hdr[0]) == 0x28, "unexpected RomFS header size");
    const uint64_t dirMetaOff  = le32(&hdr[0x0C]), dirMetaSize  = le32(&hdr[0x10]);
    const uint64_t fileMetaOff = le32(&hdr[0x1C]), fileMetaSize = le32(&hdr[0x20]);
    const uint64_t dataOff     = le32(&hdr[0x24]);
    const auto dirs  = f.readVec(l3 + dirMetaOff, dirMetaSize);
    const auto files = f.readVec(l3 + fileMetaOff, fileMetaSize);

    auto nameAt = [](const std::vector<uint8_t>& table, uint32_t at, uint32_t nameField, uint32_t lenField) {
        require(uint64_t(at) + nameField <= table.size(), "RomFS entry out of range");
        const uint32_t len = le32(&table[at + lenField]);
        require(len % 2 == 0 && uint64_t(at) + nameField + len <= table.size(), "RomFS name out of range");
        std::u16string s;
        for (uint32_t i = 0; i < len; i += 2) s += char16_t(le16(&table[at + nameField + i]));
        return s;
    };

    // Total size first, for progress.
    uint64_t total = 0;
    for (uint32_t at = 0; at + 0x20 <= files.size();) {
        total += le64(&files[at + 0x10]);
        at += 0x20 + uint32_t(align(le32(&files[at + 0x1C]), 4));
    }

    std::filesystem::create_directories(outDir);
    std::vector<char> buf(1 << 20);
    uint64_t done = 0;
    size_t written = 0;
    size_t guard = 0;
    constexpr uint32_t kNone = 0xFFFFFFFF;

    std::function<void(uint32_t, const std::filesystem::path&)> walk = [&](uint32_t dirAt, const std::filesystem::path& here) {
        require(++guard < 1000000, "RomFS directory tree loops");
        require(uint64_t(dirAt) + 0x18 <= dirs.size(), "RomFS directory out of range");

        for (uint32_t fileAt = le32(&dirs[dirAt + 0x0C]); fileAt != kNone;) {
            require(uint64_t(fileAt) + 0x20 <= files.size(), "RomFS file entry out of range");
            const uint64_t off  = le64(&files[fileAt + 0x08]);
            const uint64_t size = le64(&files[fileAt + 0x10]);
            const auto path = here / safeName(nameAt(files, fileAt, 0x20, 0x1C));

            std::ofstream out(path, std::ios::binary);
            require(out.good(), "cannot write " + path.u8string());
            for (uint64_t pos = 0; pos < size;) {
                const size_t chunk = size_t(std::min<uint64_t>(buf.size(), size - pos));
                f.read(l3 + dataOff + off + pos, buf.data(), chunk);
                out.write(buf.data(), static_cast<std::streamsize>(chunk));
                pos += chunk;
                done += chunk;
                if (progress) progress(done, total);
            }
            require(out.good(), "write failed for " + path.u8string() + " (disk full?)");
            ++written;
            fileAt = le32(&files[fileAt + 0x04]);
        }

        for (uint32_t child = le32(&dirs[dirAt + 0x08]); child != kNone;) {
            const auto path = here / safeName(nameAt(dirs, child, 0x18, 0x14));
            std::filesystem::create_directories(path);
            walk(child, path);
            child = le32(&dirs[child + 0x04]);
        }
    };

    walk(0, outDir);
    return written;
}

}  // namespace ctr
