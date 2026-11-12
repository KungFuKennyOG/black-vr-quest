/* xr_result_names.h - XrResult / VkResult codes as their names, for the shell's failure lines and the preflight.
 * Generated from the OpenXR-SDK and Vulkan-Headers enums (core codes, |value| < 1000; extension codes print "?"). */
#ifndef XR_RESULT_NAMES_H
#define XR_RESULT_NAMES_H
static inline const char *xr_result_name(int r)
{
    switch (r) {
    case 9: return "XR_FRAME_DISCARDED";
    case 8: return "XR_SESSION_NOT_FOCUSED";
    case 7: return "XR_SPACE_BOUNDS_UNAVAILABLE";
    case 4: return "XR_EVENT_UNAVAILABLE";
    case 3: return "XR_SESSION_LOSS_PENDING";
    case 1: return "XR_TIMEOUT_EXPIRED";
    case 0: return "XR_SUCCESS";
    case -1: return "XR_ERROR_VALIDATION_FAILURE";
    case -2: return "XR_ERROR_RUNTIME_FAILURE";
    case -3: return "XR_ERROR_OUT_OF_MEMORY";
    case -4: return "XR_ERROR_API_VERSION_UNSUPPORTED";
    case -6: return "XR_ERROR_INITIALIZATION_FAILED";
    case -7: return "XR_ERROR_FUNCTION_UNSUPPORTED";
    case -8: return "XR_ERROR_FEATURE_UNSUPPORTED";
    case -9: return "XR_ERROR_EXTENSION_NOT_PRESENT";
    case -10: return "XR_ERROR_LIMIT_REACHED";
    case -11: return "XR_ERROR_SIZE_INSUFFICIENT";
    case -12: return "XR_ERROR_HANDLE_INVALID";
    case -13: return "XR_ERROR_INSTANCE_LOST";
    case -14: return "XR_ERROR_SESSION_RUNNING";
    case -16: return "XR_ERROR_SESSION_NOT_RUNNING";
    case -17: return "XR_ERROR_SESSION_LOST";
    case -18: return "XR_ERROR_SYSTEM_INVALID";
    case -19: return "XR_ERROR_PATH_INVALID";
    case -20: return "XR_ERROR_PATH_COUNT_EXCEEDED";
    case -21: return "XR_ERROR_PATH_FORMAT_INVALID";
    case -22: return "XR_ERROR_PATH_UNSUPPORTED";
    case -23: return "XR_ERROR_LAYER_INVALID";
    case -24: return "XR_ERROR_LAYER_LIMIT_EXCEEDED";
    case -25: return "XR_ERROR_SWAPCHAIN_RECT_INVALID";
    case -26: return "XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED";
    case -27: return "XR_ERROR_ACTION_TYPE_MISMATCH";
    case -28: return "XR_ERROR_SESSION_NOT_READY";
    case -29: return "XR_ERROR_SESSION_NOT_STOPPING";
    case -30: return "XR_ERROR_TIME_INVALID";
    case -31: return "XR_ERROR_REFERENCE_SPACE_UNSUPPORTED";
    case -32: return "XR_ERROR_FILE_ACCESS_ERROR";
    case -33: return "XR_ERROR_FILE_CONTENTS_INVALID";
    case -34: return "XR_ERROR_FORM_FACTOR_UNSUPPORTED";
    case -35: return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
    case -36: return "XR_ERROR_API_LAYER_NOT_PRESENT";
    case -37: return "XR_ERROR_CALL_ORDER_INVALID";
    case -38: return "XR_ERROR_GRAPHICS_DEVICE_INVALID";
    case -39: return "XR_ERROR_POSE_INVALID";
    case -40: return "XR_ERROR_INDEX_OUT_OF_RANGE";
    case -41: return "XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED";
    case -42: return "XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED";
    case -44: return "XR_ERROR_NAME_DUPLICATED";
    case -45: return "XR_ERROR_NAME_INVALID";
    case -46: return "XR_ERROR_ACTIONSET_NOT_ATTACHED";
    case -47: return "XR_ERROR_ACTIONSETS_ALREADY_ATTACHED";
    case -48: return "XR_ERROR_LOCALIZED_NAME_DUPLICATED";
    case -49: return "XR_ERROR_LOCALIZED_NAME_INVALID";
    case -50: return "XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING";
    case -51: return "XR_ERROR_RUNTIME_UNAVAILABLE";
    default: return "?";
    }
}
static inline const char *vk_result_name(int r)
{
    switch (r) {
    case 5: return "VK_INCOMPLETE";
    case 4: return "VK_EVENT_RESET";
    case 3: return "VK_EVENT_SET";
    case 2: return "VK_TIMEOUT";
    case 1: return "VK_NOT_READY";
    case 0: return "VK_SUCCESS";
    case -1: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case -2: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case -3: return "VK_ERROR_INITIALIZATION_FAILED";
    case -4: return "VK_ERROR_DEVICE_LOST";
    case -5: return "VK_ERROR_MEMORY_MAP_FAILED";
    case -6: return "VK_ERROR_LAYER_NOT_PRESENT";
    case -7: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case -8: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case -9: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case -10: return "VK_ERROR_TOO_MANY_OBJECTS";
    case -11: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case -12: return "VK_ERROR_FRAGMENTED_POOL";
    case -13: return "VK_ERROR_UNKNOWN";
    default: return "?";
    }
}

#endif
