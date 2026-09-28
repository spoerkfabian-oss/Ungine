#pragma once
#include <cstdio>
#include <format>
#include <string>
#include <utility>

namespace Engine::Log {

enum class Level { Trace, Info, Warn, Error };

template <class... Args>
void Write(Level level, std::format_string<Args...> fmt, Args&&... args)
{
    static constexpr const char* kTags[] = {"TRACE", "INFO ", "WARN ", "ERROR"};
    const std::string msg = std::format(fmt, std::forward<Args>(args)...);
    std::FILE* out = level >= Level::Warn ? stderr : stdout;
    std::fprintf(out, "[%s] %s\n", kTags[static_cast<int>(level)], msg.c_str());
}

} // namespace Engine::Log

#define ENGINE_TRACE(...) ::Engine::Log::Write(::Engine::Log::Level::Trace, __VA_ARGS__)
#define ENGINE_INFO(...)  ::Engine::Log::Write(::Engine::Log::Level::Info,  __VA_ARGS__)
#define ENGINE_WARN(...)  ::Engine::Log::Write(::Engine::Log::Level::Warn,  __VA_ARGS__)
#define ENGINE_ERROR(...) ::Engine::Log::Write(::Engine::Log::Level::Error, __VA_ARGS__)
