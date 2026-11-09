#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Engine {

[[nodiscard]] std::uint64_t Fnv1a64(std::span<const std::byte> bytes, std::uint64_t hash = 0xcbf29ce484222325ull);

// --- Content pak -----------------------------------------------------------------------------------
// One file: a header, the entries' bytes (16-byte aligned), an index at the end and, when signed,
// an Ed25519 signature of the index after it. Entry paths are relative to the directory the pak is
// mounted at ('/'-separated UTF-8); names starting with ":cooked/" hold cooked data (textures,
// models) that is not visible as a file. Version 2 compresses entries with zstd where it pays off,
// keeps a BLAKE2b-256 digest of every entry's stored bytes (checked on each read; the signature
// covers them through the index) and can mark entries as deleted (patch paks hide the entry of
// older mounts). Version 1 paks (uncompressed, FNV-1a 64 per entry) are still read.

inline constexpr std::string_view kCookedPrefix = ":cooked/";
inline constexpr std::uint32_t    kPakVersion   = 2;
inline constexpr std::uint32_t    kPakEntryCompressed = 1u << 0; // zstd frame
inline constexpr std::uint32_t    kPakEntryDeleted    = 1u << 1; // patch: hides older mounts' entry

struct PakEntryInfo {
    std::string   path;
    std::uint64_t offset     = 0;
    std::uint64_t size       = 0; // bytes Read returns (after decompression)
    std::uint64_t hash       = 0; // FNV-1a 64 of those bytes (content identity, e.g. for patches)
    std::uint64_t storedSize = 0; // bytes in the file (size unless compressed; 0 for deleted entries)
    std::uint32_t flags      = 0; // kPakEntry*
    std::array<std::uint8_t, 32> digest{}; // BLAKE2b-256 of the stored bytes (version 2)

    [[nodiscard]] bool Compressed() const { return (flags & kPakEntryCompressed) != 0; }
    [[nodiscard]] bool Deleted() const { return (flags & kPakEntryDeleted) != 0; }
};

// Ed25519 keys for signed paks. The seed (32 bytes) is the private key; keep it out of the project.
using PakPublicKey = std::array<std::uint8_t, 32>;
struct PakKeyPair {
    std::array<std::uint8_t, 32> seed{};
    PakPublicKey                 publicKey{};
};
[[nodiscard]] PakKeyPair                  GeneratePakKey();                                   // random seed
[[nodiscard]] PakKeyPair                  PakKeyFromSeed(const std::array<std::uint8_t, 32>& seed);
[[nodiscard]] std::string                 HexString(std::span<const std::uint8_t> bytes);     // lowercase
[[nodiscard]] std::optional<PakPublicKey> ParsePakKeyHex(std::string_view hex);               // 64 hex digits

// How PakWriter::Add stores an entry: Auto compresses with zstd when that saves at least 10 %;
// None keeps the bytes as they are (e.g. sounds that are streamed from the file).
enum class PakCompression : std::uint8_t { Auto, None };

class PakWriter {
public:
    // compressionLevel: zstd level for PakCompression::Auto entries; 0 never compresses.
    explicit PakWriter(const std::filesystem::path& file, int compressionLevel = 9); // throws when it cannot be created
    ~PakWriter();                                          // without Finish: the file is removed
    PakWriter(const PakWriter&)            = delete;
    PakWriter& operator=(const PakWriter&) = delete;

    // Throw on invalid / duplicate paths and I/O errors.
    void Add(std::string_view path, std::span<const std::byte> bytes, PakCompression compression = PakCompression::Auto);
    void AddDeleted(std::string_view path); // a patch entry that hides `path` in older mounts
    // Writes the index (signed when `key` is given); throws.
    void Finish(const PakKeyPair* key = nullptr);

    [[nodiscard]] std::uint64_t BytesWritten() const { return m_Offset; }
    [[nodiscard]] const std::vector<PakEntryInfo>& Entries() const { return m_Entries; }

private:
    void AddEntry(PakEntryInfo entry, std::span<const std::byte> stored);

