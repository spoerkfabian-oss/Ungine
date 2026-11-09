#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Platform.h"

#include <monocypher-ed25519.h>
#include <monocypher.h>
#include <zstd.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstring>
#include <format>
#include <functional>
#include <mutex>
#include <random>
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
constexpr std::uint32_t kHeaderSigned   = 1u << 0; // a 64-byte Ed25519 signature + 32-byte key follow the index
constexpr std::uint64_t kMaxRatio       = 1024;    // decompressed / stored; the writer stores raw beyond it
constexpr std::size_t   kSignatureBytes = 64 + 32;

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

// Entry paths are relative, '/'-separated and never leave their root: no absolute paths, drive
// letters, backslashes, NULs, empty, "." or ".." components (a crafted pak must not reach files
// outside the mount point). Cooked entries are checked after their ":cooked/" prefix.
bool ValidEntryPath(std::string_view path)
{
    if (path.starts_with(kCookedPrefix))
        path.remove_prefix(kCookedPrefix.size());
    if (path.empty() || path.find_first_of(std::string_view("\\:\0", 3)) != std::string_view::npos)
        return false;
    for (std::size_t start = 0;;) {
        const std::size_t      end       = path.find('/', start);
        const std::string_view component = path.substr(start, end == std::string_view::npos ? path.npos : end - start);
        if (component.empty() || component == "." || component == "..")
            return false;
        if (end == std::string_view::npos)
            return true;
        start = end + 1;
    }
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

std::array<std::uint8_t, 32> Blake2b256(std::span<const std::byte> bytes)
{
    std::array<std::uint8_t, 32> digest{};
    crypto_blake2b(digest.data(), digest.size(), reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    return digest;
}

// The secret key Monocypher signs with (seed + public key).
std::array<std::uint8_t, 64> SecretKey(const PakKeyPair& key)
{
    std::array<std::uint8_t, 64> secret{};
    std::array<std::uint8_t, 32> seed = key.seed; // wiped by Monocypher
    PakPublicKey                 publicKey{};
    crypto_ed25519_key_pair(secret.data(), publicKey.data(), seed.data());
    return secret;
}

} // namespace

// --- Keys --------------------------------------------------------------------------------------------

PakKeyPair PakKeyFromSeed(const std::array<std::uint8_t, 32>& seed)
{
    PakKeyPair                   key{.seed = seed, .publicKey = {}};
    std::array<std::uint8_t, 32> copy = seed;
    std::array<std::uint8_t, 64> secret{};
    crypto_ed25519_key_pair(secret.data(), key.publicKey.data(), copy.data());
    crypto_wipe(secret.data(), secret.size());
    return key;
}

PakKeyPair GeneratePakKey()
{
    std::random_device           random; // the OS entropy source (getrandom / BCryptGenRandom)
    std::array<std::uint8_t, 32> seed{};
    for (std::size_t i = 0; i < seed.size(); i += 4) {
        const std::uint32_t r = random();
        std::memcpy(seed.data() + i, &r, 4);
    }
    PakKeyPair key = PakKeyFromSeed(seed);
    crypto_wipe(seed.data(), seed.size());
    return key;
}

std::string HexString(std::span<const std::uint8_t> bytes)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string           out;
    out.reserve(bytes.size() * 2);
    for (const std::uint8_t b : bytes) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 15]);
    }
    return out;
}

std::optional<PakPublicKey> ParsePakKeyHex(std::string_view hex)
{
    while (!hex.empty() && std::isspace(static_cast<unsigned char>(hex.back())))
        hex.remove_suffix(1);
    while (!hex.empty() && std::isspace(static_cast<unsigned char>(hex.front())))
        hex.remove_prefix(1);
    if (hex.size() != 64)
        return std::nullopt;
    const auto nibble = [](char ch) -> int {
        if (ch >= '0' && ch <= '9')
            return ch - '0';
        if (ch >= 'a' && ch <= 'f')
            return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F')
            return ch - 'A' + 10;
        return -1;
    };
    PakPublicKey key{};
    for (std::size_t i = 0; i < key.size(); ++i) {
        const int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return std::nullopt;
        key[i] = static_cast<std::uint8_t>(hi * 16 + lo);
    }
    return key;
}

// --- PakWriter ---------------------------------------------------------------------------------------

PakWriter::PakWriter(const fs::path& file, int compressionLevel) : m_File(file), m_Level(compressionLevel)
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

