/* vkprobe: create a Vulkan instance through the system loader and list the physical devices.
 * Used by scripts/quest_setup.sh to verify SwiftShader (VK_ICD_FILENAMES) and the validation layer
 * (VK_LAYER_PATH, layer names as arguments: they are enabled explicitly, so a missing layer fails with
 * VK_ERROR_LAYER_NOT_PRESENT). Exit 0 = at least one device, 1 = vkCreateInstance failed, 2 = no device.
 *   cc -I<Vulkan-Headers>/include vkprobe.c -o vkprobe -l:libvulkan.so.1
 */
#include <stdio.h>
#include <vulkan/vulkan.h>

int main(int argc, char **argv)
{
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkInstance inst;
    VkPhysicalDevice dev[8];
    uint32_t n = 0, i;
    VkResult r;

    ai.pApplicationName = "vkprobe";
    ai.apiVersion = VK_API_VERSION_1_1;
    ci.pApplicationInfo = &ai;
    ci.enabledLayerCount = (uint32_t)(argc - 1);
    ci.ppEnabledLayerNames = (const char *const *)(argv + 1);
    r = vkCreateInstance(&ci, NULL, &inst);
    if (r != VK_SUCCESS) {
        printf("vkCreateInstance failed: %d%s\n", (int)r, r == VK_ERROR_LAYER_NOT_PRESENT ? " (layer not present)" : "");
        return 1;
    }
    vkEnumeratePhysicalDevices(inst, &n, NULL);
    if (n > 8) n = 8;
    vkEnumeratePhysicalDevices(inst, &n, dev);
    for (i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(dev[i], &p);
        printf("device %u: %s (Vulkan %u.%u.%u)\n", i, p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
               VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
    }
    for (i = 1; i < (uint32_t)argc; i++) printf("layer enabled: %s\n", argv[i]);
    vkDestroyInstance(inst, NULL);
    return n ? 0 : 2;
}