    std::filesystem::path     m_File;
    std::ofstream             m_Out;
    std::vector<PakEntryInfo> m_Entries;
    std::uint64_t             m_Offset   = 0;
    int                       m_Level    = 9;
    bool                      m_Finished = false;
};

class PakFile {
public:
    // Reads and checks the header and index; throws std::runtime_error. With `requiredKey` the pak
    // must be signed by it (unsigned, version 1 and wrongly signed paks throw).
    [[nodiscard]] static std::shared_ptr<const PakFile> Open(const std::filesystem::path& file,
                                                             const PakPublicKey* requiredKey = nullptr);

    // The entry for `path` (deleted entries included); case-insensitive on Windows.
    [[nodiscard]] const PakEntryInfo* Find(std::string_view path) const;
    // The entry's bytes, decompressed; throws on I/O errors, bad digests / hashes and deleted entries.
    [[nodiscard]] std::vector<std::byte> Read(const PakEntryInfo& entry) const;
    [[nodiscard]] const std::vector<PakEntryInfo>& Entries() const { return m_Entries; }
    [[nodiscard]] const std::filesystem::path&     File() const { return m_File; }
    [[nodiscard]] std::uint32_t                    Version() const { return m_Version; }
    [[nodiscard]] bool                             Signed() const { return m_SignedBy.has_value(); }
    [[nodiscard]] const std::optional<PakPublicKey>& SignedBy() const { return m_SignedBy; } // checked on open

private:
    std::filesystem::path                          m_File;
    std::vector<PakEntryInfo>                      m_Entries;
    std::unordered_map<std::string, std::size_t>   m_Index; // lookup key -> entry
    std::uint32_t                                  m_Version = kPakVersion;
    std::optional<PakPublicKey>                    m_SignedBy;
};

// --- Virtual file system ---------------------------------------------------------------------------
// Content reads go through here: files below a mounted pak's root come from the pak (newest mount
// first), everything else from the disk. Mount / unmount on the main thread before loading starts;
// the read functions are thread-safe.
namespace Vfs {

void Mount(std::shared_ptr<const PakFile> pak, const std::filesystem::path& root);
void UnmountAll();
[[nodiscard]] bool Mounted();

// The pak entry for `path`, else the disk file; nullopt when neither exists (or it cannot be read).
// A pak entry with a bad hash throws.
[[nodiscard]] std::optional<std::vector<std::byte>> Read(const std::filesystem::path& path);
// Like Read, throws std::runtime_error "cannot read '<path>'" when missing.
[[nodiscard]] std::vector<std::byte> ReadOrThrow(const std::filesystem::path& path);
[[nodiscard]] std::optional<std::string> ReadText(const std::filesystem::path& path);
[[nodiscard]] bool Exists(const std::filesystem::path& path); // a pak entry or a regular file
[[nodiscard]] bool IsPacked(const std::filesystem::path& path);

// Where the bytes of `path` live (streaming): the pak file and the entry's range, or the disk file
// (offset 0, its size).
struct Location {
    std::filesystem::path file;
    std::uint64_t         offset = 0;
    std::uint64_t         size   = 0;     // bytes of the content (decompressed)
    bool                  packed = false;
    bool                  compressed = false; // the range holds a zstd frame: use Read instead
};
[[nodiscard]] std::optional<Location> Locate(const std::filesystem::path& path);

// Last change: the disk file's time, or the pak's for packed files; min() when missing.
[[nodiscard]] std::filesystem::file_time_type ModifiedTime(const std::filesystem::path& path);

// Regular files in `directory` (and below when recursive) from the paks and the disk: absolute,
// lexically normal, sorted, without duplicates.
[[nodiscard]] std::vector<std::filesystem::path> ListFiles(const std::filesystem::path& directory, bool recursive);

// Cooked data (":cooked/<name>") from the mounted paks; nullopt when none has it.
[[nodiscard]] std::optional<std::vector<std::byte>> ReadCooked(std::string_view name);
// Cooked data for a file below a mount root: ":cooked/<category>/<path relative to the root>".
[[nodiscard]] std::optional<std::vector<std::byte>> ReadCookedFor(const std::filesystem::path& file,
                                                                  std::string_view category);

} // namespace Vfs

} // namespace Engine
