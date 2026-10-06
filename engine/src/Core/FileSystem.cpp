#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Platform.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstring>
#include <format>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>

namespace Engine {

namespace fs = std::filesystem;

static_assert(std::endian::native == std::endian::little, "the pak format is little-endian");

std::uint64_t Fnv1a64(std::span<const std::byte> bytes, std::uint64_t hash)
{
    for (const std::byte b : bytes) {
        hash ^= static_cast<std::uint64_t>(b);
        hash *= 0x100000001b3ull;
    }
    return hash;
}

namespace {

constexpr char          kMagic[4]  = {'U', 'P', 'A', 'K'};
constexpr std::uint64_t kAlignment = 16;

struct Header {
    char          magic[4]    = {};
    std::uint32_t version     = 0;
    std::uint32_t entryCount  = 0;
    std::uint32_t flags       = 0;
    std::uint64_t indexOffset = 0;
    std::uint64_t indexSize   = 0;
    std::uint64_t indexHash   = 0;
};
static_assert(sizeof(Header) == 40);

// Index lookups: entry paths as stored; Windows file names are case-insensitive.
std::string LookupKey(std::string_view path)
{
    std::string key(path);
#ifdef _WIN32
    std::ranges::transform(key, key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return key;
}

std::string GenericUtf8(const fs::path& path)
{
    const std::u8string s = path.generic_u8string();
    return {s.begin(), s.end()};
}

template <class T>
void Append(std::vector<std::byte>& out, const T& value)
{
    const auto* p = reinterpret_cast<const std::byte*>(&value);
    out.insert(out.end(), p, p + sizeof(T));
}

template <class T>
T Take(std::span<const std::byte> in, std::size_t& at)
{
    if (at + sizeof(T) > in.size())
        throw std::runtime_error("truncated pak index");
    T value;
    std::memcpy(&value, in.data() + at, sizeof(T));
    at += sizeof(T);
    return value;
}

} // namespace

// --- PakWriter ---------------------------------------------------------------------------------------

PakWriter::PakWriter(const fs::path& file) : m_File(file)
{
    std::error_code ec;
    if (file.has_parent_path())
        fs::create_directories(file.parent_path(), ec);
    m_Out.open(file, std::ios::binary | std::ios::trunc);
    if (!m_Out)
        throw std::runtime_error("cannot create '" + PathToUtf8(file) + "'");
    const Header placeholder{};
    m_Out.write(reinterpret_cast<const char*>(&placeholder), sizeof(placeholder));
    m_Offset = sizeof(Header);
}

PakWriter::~PakWriter()
{
    if (m_Finished)
        return;
    m_Out.close();
    std::error_code ec;
    fs::remove(m_File, ec); // incomplete: never leave a pak without an index behind
}

void PakWriter::Add(std::string_view path, std::span<const std::byte> bytes)
{
    if (m_Finished)
        throw std::logic_error("PakWriter: already finished");
    if (path.empty())
        throw std::invalid_argument("PakWriter: empty entry path");
    const std::string key = LookupKey(path);
    for (const PakEntryInfo& e : m_Entries)
        if (LookupKey(e.path) == key)
            throw std::invalid_argument(std::format("PakWriter: duplicate entry '{}'", path));
    static constexpr char zeros[kAlignment] = {};
    const std::uint64_t   padding = (kAlignment - m_Offset % kAlignment) % kAlignment;
    m_Out.write(zeros, static_cast<std::streamsize>(padding));
    m_Offset += padding;
    m_Out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!m_Out)
        throw std::runtime_error("cannot write '" + PathToUtf8(m_File) + "'");
    m_Entries.push_back({std::string(path), m_Offset, bytes.size(), Fnv1a64(bytes)});
    m_Offset += bytes.size();
}

void PakWriter::Finish()
{
    if (m_Finished)
        return;
    std::vector<std::byte> index;
    for (const PakEntryInfo& e : m_Entries) {
        Append(index, static_cast<std::uint32_t>(e.path.size()));
        const auto* p = reinterpret_cast<const std::byte*>(e.path.data());
        index.insert(index.end(), p, p + e.path.size());
        Append(index, e.offset);
        Append(index, e.size);
        Append(index, e.hash);
    }
    Header header;
    std::memcpy(header.magic, kMagic, sizeof(kMagic));
    header.version     = kPakVersion;
    header.entryCount  = static_cast<std::uint32_t>(m_Entries.size());
    header.indexOffset = m_Offset;
    header.indexSize   = index.size();
    header.indexHash   = Fnv1a64(index);
    m_Out.write(reinterpret_cast<const char*>(index.data()), static_cast<std::streamsize>(index.size()));
    m_Offset += index.size();
    m_Out.seekp(0);
    m_Out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    m_Out.close();
    if (!m_Out)
        throw std::runtime_error("cannot write '" + PathToUtf8(m_File) + "'");
    m_Finished = true;
}

// --- PakFile -----------------------------------------------------------------------------------------

std::shared_ptr<const PakFile> PakFile::Open(const fs::path& file)
{
    const std::string name = PathToUtf8(file);
    std::ifstream     in(file, std::ios::binary | std::ios::ate);
    if (!in)
        throw std::runtime_error("cannot open pak '" + name + "'");
    const std::uint64_t fileSize = static_cast<std::uint64_t>(in.tellg());
    Header              header;
    in.seekg(0);
    if (fileSize < sizeof(Header) || !in.read(reinterpret_cast<char*>(&header), sizeof(header)) ||
        std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0)
        throw std::runtime_error("'" + name + "' is not a pak");
    if (header.version != kPakVersion)
        throw std::runtime_error(std::format("'{}': pak version {} (expected {})", name, header.version, kPakVersion));
    if (header.indexOffset < sizeof(Header) || header.indexOffset > fileSize || header.indexSize > fileSize - header.indexOffset)
        throw std::runtime_error("'" + name + "': bad pak index range");

    std::vector<std::byte> index(static_cast<std::size_t>(header.indexSize));
    in.seekg(static_cast<std::streamoff>(header.indexOffset));
    if (!in.read(reinterpret_cast<char*>(index.data()), static_cast<std::streamsize>(index.size())))
        throw std::runtime_error("'" + name + "': cannot read the pak index");
    if (Fnv1a64(index) != header.indexHash)
        throw std::runtime_error("'" + name + "': pak index is corrupt");

    auto        pak = std::make_shared<PakFile>();
    pak->m_File     = fs::absolute(file).lexically_normal();
    std::size_t at  = 0;
    for (std::uint32_t i = 0; i < header.entryCount; ++i) {
        const auto length = Take<std::uint32_t>(index, at);
        if (at + length > index.size())
            throw std::runtime_error("'" + name + "': truncated pak index");
        PakEntryInfo e;
        e.path.assign(reinterpret_cast<const char*>(index.data() + at), length);
        at += length;
        e.offset = Take<std::uint64_t>(index, at);
        e.size   = Take<std::uint64_t>(index, at);
        e.hash   = Take<std::uint64_t>(index, at);
        if (e.offset < sizeof(Header) || e.offset > header.indexOffset || e.size > header.indexOffset - e.offset)
            throw std::runtime_error(std::format("'{}': entry '{}' out of range", name, e.path));
        if (!pak->m_Index.emplace(LookupKey(e.path), pak->m_Entries.size()).second)
            throw std::runtime_error(std::format("'{}': duplicate entry '{}'", name, e.path));
        pak->m_Entries.push_back(std::move(e));
    }
    return pak;
}

const PakEntryInfo* PakFile::Find(std::string_view path) const
{
    const auto it = m_Index.find(LookupKey(path));
    return it != m_Index.end() ? &m_Entries[it->second] : nullptr;
}

std::vector<std::byte> PakFile::Read(const PakEntryInfo& entry) const
{
    std::ifstream in(m_File, std::ios::binary);
    std::vector<std::byte> bytes(static_cast<std::size_t>(entry.size));
    if (!in || !in.seekg(static_cast<std::streamoff>(entry.offset)) ||
        !in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error(std::format("cannot read '{}' from '{}'", entry.path, PathToUtf8(m_File)));
    if (Fnv1a64(bytes) != entry.hash)
        throw std::runtime_error(std::format("'{}' in '{}' is corrupt (hash mismatch)", entry.path, PathToUtf8(m_File)));
    return bytes;
}

// --- Vfs ---------------------------------------------------------------------------------------------

namespace Vfs {
namespace {

struct MountPoint {
    std::shared_ptr<const PakFile> pak;
    fs::path                       root; // absolute, lexically normal
};

std::shared_mutex       g_Mutex;
std::vector<MountPoint> g_Mounts;

std::vector<MountPoint> Mounts()
{
    std::shared_lock lock{g_Mutex};
    return g_Mounts;
}

fs::path Normal(const fs::path& path)
{
    std::error_code ec;
    return fs::absolute(path, ec).lexically_normal();
}

// `path` relative to `root` ("" for the root itself), nullopt when outside.
std::optional<std::string> Relative(const fs::path& path, const fs::path& root)
{
    const fs::path relative = Normal(path).lexically_relative(root);
    if (relative.empty())
        return std::nullopt;
    const std::string s = GenericUtf8(relative);
    if (s == ".")
        return std::string();
    if (s == ".." || s.starts_with("../"))
        return std::nullopt;
    return s;
}

struct Found {
    std::shared_ptr<const PakFile> pak;
    const PakEntryInfo*            entry = nullptr;
};

std::optional<Found> FindPacked(const fs::path& path)
{
    const std::vector<MountPoint> mounts = Mounts();
    for (auto it = mounts.rbegin(); it != mounts.rend(); ++it)
        if (const auto relative = Relative(path, it->root); relative && !relative->empty() && !relative->starts_with(kCookedPrefix))
            if (const PakEntryInfo* entry = it->pak->Find(*relative))
                return Found{it->pak, entry};
    return std::nullopt;
}

} // namespace

void Mount(std::shared_ptr<const PakFile> pak, const fs::path& root)
{
    if (!pak)
        return;
    std::unique_lock lock{g_Mutex};
    g_Mounts.push_back({std::move(pak), Normal(root)});
}

void UnmountAll()
{
    std::unique_lock lock{g_Mutex};
    g_Mounts.clear();
}

bool Mounted()
{
    std::shared_lock lock{g_Mutex};
    return !g_Mounts.empty();
}

std::optional<std::vector<std::byte>> Read(const fs::path& path)
{
    if (const auto found = FindPacked(path))
        return found->pak->Read(*found->entry);
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        return std::nullopt;
    const std::streamoff size = file.tellg();
    if (size < 0)
        return std::nullopt;
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        return std::nullopt;
    return bytes;
}

std::vector<std::byte> ReadOrThrow(const fs::path& path)
{
    std::optional<std::vector<std::byte>> bytes = Read(path);
    if (!bytes)
        throw std::runtime_error("cannot read '" + PathToUtf8(path) + "'");
    return std::move(*bytes);
}

std::optional<std::string> ReadText(const fs::path& path)
{
    std::optional<std::vector<std::byte>> bytes = Read(path);
    if (!bytes)
        return std::nullopt;
    return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
}

bool Exists(const fs::path& path)
{
    if (FindPacked(path))
        return true;
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

bool IsPacked(const fs::path& path)
{
    return FindPacked(path).has_value();
}

std::optional<Location> Locate(const fs::path& path)
{
    if (const auto found = FindPacked(path))
        return Location{found->pak->File(), found->entry->offset, found->entry->size, true};
    std::error_code ec;
    if (!fs::is_regular_file(path, ec))
        return std::nullopt;
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec)
        return std::nullopt;
    return Location{Normal(path), 0, size, false};
}

fs::file_time_type ModifiedTime(const fs::path& path)
{
    std::error_code ec;
    if (const auto found = FindPacked(path)) {
        const auto time = fs::last_write_time(found->pak->File(), ec);
        return ec ? fs::file_time_type::min() : time;
    }
    const auto time = fs::last_write_time(path, ec);
    return ec ? fs::file_time_type::min() : time;
}

std::vector<fs::path> ListFiles(const fs::path& directory, bool recursive)
{
    std::vector<fs::path> files;
    std::error_code       ec;
    const fs::path        dir = Normal(directory);
    if (fs::is_directory(dir, ec)) {
        const auto add = [&](const fs::directory_entry& entry) {
            std::error_code e;
            if (entry.is_regular_file(e))
                files.push_back(entry.path().lexically_normal());
        };
        if (recursive)
            for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
                add(*it);
        else
            for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
                add(*it);
    }
    for (const MountPoint& mount : Mounts()) {
        const auto relative = Relative(dir, mount.root);
        if (!relative)
            continue;
        const std::string prefix = relative->empty() ? std::string() : *relative + "/";
        for (const PakEntryInfo& e : mount.pak->Entries()) {
            if (e.path.starts_with(kCookedPrefix) || LookupKey(e.path).rfind(LookupKey(prefix), 0) != 0)
                continue;
            if (!recursive && e.path.find('/', prefix.size()) != std::string::npos)
                continue;
            files.push_back((mount.root / PathFromUtf8(e.path)).lexically_normal());
        }
    }
    std::ranges::sort(files);
    files.erase(std::unique(files.begin(), files.end()), files.end());
    return files;
}

std::optional<std::vector<std::byte>> ReadCooked(std::string_view name)
{
    const std::string             path   = std::string(kCookedPrefix) + std::string(name);
    const std::vector<MountPoint> mounts = Mounts();
    for (auto it = mounts.rbegin(); it != mounts.rend(); ++it)
        if (const PakEntryInfo* entry = it->pak->Find(path))
            return it->pak->Read(*entry);
    return std::nullopt;
}

std::optional<std::vector<std::byte>> ReadCookedFor(const fs::path& file, std::string_view category)
{
    const std::vector<MountPoint> mounts = Mounts();
    for (auto it = mounts.rbegin(); it != mounts.rend(); ++it)
        if (const auto relative = Relative(file, it->root); relative && !relative->empty())
            if (const PakEntryInfo* entry =
                    it->pak->Find(std::string(kCookedPrefix) + std::string(category) + "/" + *relative))
                return it->pak->Read(*entry);
    return std::nullopt;
}

} // namespace Vfs

} // namespace Engine
