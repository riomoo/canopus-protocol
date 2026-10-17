#include "romfs.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace rt {

namespace {

constexpr u32 NONE = 0xFFFFFFFF;

std::u16string ToUtf16(const std::string& s)
{
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        u32 c = (u8)s[i];
        int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (n == 2) c &= 0x1F;
        else if (n == 3) c &= 0x0F;
        else if (n == 4) c &= 0x07;
        for (int k = 1; k < n && i + k < s.size(); ++k) c = c << 6 | ((u8)s[i + k] & 0x3F);
        i += n;
        if (c >= 0x10000) {
            c -= 0x10000;
            out += (char16_t)(0xD800 + (c >> 10));
            out += (char16_t)(0xDC00 + (c & 0x3FF));
        } else {
            out += (char16_t)c;
        }
    }
    return out;
}

u32 Hash(u32 parent, const std::u16string& name, u32 buckets)
{
    u32 h = parent ^ 123456789;
    for (char16_t ch : name) {
        h = (h >> 5) | (h << 27);
        h ^= ch;
    }
    return h % buckets;
}

u32 BucketCount(u32 n)
{
    if (n < 3) return 3;
    if (n < 19) return n | 1;
    while (n % 2 == 0 || n % 3 == 0 || n % 5 == 0 || n % 7 == 0 || n % 11 == 0 || n % 13 == 0 || n % 17 == 0) ++n;
    return n;
}

void Put32(std::vector<u8>& v, size_t o, u32 x) { std::memcpy(&v[o], &x, 4); }

struct Dir {
    std::u16string name;
    fs::path path;
    int parent = -1;
    std::vector<int> dirs, files;
    u32 offset = 0;
};

struct FileEntry {
    std::u16string name;
    fs::path path;
    int parent;
    u64 size;
    u32 offset = 0;
    u64 dataOffset = 0;
};

size_t EntrySize(size_t base, const std::u16string& name) { return base + ((name.size() * 2 + 3) & ~size_t(3)); }

}  // namespace

// Cache: "DS2RBROMFS1", meta size, meta, file count, then per file:
// offset, size, path length, path relative to the folder.
bool RomFs::Load(const std::string& folder, const std::string& cache)
{
    FILE* f = std::fopen(cache.c_str(), "rb");
    if (!f) return false;
    char magic[12] = {};
    u64 n = 0, count = 0;
    bool ok = std::fread(magic, 1, 12, f) == 12 && std::memcmp(magic, "DS2RBROMFS1", 12) == 0 &&
              std::fread(&n, 8, 1, f) == 1 && n < (1u << 30);
    if (ok) {
        m_meta.resize(n);
        ok = std::fread(m_meta.data(), 1, n, f) == n && std::fread(&m_dataOffset, 8, 1, f) == 1 &&
             std::fread(&m_size, 8, 1, f) == 1 && std::fread(&count, 8, 1, f) == 1 && count < 10000000;
    }
    m_files.clear();
    for (u64 i = 0; ok && i < count; ++i) {
        File e;
        u64 len = 0;
        ok = std::fread(&e.offset, 8, 1, f) == 1 && std::fread(&e.size, 8, 1, f) == 1 && std::fread(&len, 8, 1, f) == 1 && len < 4096;
        std::string rel(len, '\0');
        ok = ok && std::fread(&rel[0], 1, len, f) == len;
        e.hostPath = (fs::path(folder) / fs::u8path(rel)).string();
        m_files.push_back(e);
    }
    std::fclose(f);
    if (!ok) m_files.clear();
    return ok;
}

void RomFs::Save(const std::string& folder, const std::string& cache) const
{
    FILE* f = std::fopen(cache.c_str(), "wb");
    if (!f) return;
    const u64 n = m_meta.size(), count = m_files.size();
    std::fwrite("DS2RBROMFS1", 1, 12, f);
    std::fwrite(&n, 8, 1, f);
    std::fwrite(m_meta.data(), 1, n, f);
    std::fwrite(&m_dataOffset, 8, 1, f);
    std::fwrite(&m_size, 8, 1, f);
    std::fwrite(&count, 8, 1, f);
    for (auto& e : m_files) {
        const auto rel = fs::path(e.hostPath).lexically_relative(folder).generic_u8string();
        const u64 len = rel.size();
        std::fwrite(&e.offset, 8, 1, f);
        std::fwrite(&e.size, 8, 1, f);
        std::fwrite(&len, 8, 1, f);
        std::fwrite(rel.data(), 1, len, f);
    }
    std::fclose(f);
}

