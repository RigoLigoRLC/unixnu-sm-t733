// SPDX-License-Identifier: MIT
#include "NativeTurnipSmoke.h"

#include <syslog.h>

namespace {

void progress(uint32_t value)
{
    printf("t733-turnip: stage=0x%04x\n", value);
    fflush(stdout);
    syslog(LOG_NOTICE, "stage=0x%04x", value);
}

void describe_device(const char *path)
{
    void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        printf("t733-turnip: load failed: %s\n", dlerror());
        return;
    }

    auto entry = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vk_icdGetInstanceProcAddr"));
    if (!entry) {
        dlclose(library);
        return;
    }

    auto create = reinterpret_cast<PFN_vkCreateInstance>(entry(nullptr, "vkCreateInstance"));
    if (!create) {
        dlclose(library);
        return;
    }

    VkApplicationInfo application = {};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "t733-turnip";
    application.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo description = {};
    description.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    description.pApplicationInfo = &application;
    VkInstance instance = nullptr;
    VkResult result = create(&description, nullptr, &instance);
    if (result == VK_SUCCESS) {
        auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(entry(instance, "vkEnumeratePhysicalDevices"));
        auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(entry(instance, "vkGetPhysicalDeviceProperties"));
        auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(entry(instance, "vkDestroyInstance"));
        uint32_t count = 1;
        VkPhysicalDevice physical = nullptr;
        if (enumerate && properties && enumerate(instance, &count, &physical) == VK_SUCCESS && count && physical) {
            VkPhysicalDeviceProperties data = {};
            properties(physical, &data);
            printf("t733-turnip: device=%s vendor=0x%x device_id=0x%x Vulkan=%u.%u.%u\n",
                data.deviceName, data.vendorID, data.deviceID, VK_API_VERSION_MAJOR(data.apiVersion),
                VK_API_VERSION_MINOR(data.apiVersion), VK_API_VERSION_PATCH(data.apiVersion));
        }

        if (destroy) {
            destroy(instance, nullptr);
        }
    } else {
        printf("t733-turnip: identity instance result=%d\n", result);
    }

    fflush(stdout);
    dlclose(library);
}

}

int main(int argc, char **argv)
{
    if (argc > 2) {
        fprintf(stderr, "Usage: %s [libvulkan_freedreno.dylib]\n", argv[0]);
        return 1;
    }

    char sibling[1024];
    const char *path = argc == 2 ? argv[1] : nullptr;
    if (!path) {
        if (!turnip::libraryPath(sibling, sizeof(sibling))) {
            return 1;
        }

        path = sibling;
    }

    openlog("t733-turnip", LOG_PID | LOG_CONS, LOG_DAEMON);
    setenv("TU_DARWIN_SERVICE", "NativeAdrenoGpuPublished", 1);
    setenv("TU_DARWIN_OPEN_TYPE", "0x4147", 1);
    describe_device(path);

    auto marker = [](uint32_t value) {
        progress(value);
    };
    turnip::Result result = turnip::run(marker, path);
    printf("t733-turnip: result=%s stage=%d VkResult=%d fill_words=%d compute_words=%d "
        "compute_stage=%d triangle_pixels=%d other_pixels=%d render_stage=%d error=%s\n",
        result.ok ? "completed" : "failed", result.stage, result.vkResult,
        result.verified, result.computeWords, result.computeStage, result.renderTriangle,
        result.renderOther, result.renderStage, turnip::gFirstError);
    fflush(stdout);
    syslog(result.ok ? LOG_NOTICE : LOG_ERR,
        "result=%s stage=%d VkResult=%d fill=%d compute=%d triangle=%d other=%d compute_stage=%d render_stage=%d",
        result.ok ? "completed" : "failed", result.stage, result.vkResult,
        result.verified, result.computeWords, result.renderTriangle, result.renderOther,
        result.computeStage, result.renderStage);
    return result.ok ? 0 : 2;
}
