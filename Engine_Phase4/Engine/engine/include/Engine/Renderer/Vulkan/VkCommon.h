#pragma once
// Always include this instead of <vulkan/vulkan.h>: volk defines VK_NO_PROTOTYPES.
#include <volk.h>

#include <cstdlib>
#include "Engine/Core/Log.h"

namespace Engine {
// vk_enum_string_helper.h lives in Vulkan-Utility-Libraries, not Vulkan-Headers; keep it local.
[[nodiscard]] constexpr const char* ToString(VkResult r) noexcept
{
    switch (r) {
    case VK_SUCCESS:                        return "VK_SUCCESS";
    case VK_NOT_READY:                      return "VK_NOT_READY";
    case VK_TIMEOUT:                        return "VK_TIMEOUT";
    case VK_SUBOPTIMAL_KHR:                 return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY:       return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:     return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:    return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST:              return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_LAYER_NOT_PRESENT:        return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:    return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT:      return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:      return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_SURFACE_LOST_KHR:         return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR:          return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    default:                                return "VK_RESULT_UNKNOWN";
    }
}
} // namespace Engine

#define VK_CHECK(expr)                                                              \
    do {                                                                            \
        const VkResult vkCheckResult_ = (expr);                                     \
        if (vkCheckResult_ != VK_SUCCESS) {                                         \
            ENGINE_ERROR("Vulkan call failed: {} [{}] ({}:{})",                          \
                         ::Engine::ToString(vkCheckResult_),      \
                         static_cast<int>(vkCheckResult_), __FILE__, __LINE__);      \
            std::abort();                                                           \
        }                                                                           \
    } while (0)
