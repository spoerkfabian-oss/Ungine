#include "Engine/Script/ScriptRegistry.h"
#include "Engine/Script/ScriptGraph.h"
#include "ScriptJson.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <functional>
#include <map>
#include <stdexcept>
#include <unordered_set>

namespace Engine::ScriptRegistry {

using json = nlohmann::json;

namespace {
struct State {
    std::map<std::string, ScriptEnum, std::less<>>      enums;
    std::map<std::string, ScriptStructDef, std::less<>> structs;
    std::map<std::string, ScriptInterface, std::less<>> interfaces;
    std::map<std::string, ScriptLibrary, std::less<>>   libraries;
    std::uint64_t                                       revision = 1;
};

std::pair<std::string_view, std::string_view> SplitQualified(std::string_view q)
{
    const std::size_t dot = q.find('.');
    return dot == std::string_view::npos ? std::pair{q, std::string_view()} : std::pair{q.substr(0, dot), q.substr(dot + 1)};
}

json ParamsJson(const std::vector<ScriptParam>& params)
{
    json list = json::array();
    for (const ScriptParam& p : params)
        list.push_back({{"name", p.name}, {"type", ToString(p.type)}});
    return list;
}

std::vector<ScriptParam> ParamsFrom(const json& j)
{
    std::vector<ScriptParam> params;
    for (const json& p : j) {
        ScriptParam param{p.at("name").get<std::string>(),
                          PinTypeFromString(p.value("type", std::string("float"))).value_or(PinType::Float)};
        if (param.type.kind == PinKind::Exec)
            param.type = PinType::Float;
        params.push_back(std::move(param));
    }
    return params;
}
State& S()
{
    static State state;
    return state;
}

std::string ToUtf8(const std::filesystem::path& p)
{
    const std::u8string s = p.generic_u8string();
    return {s.begin(), s.end()};
}

json ReadJson(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot read '" + ToUtf8(file) + "'");
    try {
        return json::parse(in);
    } catch (const json::exception& e) {
        throw std::runtime_error("'" + ToUtf8(file) + "': " + e.what());
    }
}

void WriteJson(const std::filesystem::path& file, const json& j)
{
    std::filesystem::path temp = file;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out)
            throw std::runtime_error("cannot write '" + ToUtf8(temp) + "'");
        out << j.dump(2) << '\n';
        if (!out)
            throw std::runtime_error("write failed: '" + ToUtf8(temp) + "'");
    }
    std::error_code ec;
    std::filesystem::rename(temp, file, ec);
    if (ec)
        throw std::runtime_error("cannot replace '" + ToUtf8(file) + "': " + ec.message());
}
} // namespace

void Clear()
{
    S().enums.clear();
    S().structs.clear();
    S().interfaces.clear();
    S().libraries.clear();
    ++S().revision;
}

void AddInterface(ScriptInterface def)
{
    const std::string name = def.name;
    S().interfaces[name]   = std::move(def);
    ++S().revision;
}

void AddLibrary(ScriptLibrary def)
{
    const std::string name = def.name;
    S().libraries[name]    = std::move(def);
    ++S().revision;
}

void RemoveInterface(std::string_view name)
{
    if (const auto it = S().interfaces.find(name); it != S().interfaces.end()) {
        S().interfaces.erase(it);
        ++S().revision;
    }
}

void RemoveLibrary(std::string_view name)
{
    if (const auto it = S().libraries.find(name); it != S().libraries.end()) {
        S().libraries.erase(it);
        ++S().revision;
    }
}

const ScriptInterface* FindInterface(std::string_view name)
{
    const auto it = S().interfaces.find(name);
    return it != S().interfaces.end() ? &it->second : nullptr;
}

const ScriptLibrary* FindLibrary(std::string_view name)
{
    const auto it = S().libraries.find(name);
    return it != S().libraries.end() ? &it->second : nullptr;
}

std::vector<std::string> InterfaceNames()
{
    std::vector<std::string> names;
    for (const auto& [name, def] : S().interfaces)
        names.push_back(name);
    return names;
}

std::vector<std::string> LibraryNames()
{
    std::vector<std::string> names;
    for (const auto& [name, def] : S().libraries)
        names.push_back(name);
    return names;
}

const ScriptFunction* FindLibraryFunction(std::string_view qualified)
{
    const auto [lib, fn] = SplitQualified(qualified);
    const ScriptLibrary* l = FindLibrary(lib);
    return l ? l->graph.FindFunction(fn) : nullptr;
}

const ScriptMacro* FindLibraryMacro(std::string_view qualified)
{
    const auto [lib, name] = SplitQualified(qualified);
    const ScriptLibrary* l = FindLibrary(lib);
    return l ? l->graph.FindMacro(name) : nullptr;
}

const ScriptInterfaceFunction* FindInterfaceFunction(std::string_view qualified)
{
    const auto [iface, fn] = SplitQualified(qualified);
    if (const ScriptInterface* i = FindInterface(iface))
        for (const ScriptInterfaceFunction& f : i->functions)
            if (f.name == fn)
                return &f;
    return nullptr;
}