void PakWriter::Add(std::string_view path, std::span<const std::byte> bytes, PakCompression compression)
{
    PakEntryInfo entry;
    entry.path = std::string(path);
    entry.size = bytes.size();
    entry.hash = Fnv1a64(bytes);
    if (compression == PakCompression::Auto && m_Level > 0 && bytes.size() >= 64) {
        std::vector<std::byte> packed(ZSTD_compressBound(bytes.size()));
        const std::size_t      n = ZSTD_compress(packed.data(), packed.size(), bytes.data(), bytes.size(), m_Level);
        // Worth it (>= 10 % smaller) and within the ratio the reader accepts.
        if (!ZSTD_isError(n) && n * 10 <= bytes.size() * 9 && bytes.size() <= n * kMaxRatio) {
            packed.resize(n);
            entry.flags = kPakEntryCompressed;
            AddEntry(std::move(entry), packed);
            return;
        }
    }
    AddEntry(std::move(entry), bytes);
}

void PakWriter::AddDeleted(std::string_view path)
{
    PakEntryInfo entry;
    entry.path  = std::string(path);
    entry.flags = kPakEntryDeleted;
    AddEntry(std::move(entry), {});
}

void PakWriter::AddEntry(PakEntryInfo entry, std::span<const std::byte> stored)
{
    if (m_Finished)
        throw std::logic_error("PakWriter: already finished");
    if (!ValidEntryPath(entry.path))
        throw std::invalid_argument(std::format("PakWriter: invalid entry path '{}'", entry.path));
    const std::string key = LookupKey(entry.path);
    for (const PakEntryInfo& e : m_Entries)
        if (LookupKey(e.path) == key)
            throw std::invalid_argument(std::format("PakWriter: duplicate entry '{}'", entry.path));
    static constexpr char zeros[kAlignment] = {};
    const std::uint64_t   padding = (kAlignment - m_Offset % kAlignment) % kAlignment;
    m_Out.write(zeros, static_cast<std::streamsize>(padding));
    m_Offset += padding;
    m_Out.write(reinterpret_cast<const char*>(stored.data()), static_cast<std::streamsize>(stored.size()));
    if (!m_Out)
        throw std::runtime_error("cannot write '" + PathToUtf8(m_File) + "'");
    entry.offset     = m_Offset;
    entry.storedSize = stored.size();
    entry.digest     = Blake2b256(stored);
    m_Entries.push_back(std::move(entry));
    m_Offset += stored.size();
}

void PakWriter::Finish(const PakKeyPair* key)
{
    if (m_Finished)
        return;
    std::vector<std::byte> index;
    for (const PakEntryInfo& e : m_Entries) {
        Append(index, static_cast<std::uint32_t>(e.path.size()));
        const auto* p = reinterpret_cast<const std::byte*>(e.path.data());
        index.insert(index.end(), p, p + e.path.size());
        Append(index, e.offset);
        Append(index, e.storedSize);
        Append(index, e.size);
        Append(index, e.hash);
        Append(index, e.flags);
        Append(index, e.digest);
    }
    Header header;
    std::memcpy(header.magic, kMagic, sizeof(kMagic));
    header.version     = kPakVersion;
    header.entryCount  = static_cast<std::uint32_t>(m_Entries.size());
    header.flags       = key ? kHeaderSigned : 0;
    header.indexOffset = m_Offset;
    header.indexSize   = index.size();
    header.indexHash   = Fnv1a64(index);
    m_Out.write(reinterpret_cast<const char*>(index.data()), static_cast<std::streamsize>(index.size()));
    m_Offset += index.size();
    if (key) { // the index holds every entry's digest: signing it covers the content
        std::array<std::uint8_t, 64> secret = SecretKey(*key);
        std::array<std::uint8_t, 64> signature{};
        crypto_ed25519_sign(signature.data(), secret.data(), reinterpret_cast<const std::uint8_t*>(index.data()), index.size());
        crypto_wipe(secret.data(), secret.size());
        m_Out.write(reinterpret_cast<const char*>(signature.data()), static_cast<std::streamsize>(signature.size()));
        m_Out.write(reinterpret_cast<const char*>(key->publicKey.data()), static_cast<std::streamsize>(key->publicKey.size()));
        m_Offset += kSignatureBytes;
    }
    m_Out.seekp(0);
    m_Out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    m_Out.close();
    if (!m_Out)
        throw std::runtime_error("cannot write '" + PathToUtf8(m_File) + "'");
    m_Finished = true;
}

// --- PakFile -----------------------------------------------------------------------------------------