bool RomFs::Build(const std::string& folder, const std::string& cache)
{
    if (Load(folder, cache)) {
        Log(LOG_INFO, "romfs", "%zu files, %.1f MB (index from %s)", m_files.size(), m_size / 1048576.0, cache.c_str());
        return true;
    }
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) {
        Log(LOG_ERROR, "romfs", "no RomFS folder at %s", folder.c_str());
        return false;
    }
    // Walk the folder breadth first, names sorted.
    std::vector<Dir> dirs(1);
    std::vector<FileEntry> files;
    dirs[0].path = folder;
    for (size_t d = 0; d < dirs.size(); ++d) {
        std::vector<fs::directory_entry> list;
        for (auto& e : fs::directory_iterator(dirs[d].path, ec)) list.push_back(e);
        std::sort(list.begin(), list.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) {
            return a.path().filename().u8string() < b.path().filename().u8string();
        });
        for (auto& e : list) {
            const auto u8name = e.path().filename().u8string();
            const std::u16string name = ToUtf16(std::string(u8name.begin(), u8name.end()));
            if (e.is_directory(ec)) {
                Dir sub;
                sub.name = name;
                sub.path = e.path();
                sub.parent = (int)d;
                dirs[d].dirs.push_back((int)dirs.size());
                dirs.push_back(sub);
            } else if (e.is_regular_file(ec)) {
                FileEntry f{name, e.path(), (int)d, (u64)e.file_size(ec)};
                dirs[d].files.push_back((int)files.size());
                files.push_back(f);
            }
        }
    }
    // Metadata offsets.
    u32 at = 0;
    for (auto& d : dirs) {
        d.offset = at;
        at += (u32)EntrySize(0x18, d.name);
    }
    const u32 dirMetaSize = at;
    at = 0;
    u64 data = 0;
    for (auto& f : files) {
        f.offset = at;
        at += (u32)EntrySize(0x20, f.name);
        f.dataOffset = data;
        data = (data + f.size + 15) & ~u64(15);
    }
    const u32 fileMetaSize = at;
    const u32 dirBuckets = BucketCount((u32)dirs.size()), fileBuckets = BucketCount((u32)files.size());
    std::vector<u32> dirHash(dirBuckets, NONE), fileHash(fileBuckets, NONE);
    std::vector<u8> dmeta(dirMetaSize), fmeta(fileMetaSize);
    for (size_t i = 0; i < dirs.size(); ++i) {
        Dir& d = dirs[i];
        const u32 parent = d.parent < 0 ? 0 : dirs[d.parent].offset;
        u32 sibling = NONE;
        if (d.parent >= 0) {
            auto& sibs = dirs[d.parent].dirs;
            auto it = std::find(sibs.begin(), sibs.end(), (int)i);
            if (it + 1 != sibs.end()) sibling = dirs[*(it + 1)].offset;
        }
        const u32 b = Hash(parent, d.name, dirBuckets);
        Put32(dmeta, d.offset + 0x00, parent);
        Put32(dmeta, d.offset + 0x04, sibling);
        Put32(dmeta, d.offset + 0x08, d.dirs.empty() ? NONE : dirs[d.dirs[0]].offset);
        Put32(dmeta, d.offset + 0x0C, d.files.empty() ? NONE : files[d.files[0]].offset);
        Put32(dmeta, d.offset + 0x10, dirHash[b]);
        dirHash[b] = d.offset;
        Put32(dmeta, d.offset + 0x14, (u32)d.name.size() * 2);
        std::memcpy(&dmeta[d.offset + 0x18], d.name.data(), d.name.size() * 2);
    }
    for (size_t i = 0; i < files.size(); ++i) {
        FileEntry& f = files[i];
        const u32 parent = dirs[f.parent].offset;
        auto& sibs = dirs[f.parent].files;
        auto it = std::find(sibs.begin(), sibs.end(), (int)i);
        const u32 sibling = it + 1 != sibs.end() ? files[*(it + 1)].offset : NONE;
        const u32 b = Hash(parent, f.name, fileBuckets);
        Put32(fmeta, f.offset + 0x00, parent);
        Put32(fmeta, f.offset + 0x04, sibling);
        Put32(fmeta, f.offset + 0x08, (u32)f.dataOffset);
        Put32(fmeta, f.offset + 0x0C, (u32)(f.dataOffset >> 32));
        Put32(fmeta, f.offset + 0x10, (u32)f.size);
        Put32(fmeta, f.offset + 0x14, (u32)(f.size >> 32));
        Put32(fmeta, f.offset + 0x18, fileHash[b]);
        fileHash[b] = f.offset;
        Put32(fmeta, f.offset + 0x1C, (u32)f.name.size() * 2);
        std::memcpy(&fmeta[f.offset + 0x20], f.name.data(), f.name.size() * 2);
    }
    // Header and layout: header, dir hash, dir meta, file hash, file meta, data.
    m_meta.assign(0x28, 0);
    auto append = [&](const void* p, size_t n) {
        const size_t o = m_meta.size();
        m_meta.resize(o + n);
        std::memcpy(&m_meta[o], p, n);
        return (u32)o;
    };
    const u32 dh = append(dirHash.data(), dirHash.size() * 4);
    const u32 dm = append(dmeta.data(), dmeta.size());
    const u32 fh = append(fileHash.data(), fileHash.size() * 4);
    const u32 fm = append(fmeta.data(), fmeta.size());
    m_meta.resize((m_meta.size() + 15) & ~size_t(15));
    m_dataOffset = m_meta.size();
    Put32(m_meta, 0x00, 0x28);
    Put32(m_meta, 0x04, dh);
    Put32(m_meta, 0x08, (u32)dirHash.size() * 4);
    Put32(m_meta, 0x0C, dm);
    Put32(m_meta, 0x10, dirMetaSize);
    Put32(m_meta, 0x14, fh);
    Put32(m_meta, 0x18, (u32)fileHash.size() * 4);
    Put32(m_meta, 0x1C, fm);
    Put32(m_meta, 0x20, fileMetaSize);
    Put32(m_meta, 0x24, (u32)m_dataOffset);
    m_files.clear();
    for (auto& f : files) m_files.push_back({f.path.string(), m_dataOffset + f.dataOffset, f.size});
    m_size = m_dataOffset + data;
    Log(LOG_INFO, "romfs", "%zu folders, %zu files, %.1f MB", dirs.size(), files.size(), m_size / 1048576.0);
    Save(folder, cache);
    return true;
}

