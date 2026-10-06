#pragma once
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
// One uncompressed file: a header, the entries' bytes (16-byte aligned) and an index at the end.
// Entry paths are relative to the directory the pak is mounted at ('/'-separated UTF-8); names
// starting with ":cooked/" hold cooked data (textures, models) that is not visible as a file.
// Every entry carries its FNV-1a 64 hash (checked on each read), the index its own.

inline constexpr std::string_view kCookedPrefix = ":cooked/";
inline constexpr std::uint32_t    kPakVersion   = 1;

struct PakEntryInfo {
    std::string   path;
    std::uint64_t offset = 0;
    std::uint64_t size   = 0;
    std::uint64_t hash   = 0;
};

class PakWriter {
public:
    explicit PakWriter(const std::filesystem::path& file); // throws when it cannot be created
    ~PakWriter();                                          // without Finish: the file is removed
    PakWriter(const PakWriter&)            = delete;
    PakWriter& operator=(const PakWriter&) = delete;

    void Add(std::string_view path, std::span<const std::byte> bytes); // throws on duplicates / I/O errors
    void Finish();                                                       // writes the index; throws

    [[nodiscard]] std::uint64_t BytesWritten() const { return m_Offset; }

private:
    std::filesystem::path     m_File;
    std::ofstream             m_Out;
    std::vector<PakEntryInfo> m_Entries;
    std::uint64_t             m_Offset   = 0;
    bool                      m_Finished = false;
};

class PakFile {
public:
    // Reads and checks the header and index; throws std::runtime_error.
    [[nodiscard]] static std::shared_ptr<const PakFile> Open(const std::filesystem::path& file);

    [[nodiscard]] const PakEntryInfo* Find(std::string_view path) const; // case-insensitive on Windows
    [[nodiscard]] std::vector<std::byte> Read(const PakEntryInfo& entry) const; // throws on I/O error or bad hash
    [[nodiscard]] const std::vector<PakEntryInfo>& Entries() const { return m_Entries; }
    [[nodiscard]] const std::filesystem::path&     File() const { return m_File; }

private:
    std::filesystem::path                          m_File;
    std::vector<PakEntryInfo>                      m_Entries;
    std::unordered_map<std::string, std::size_t>   m_Index; // lookup key -> entry
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
    std::uint64_t         size   = 0;
    bool                  packed = false;
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
