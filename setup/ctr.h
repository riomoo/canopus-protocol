// Reading 3DS game containers: .3ds/.cci (NCSD), .cia, .cxi (NCCH).
// Formats as documented on 3dbrew.org. Only decrypted dumps are supported.
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace ctr {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Random-access reads from a large file.
class File {
public:
    explicit File(const std::filesystem::path& p);
    void     read(uint64_t offset, void* dst, size_t n);
    uint64_t size() const { return size_; }
    std::vector<uint8_t> readVec(uint64_t offset, size_t n);

private:
    std::ifstream in_;
    uint64_t size_ = 0;
};

enum class ContainerKind { NCSD, CIA, NCCH };

struct NcchInfo {
    uint64_t    offset = 0;        // absolute offset of the NCCH in the file
    uint64_t    programId = 0;
    std::string productCode;       // e.g. "CTR-P-XXXX"
    bool        encrypted = false; // NoCrypto flag not set
    bool        codeCompressed = false;
    uint64_t    exefsOffset = 0, exefsSize = 0;  // absolute
    uint64_t    romfsOffset = 0, romfsSize = 0;  // absolute
    std::vector<uint8_t> exheader;               // 0x400 bytes
};

struct Container {
    ContainerKind kind;
    uint64_t      titleId = 0;          // CIA only
    bool          ciaContentEncrypted = false;
    NcchInfo      main;                 // the game partition / content 0
};

// Identifies the container and parses its main NCCH. Throws ctr::Error.
Container open(File& f);

// Returns .code from the ExeFS, decompressed if needed.
std::vector<uint8_t> readCode(File& f, const NcchInfo& n);

// Reverse-LZ ("BLZ") decompression used for .code.
std::vector<uint8_t> blzDecompress(const std::vector<uint8_t>& in);

// Extracts the RomFS tree under outDir. progress(bytesDone, bytesTotal).
// Returns the number of files written.
size_t extractRomfs(File& f, const NcchInfo& n, const std::filesystem::path& outDir,
                    const std::function<void(uint64_t, uint64_t)>& progress);

}  // namespace ctr