ScriptInterface LoadInterfaceFile(const std::filesystem::path& file)
{
    const json      j = ReadJson(file);
    ScriptInterface i;
    try {
        i.name = j.at("interface").get<std::string>();
        for (const json& f : j.at("functions"))
            i.functions.push_back({f.at("name").get<std::string>(), ParamsFrom(f.value("inputs", json::array())),
                                   ParamsFrom(f.value("outputs", json::array()))});
    } catch (const json::exception& ex) {
        throw std::runtime_error("'" + ToUtf8(file) + "': " + ex.what());
    }
    if (!IsValidScriptName(i.name))
        throw std::runtime_error("'" + ToUtf8(file) + "': invalid interface name '" + i.name + "'");
    i.file = file;
    return i;
}

void SaveInterfaceFile(const std::filesystem::path& file, const ScriptInterface& def)
{
    json functions = json::array();
    for (const ScriptInterfaceFunction& f : def.functions)
        functions.push_back({{"name", f.name}, {"inputs", ParamsJson(f.inputs)}, {"outputs", ParamsJson(f.outputs)}});
    WriteJson(file, {{"version", 1}, {"interface", def.name}, {"functions", std::move(functions)}});
}

void AddEnum(ScriptEnum def)
{
    const std::string name = def.name;
    S().enums[name]        = std::move(def);
    ++S().revision;
}

void AddStruct(ScriptStructDef def)
{
    const std::string name = def.name;
    S().structs[name]      = std::move(def);
    ++S().revision;
}

void RemoveEnum(std::string_view name)
{
    if (const auto it = S().enums.find(name); it != S().enums.end()) {
        S().enums.erase(it);
        ++S().revision;
    }
}

void RemoveStruct(std::string_view name)
{
    if (const auto it = S().structs.find(name); it != S().structs.end()) {
        S().structs.erase(it);
        ++S().revision;
    }
}

const ScriptEnum* FindEnum(std::string_view name)
{
    const auto it = S().enums.find(name);
    return it != S().enums.end() ? &it->second : nullptr;
}

const ScriptStructDef* FindStruct(std::string_view name)
{
    const auto it = S().structs.find(name);
    return it != S().structs.end() ? &it->second : nullptr;
}

std::vector<std::string> EnumNames()
{
    std::vector<std::string> names;
    for (const auto& [name, def] : S().enums)
        names.push_back(name);
    return names;
}

std::vector<std::string> StructNames()
{
    std::vector<std::string> names;
    for (const auto& [name, def] : S().structs)
        names.push_back(name);
    return names;
}

std::uint64_t Revision() { return S().revision; }

std::string EnumValueName(std::string_view enumName, std::int32_t value)
{
    const ScriptEnum* e = FindEnum(enumName);
    return e && value >= 0 && static_cast<std::size_t>(value) < e->values.size() ? e->values[static_cast<std::size_t>(value)]
                                                                                  : std::string();
}

std::int32_t EnumValueIndex(std::string_view enumName, std::string_view valueName)
{
    if (const ScriptEnum* e = FindEnum(enumName))
        for (std::size_t i = 0; i < e->values.size(); ++i)
            if (e->values[i] == valueName)
                return static_cast<std::int32_t>(i);
    return -1;
}

ScriptEnum LoadEnumFile(const std::filesystem::path& file)
{
    const json j = ReadJson(file);
    ScriptEnum e;
    try {
        e.name = j.at("enum").get<std::string>();
        for (const json& v : j.at("values"))
            e.values.push_back(v.get<std::string>());
    } catch (const json::exception& ex) {
        throw std::runtime_error("'" + ToUtf8(file) + "': " + ex.what());
    }
    if (!IsValidScriptName(e.name))
        throw std::runtime_error("'" + ToUtf8(file) + "': invalid enum name '" + e.name + "'");
    e.file = file;
    return e;
}

ScriptStructDef LoadStructFile(const std::filesystem::path& file)
{
    const json      j = ReadJson(file);
    ScriptStructDef s;
    try {
        s.name = j.at("struct").get<std::string>();
        for (const json& f : j.at("fields")) {
            ScriptStructField field;
            field.name = f.at("name").get<std::string>();
            field.type = PinTypeFromString(f.value("type", std::string("float"))).value_or(PinType::Float);
            if (field.type.kind == PinKind::Exec)
                field.type = PinType::Float;
            // Field defaults of nested structs need their definition: read once all are known (Validate).
            field.value = f.contains("value") ? ScriptValueFromJson(f["value"], field.type) : DefaultValue(field.type);
            s.fields.push_back(std::move(field));
        }
    } catch (const json::exception& ex) {
        throw std::runtime_error("'" + ToUtf8(file) + "': " + ex.what());
    }
    if (!IsValidScriptName(s.name))
        throw std::runtime_error("'" + ToUtf8(file) + "': invalid struct name '" + s.name + "'");
    s.file = file;
    return s;
}

void SaveEnumFile(const std::filesystem::path& file, const ScriptEnum& def)
{
    WriteJson(file, {{"version", 1}, {"enum", def.name}, {"values", def.values}});
}

