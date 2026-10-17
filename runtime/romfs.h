// The game's RomFS as the 3DS file system serves it (the level-3 image of the
// IVFC tree: a 0x28-byte header, directory and file hash tables and
// metadata, then file data), rebuilt from the folder the setup tool
// extracted. Metadata is built at start; file data is read from the
// extracted files on demand.
#pragma once

#include "rt.h"

#include <string>
#include <vector>

namespace rt {

class RomFs {
public:
    // Builds the image from the folder, or loads the index saved by an
    // earlier run (cache), and saves it when built.
    bool Build(const std::string& folder, const std::string& cache);
    u64 Size() const { return m_size; }
    // Reads [offset, offset + size) of the image; returns bytes read.
    u32 Read(u64 offset, u32 size, u8* out) const;

private:
    bool Load(const std::string& folder, const std::string& cache);
    void Save(const std::string& folder, const std::string& cache) const;
    struct File {
        std::string hostPath;
        u64 offset, size;  // in the image
    };
    std::vector<u8> m_meta;     // header + tables (image offset 0)
    std::vector<File> m_files;  // sorted by offset
    u64 m_dataOffset = 0, m_size = 0;
};

}  // namespace rt