std::shared_ptr<const PakFile> PakFile::Open(const fs::path& file, const PakPublicKey* requiredKey)
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
    if (header.version != 1 && header.version != kPakVersion)
        throw std::runtime_error(std::format("'{}': pak version {} (expected 1 or {})", name, header.version, kPakVersion));
    const bool          signedPak = header.version >= 2 && (header.flags & kHeaderSigned) != 0;
    const std::uint64_t trailer   = signedPak ? kSignatureBytes : 0;
    if (header.indexOffset < sizeof(Header) || header.indexOffset > fileSize || header.indexSize > fileSize - header.indexOffset ||
        trailer > fileSize - header.indexOffset - header.indexSize)
        throw std::runtime_error("'" + name + "': bad pak index range");

    std::vector<std::byte> index(static_cast<std::size_t>(header.indexSize));
    in.seekg(static_cast<std::streamoff>(header.indexOffset));
    if (!in.read(reinterpret_cast<char*>(index.data()), static_cast<std::streamsize>(index.size())))
        throw std::runtime_error("'" + name + "': cannot read the pak index");
    if (Fnv1a64(index) != header.indexHash)
        throw std::runtime_error("'" + name + "': pak index is corrupt");

    auto        pak = std::make_shared<PakFile>();
    pak->m_File     = fs::absolute(file).lexically_normal();
    pak->m_Version  = header.version;
    if (signedPak) {
        std::array<std::uint8_t, 64> signature{};
        PakPublicKey                 signer{};
        if (!in.read(reinterpret_cast<char*>(signature.data()), static_cast<std::streamsize>(signature.size())) ||
            !in.read(reinterpret_cast<char*>(signer.data()), static_cast<std::streamsize>(signer.size())))
            throw std::runtime_error("'" + name + "': cannot read the pak signature");
        if (crypto_ed25519_check(signature.data(), signer.data(), reinterpret_cast<const std::uint8_t*>(index.data()), index.size()) != 0)
            throw std::runtime_error("'" + name + "': the pak signature is invalid");
        pak->m_SignedBy = signer;
    }
    if (requiredKey && pak->m_SignedBy != *requiredKey)
        throw std::runtime_error("'" + name + "': " + (pak->m_SignedBy ? "signed by another key" : "not signed") +
                                 " (the project requires its signing key)");
    std::size_t at  = 0;
    for (std::uint32_t i = 0; i < header.entryCount; ++i) {
        const auto length = Take<std::uint32_t>(index, at);
        if (at + length > index.size())
            throw std::runtime_error("'" + name + "': truncated pak index");
        PakEntryInfo e;
        e.path.assign(reinterpret_cast<const char*>(index.data() + at), length);
        at += length;
        e.offset = Take<std::uint64_t>(index, at);
        if (header.version == 1) {
            e.size       = Take<std::uint64_t>(index, at);
            e.hash       = Take<std::uint64_t>(index, at);
            e.storedSize = e.size;
        } else {
            e.storedSize = Take<std::uint64_t>(index, at);
            e.size       = Take<std::uint64_t>(index, at);
            e.hash       = Take<std::uint64_t>(index, at);
            e.flags      = Take<std::uint32_t>(index, at);
            e.digest     = Take<std::array<std::uint8_t, 32>>(index, at);
        }
        if (!ValidEntryPath(e.path))
            throw std::runtime_error(std::format("'{}': invalid entry path '{}'", name, e.path));
        if ((e.flags & ~(kPakEntryCompressed | kPakEntryDeleted)) != 0 || (e.Deleted() && (e.Compressed() || e.size != 0 || e.storedSize != 0)))
            throw std::runtime_error(std::format("'{}': entry '{}' has invalid flags", name, e.path));
        if (!e.Deleted() &&
            (e.offset < sizeof(Header) || e.offset > header.indexOffset || e.storedSize > header.indexOffset - e.offset))
            throw std::runtime_error(std::format("'{}': entry '{}' out of range", name, e.path));
        if (e.Compressed() ? e.size / kMaxRatio > e.storedSize : e.size != e.storedSize)
            throw std::runtime_error(std::format("'{}': entry '{}' has an implausible size", name, e.path));
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
    if (entry.Deleted())
        throw std::runtime_error(std::format("'{}' in '{}' is a deleted entry", entry.path, PathToUtf8(m_File)));
    std::ifstream in(m_File, std::ios::binary);
    std::vector<std::byte> stored(static_cast<std::size_t>(entry.storedSize));
    if (!in || !in.seekg(static_cast<std::streamoff>(entry.offset)) ||
        !in.read(reinterpret_cast<char*>(stored.data()), static_cast<std::streamsize>(stored.size())))
        throw std::runtime_error(std::format("cannot read '{}' from '{}'", entry.path, PathToUtf8(m_File)));
    const auto corrupt = [&] {
        return std::runtime_error(std::format("'{}' in '{}' is corrupt (hash mismatch)", entry.path, PathToUtf8(m_File)));
    };
    if (m_Version >= 2 && Blake2b256(stored) != entry.digest) // before decompressing anything
        throw corrupt();
    if (!entry.Compressed()) {
        if (Fnv1a64(stored) != entry.hash)
            throw corrupt();
        return stored;
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(entry.size));
    const std::size_t      n = ZSTD_decompress(bytes.data(), bytes.size(), stored.data(), stored.size());
    if (ZSTD_isError(n) || n != bytes.size() || Fnv1a64(bytes) != entry.hash)
        throw corrupt();
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
    const PakEntryInfo*            entry = nullptr; // Deleted(): removed by a patch, hides older mounts and the disk
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

// The newest mounted entry `name` (a pak entry path, cooked data included) below a mount root.
std::optional<std::vector<std::byte>> ReadNewest(const std::vector<MountPoint>& mounts,
                                                 const std::function<std::optional<std::string>(const MountPoint&)>& name)
{
    for (auto it = mounts.rbegin(); it != mounts.rend(); ++it)
        if (const std::optional<std::string> path = name(*it))
            if (const PakEntryInfo* entry = it->pak->Find(*path))
                return entry->Deleted() ? std::nullopt : std::optional(it->pak->Read(*entry));
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
        return found->entry->Deleted() ? std::nullopt : std::optional(found->pak->Read(*found->entry));
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
    if (const auto found = FindPacked(path))
        return !found->entry->Deleted();
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

bool IsPacked(const fs::path& path)
{
    const auto found = FindPacked(path);
    return found && !found->entry->Deleted();
}

std::optional<Location> Locate(const fs::path& path)
{
    if (const auto found = FindPacked(path)) {
        if (found->entry->Deleted())
            return std::nullopt;
        return Location{found->pak->File(), found->entry->offset, found->entry->size, true, found->entry->Compressed()};
    }
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
        if (found->entry->Deleted())
            return fs::file_time_type::min();
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
    // Newest mount first: a deleted entry (patch) hides the same file in older mounts.
    std::vector<std::string>      hidden; // LookupKey of absolute generic paths
    const std::vector<MountPoint> mounts = Mounts();
    for (auto mount = mounts.rbegin(); mount != mounts.rend(); ++mount) {
        const auto relative = Relative(dir, mount->root);
        if (!relative)
            continue;
        const std::string prefix = relative->empty() ? std::string() : *relative + "/";
        for (const PakEntryInfo& e : mount->pak->Entries()) {
            if (e.path.starts_with(kCookedPrefix) || LookupKey(e.path).rfind(LookupKey(prefix), 0) != 0)
                continue;
            if (!recursive && e.path.find('/', prefix.size()) != std::string::npos)
                continue;
            const fs::path file = (mount->root / PathFromUtf8(e.path)).lexically_normal();
            if (!Relative(file, mount->root)) // entry paths are validated on open; never list outside the root
                continue;
            const std::string key = LookupKey(GenericUtf8(file));
            if (std::ranges::find(hidden, key) != hidden.end())
                continue;
            hidden.push_back(key); // the newest mount decides
            if (!e.Deleted())
                files.push_back(file);
            else
                std::erase(files, file); // a disk file of the same name stays deleted too
        }
    }
    std::ranges::sort(files);
    files.erase(std::unique(files.begin(), files.end()), files.end());
    return files;
}

std::optional<std::vector<std::byte>> ReadCooked(std::string_view name)
{
    const std::string path = std::string(kCookedPrefix) + std::string(name);
    return ReadNewest(Mounts(), [&](const MountPoint&) { return std::optional(path); });
}

std::optional<std::vector<std::byte>> ReadCookedFor(const fs::path& file, std::string_view category)
{
    return ReadNewest(Mounts(), [&](const MountPoint& mount) -> std::optional<std::string> {
        const auto relative = Relative(file, mount.root);
        if (!relative || relative->empty())
            return std::nullopt;
        return std::string(kCookedPrefix) + std::string(category) + "/" + *relative;
    });
}

} // namespace Vfs

} // namespace Engine
