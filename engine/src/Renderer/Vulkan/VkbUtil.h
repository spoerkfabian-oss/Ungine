#pragma once
// Private helper: unwrap vk-bootstrap results or throw with a readable message.
#include <VkBootstrap.h>

#include <format>
#include <stdexcept>
#include <string_view>

namespace Engine {

template <class T>
T Expect(vkb::Result<T> result, std::string_view what)
{
    if (!result)
        throw std::runtime_error(std::format("{}: {}", what, result.error().message()));
    return result.value();
}

} // namespace Engine