void SaveStructFile(const std::filesystem::path& file, const ScriptStructDef& def)
{
    json fields = json::array();
    for (const ScriptStructField& f : def.fields)
        fields.push_back({{"name", f.name}, {"type", ToString(f.type)}, {"value", ScriptValueToJson(f.value)}});
    WriteJson(file, {{"version", 1}, {"struct", def.name}, {"fields", std::move(fields)}});
}

std::vector<std::string> LoadDirectory(const std::filesystem::path& root)
{
    std::vector<std::string>             problems;
    std::vector<std::filesystem::path>   structFiles;
    std::error_code                      ec;
    for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        const std::filesystem::path& path = it->path();
        const std::string            ext  = path.extension().string();
        try {
            if (ext == ".uenum") {
                ScriptEnum e = LoadEnumFile(path);
                if (FindEnum(e.name) || FindStruct(e.name))
                    problems.push_back("'" + ToUtf8(path) + "': the type name '" + e.name + "' is used twice");
                else
                    AddEnum(std::move(e));
            } else if (ext == ".ustruct") {
                structFiles.push_back(path);
            } else if (ext == ".uinterface") {
                ScriptInterface i = LoadInterfaceFile(path);
                if (FindInterface(i.name))
                    problems.push_back("'" + ToUtf8(path) + "': the interface name '" + i.name + "' is used twice");
                else
                    AddInterface(std::move(i));
            } else if (ext == ".ugraph") { // only libraries (cheap check before parsing the whole graph)
                std::ifstream in(path, std::ios::binary);
                const std::string text{std::istreambuf_iterator<char>(in), {}};
                if (text.find("\"library\"") != std::string::npos) {
                    ScriptGraph graph = ScriptGraphFromJson(text);
                    if (graph.library) {
                        const std::string name = path.stem().string();
                        if (!IsValidScriptName(name))
                            problems.push_back("'" + ToUtf8(path) + "': invalid library name '" + name + "'");
                        else if (FindLibrary(name))
                            problems.push_back("'" + ToUtf8(path) + "': the library name '" + name + "' is used twice");
                        else
                            AddLibrary({name, std::move(graph), path});
                    }
                }
            }
        } catch (const std::exception& e) {
            problems.emplace_back(e.what());
        }
    }
    // Structs after the enums; twice, so field defaults of nested structs see their definitions.
    for (int pass = 0; pass < 2; ++pass)
        for (const std::filesystem::path& path : structFiles) {
            try {
                ScriptStructDef s = LoadStructFile(path);
                if (pass == 0 && (FindEnum(s.name) || FindStruct(s.name)))
                    problems.push_back("'" + ToUtf8(path) + "': the type name '" + s.name + "' is used twice");
                else if (pass == 0 || (FindStruct(s.name) && FindStruct(s.name)->file == path))
                    AddStruct(std::move(s));
            } catch (const std::exception& e) {
                if (pass == 0)
                    problems.emplace_back(e.what());
            }
        }
    return problems;
}

std::vector<std::string> Validate()
{
    std::vector<std::string> problems;
    const auto known = [&](PinType t) {
        const auto check = [](PinKind kind, std::uint16_t name) {
            if (kind == PinKind::Enum)
                return FindEnum(TypeNameOf(name)) != nullptr;
            if (kind == PinKind::Struct)
                return FindStruct(TypeNameOf(name)) != nullptr;
            return true;
        };
        return check(t.kind, t.name) && (!IsMap(t) || check(t.keyKind, t.keyName));
    };
    for (const auto& [name, e] : S().enums) {
        std::unordered_set<std::string> values;
        for (const std::string& v : e.values)
            if (!IsValidScriptName(v) || !values.insert(v).second)
                problems.push_back("Enum '" + name + "': value names must be unique names ('" + v + "')");
        if (e.values.empty())
            problems.push_back("Enum '" + name + "' has no values");
    }
    for (const auto& [name, s] : S().structs) {
        std::unordered_set<std::string> fields;
        for (const ScriptStructField& f : s.fields) {
            if (!IsValidScriptName(f.name) || !fields.insert(f.name).second)
                problems.push_back("Struct '" + name + "': field names must be unique names ('" + f.name + "')");
            if (!known(f.type))
                problems.push_back("Struct '" + name + "', field '" + f.name + "': unknown type " + DisplayName(f.type));
        }
    }
    // A struct holding itself (directly or through other structs; arrays / maps of it are fine).
    std::map<std::string, int> state; // 1 visiting, 2 done
    std::function<bool(const std::string&)> cyclic = [&](const std::string& name) {
        int& s = state[name];
        if (s == 1)
            return true;
        if (s == 2)
            return false;
        s = 1;
        if (const ScriptStructDef* def = FindStruct(name))
            for (const ScriptStructField& f : def->fields)
                if (f.type.kind == PinKind::Struct && !IsContainer(f.type) && cyclic(TypeNameOf(f.type.name)))
                    return true;
        state[name] = 2;
        return false;
    };
    for (const auto& [name, s] : S().structs)
        if (state[name] == 0 && cyclic(name))
            problems.push_back("Struct '" + name + "' contains itself");
    return problems;
}

} // namespace Engine::ScriptRegistry
