#include <vulkan/vulkan.h>

#include <cstdio>
#include <vector>

int main() {
  VkApplicationInfo app_info{};
  app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app_info.pApplicationName = "rct";
  app_info.apiVersion = VK_API_VERSION_1_3;

  VkInstanceCreateInfo create_info{};
  create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  create_info.pApplicationInfo = &app_info;

  VkInstance instance = VK_NULL_HANDLE;
  if (VkResult result = vkCreateInstance(&create_info, nullptr, &instance);
      result != VK_SUCCESS) {
    std::printf("vkCreateInstance failed: %d\n", result);
    return 1;
  }

  uint32_t device_count = 0;
  vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
  std::vector<VkPhysicalDevice> devices(device_count);
  vkEnumeratePhysicalDevices(instance, &device_count, devices.data());

  std::printf("%u physical device(s)\n", device_count);
  for (VkPhysicalDevice device : devices) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device, &props);
    std::printf("  %s (API %u.%u.%u)\n", props.deviceName,
                VK_API_VERSION_MAJOR(props.apiVersion),
                VK_API_VERSION_MINOR(props.apiVersion),
                VK_API_VERSION_PATCH(props.apiVersion));
  }

  vkDestroyInstance(instance, nullptr);
  return 0;
}
