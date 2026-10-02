#pragma once
#include "Engine/Script/ScriptGraph.h"
#include "Engine/Script/ScriptValue.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

// Definitions shared by every blueprint of a project, from files anywhere below the content
// directory: enums (.uenum), structs (.ustruct), interfaces (.uinterface) and libraries (.ugraph
// files with "library": true - their functions and macros are usable from any blueprint as
// "<Library>.<Name>", the library name being the file name without extension). Names are unique per
// project (letters, digits, '_', ' '). The registry is process-wide (node pins and default values
// need it); main thread.
//
//   .uenum      {"version": 1, "enum": "Color", "values": ["Red", "Green"]}
//   .ustruct    {"version": 1, "struct": "Item", "fields": [{"name": "count", "type": "int", "value": 1}]}
//   .uinterface {"version": 1, "interface": "Damageable",
//                "functions": [{"name": "TakeDamage", "inputs": [{"name": "Amount", "type": "float"}], "outputs": []}]}

struct ScriptEnum {
    std::string              name;
    std::vector<std::string> values;
    std::filesystem::path    file; // empty: registered in code
};

struct ScriptStructField {
    std::string name;
    PinType     type  = PinType::Float;
    ScriptValue value = 0.0f; // default
};

struct ScriptStructDef {
    std::string                    name;
    std::vector<ScriptStructField> fields;
    std::filesystem::path          file;
};

struct ScriptInterfaceFunction {
    std::string              name;
    std::vector<ScriptParam> inputs;
    std::vector<ScriptParam> outputs;
};

struct ScriptInterface {
    std::string                          name;
    std::vector<ScriptInterfaceFunction> functions;
    std::filesystem::path                file;
};

struct ScriptLibrary {
    std::string           name;
    ScriptGraph           graph;
    std::filesystem::path file;
};

namespace ScriptRegistry {

void Clear();
// Adds or replaces (by name).
void AddEnum(ScriptEnum def);
void AddStruct(ScriptStructDef def);
void RemoveEnum(std::string_view name);
void RemoveStruct(std::string_view name);
void AddInterface(ScriptInterface def);
void AddLibrary(ScriptLibrary def);
void RemoveInterface(std::string_view name);
void RemoveLibrary(std::string_view name);
[[nodiscard]] const ScriptInterface* FindInterface(std::string_view name);
[[nodiscard]] const ScriptLibrary*   FindLibrary(std::string_view name);
[[nodiscard]] std::vector<std::string> InterfaceNames(); // sorted
[[nodiscard]] std::vector<std::string> LibraryNames();   // sorted
// "<Library>.<Function>" / "<Library>.<Macro>" / "<Interface>.<Function>" lookups (null if unknown).
[[nodiscard]] const ScriptFunction*          FindLibraryFunction(std::string_view qualified);
[[nodiscard]] const ScriptMacro*             FindLibraryMacro(std::string_view qualified);
[[nodiscard]] const ScriptInterfaceFunction* FindInterfaceFunction(std::string_view qualified);
// Validation of a library graph (ValidateScriptGraph, computed when first asked after a change);
// null for unknown libraries.
[[nodiscard]] const std::vector<ScriptDiagnostic>* LibraryDiagnostics(std::string_view name);
[[nodiscard]] const ScriptEnum*      FindEnum(std::string_view name);
[[nodiscard]] const ScriptStructDef* FindStruct(std::string_view name);
[[nodiscard]] std::vector<std::string> EnumNames();   // sorted
[[nodiscard]] std::vector<std::string> StructNames(); // sorted
// Changes whenever a definition is added, replaced or removed (caches, the editor).
[[nodiscard]] std::uint64_t Revision();

// Name of an enum value ("" if out of range), index of a name (-1 if unknown).
[[nodiscard]] std::string  EnumValueName(std::string_view enumName, std::int32_t value);
[[nodiscard]] std::int32_t EnumValueIndex(std::string_view enumName, std::string_view valueName);

// Files. Load* throw std::runtime_error on format errors; LoadDirectory registers every type file
// below `root` (Clear first to replace) and returns the problems it skipped.
[[nodiscard]] ScriptEnum      LoadEnumFile(const std::filesystem::path& file);
[[nodiscard]] ScriptStructDef LoadStructFile(const std::filesystem::path& file);
[[nodiscard]] ScriptInterface LoadInterfaceFile(const std::filesystem::path& file);
void SaveInterfaceFile(const std::filesystem::path& file, const ScriptInterface& def);
void SaveEnumFile(const std::filesystem::path& file, const ScriptEnum& def);
void SaveStructFile(const std::filesystem::path& file, const ScriptStructDef& def);
std::vector<std::string> LoadDirectory(const std::filesystem::path& root);

// Problems of the definitions: unknown types in struct fields / interface parameters, structs
// containing themselves, duplicate / invalid names, errors in libraries.
[[nodiscard]] std::vector<std::string> Validate();

} // namespace ScriptRegistry
} // namespace Engine