namespace {
// Files stay open between reads: opening one per read is slow, above all on
// Windows (virus scanners look at every open). A few recent ones are kept.
std::mutex s_openLock;
std::vector<std::pair<std::string, FILE*>> s_open;  // most recent last

FILE* OpenCached(const std::string& path)
{
    for (size_t i = 0; i < s_open.size(); ++i)
        if (s_open[i].first == path) {
            auto e = s_open[i];
            s_open.erase(s_open.begin() + i);
            s_open.push_back(e);
            return e.second;
        }
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return nullptr;
    if (s_open.size() >= 32) {
        std::fclose(s_open.front().second);
        s_open.erase(s_open.begin());
    }
    s_open.emplace_back(path, fp);
    return fp;
}
}  // namespace

u32 RomFs::Read(u64 offset, u32 size, u8* out) const
{
    if (offset >= m_size) return 0;
    if (offset + size > m_size) size = (u32)(m_size - offset);
    std::memset(out, 0, size);
    // Metadata.
    if (offset < m_meta.size()) {
        const u32 n = (u32)std::min<u64>(size, m_meta.size() - offset);
        std::memcpy(out, &m_meta[offset], n);
    }
    // File data overlapping the range.
    const u64 end = offset + size;
    auto it = std::upper_bound(m_files.begin(), m_files.end(), offset, [](u64 o, const File& f) { return o < f.offset; });
    if (it != m_files.begin()) --it;
    for (; it != m_files.end() && it->offset < end; ++it) {
        const u64 a = std::max(offset, it->offset), b = std::min(end, it->offset + it->size);
        if (a >= b) continue;
        std::lock_guard<std::mutex> g(s_openLock);
        FILE* fp = OpenCached(it->hostPath);
        if (!fp) {
            Log(LOG_ERROR, "romfs", "cannot read %s", it->hostPath.c_str());
            continue;
        }
        Log(LOG_TRACE, "romfs", "%s +%llX", it->hostPath.c_str(), (unsigned long long)(a - it->offset));
#ifdef _WIN32
        _fseeki64(fp, (long long)(a - it->offset), SEEK_SET);
#else
        fseeko(fp, (off_t)(a - it->offset), SEEK_SET);
#endif
        const size_t got = std::fread(out + (a - offset), 1, (size_t)(b - a), fp);
        (void)got;
    }
    return size;
}

}  // namespace rt
