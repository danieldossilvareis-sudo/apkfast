// nativeapp/vk.hpp — Cena Vulkan mínima para NativeActivity.
//
// Cria instance -> physical device -> device -> surface -> swapchain ->
// image views -> command buffers, e expõe render() com clear color
// configurável via vkCmdClearColorImage (pipeline mínimo e válido).
//
// Exemplo:
//   class App : public nativeapp::App {
//     nativeapp::VulkanScene scene_;
//     void on_ready() override { scene_.init(activity(), window()); }
//     void on_resize(int w, int h) override { scene_.resize(w, h); }
//     void on_draw() override { scene_.render(); }
//   };
//
// Requer: -lvulkan (apkfast adiciona automaticamente quando detecta #include).

#pragma once

#include <android/native_activity.h>
#include <android/native_window.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace nativeapp {

class VulkanError : public std::runtime_error {
public:
    explicit VulkanError(const std::string& message) : std::runtime_error(message) {}
};

inline void vk_check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw VulkanError(std::string(operation) + " falhou (VkResult=" +
                          std::to_string(result) + ")");
    }
}

class VulkanScene {
public:
    VulkanScene() = default;
    ~VulkanScene() { destroy(); }

    VulkanScene(const VulkanScene&) = delete;
    VulkanScene& operator=(const VulkanScene&) = delete;

    /// Inicializa uma vez (instance/device). A surface/swapchain são criadas
    /// por attach(), chamado pelo App em on_surface_created().
    void init(ANativeActivity* activity) {
        if (initialized_) return;
        activity_ = activity;
        create_instance();
        pick_physical_device();
        create_device();
        initialized_ = true;
    }

    /// Compatibilidade: init com janela já anexa a surface.
    void init(ANativeActivity* activity, ANativeWindow* window) {
        init(activity);
        attach(window);
    }

    /// Cria/recria surface + swapchain para uma janela.
    void attach(ANativeWindow* window) {
        if (!initialized_ || !window) return;
        if (surface_ && window_ == window && swapchain_) return;

        invalidate();
        window_ = window;
        create_surface();
        create_swapchain();
        create_command_buffers();
        swapchain_ok_ = true;
    }

    /// Chamado quando a janela é destruída (background/rotação).
    void invalidate() {
        if (!initialized_) return;
        if (device_) vkDeviceWaitIdle(device_);
        destroy_swapchain();
        if (surface_) {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
            surface_ = nullptr;
        }
        window_ = nullptr;
        swapchain_ok_ = false;
    }

    void set_clear_color(float r, float g, float b, float a = 1.0f) {
        clear_ = {{r, g, b, a}};
    }

    void resize(int width, int height) {
        (void)width;
        (void)height;
        if (!initialized_ || !window_) return;
        recreate_swapchain();
    }

    /// Recria swapchain com validação de extent (evita 0x0 após resume).
    void recreate_swapchain() {
        if (!initialized_ || !window_) return;
        if (ANativeWindow_getWidth(window_) <= 0 ||
            ANativeWindow_getHeight(window_) <= 0) return;
        if (device_) vkDeviceWaitIdle(device_);
        destroy_swapchain();
        create_swapchain();
        create_command_buffers();
        swapchain_ok_ = true;
    }

    void render() {
        if (!initialized_ || !swapchain_ok_ || !window_) return;
        uint32_t image_index = 0;
        VkResult result = vkAcquireNextImageKHR(
            device_, swapchain_, UINT64_MAX, image_available_, VK_NULL_HANDLE, &image_index);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            recreate_swapchain();
            return;
        }
        if (result == VK_ERROR_DEVICE_LOST) {
            invalidate();
            return;
        }
        vk_check(result, "vkAcquireNextImageKHR");

        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &image_available_;
        submit.pWaitDstStageMask = &wait_stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_buffers_[image_index];
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &render_finished_;
        vk_check(vkQueueSubmit(queue_, 1, &submit, fence_), "vkQueueSubmit");

        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &render_finished_;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain_;
        present.pImageIndices = &image_index;
        result = vkQueuePresentKHR(queue_, &present);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            recreate_swapchain();
            return;
        }
        if (result == VK_ERROR_DEVICE_LOST) {
            invalidate();
            return;
        }
        vk_check(result, "vkQueuePresentKHR");

        vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
        vkResetFences(device_, 1, &fence_);
    }

    void destroy() {
        if (!initialized_) return;
        invalidate();
        if (fence_) {
            vkDestroyFence(device_, fence_, nullptr);
            fence_ = nullptr;
        }
        if (image_available_) {
            vkDestroySemaphore(device_, image_available_, nullptr);
            image_available_ = nullptr;
        }
        if (render_finished_) {
            vkDestroySemaphore(device_, render_finished_, nullptr);
            render_finished_ = nullptr;
        }
        if (device_) {
            vkDestroyDevice(device_, nullptr);
            device_ = nullptr;
        }
        if (instance_) {
            vkDestroyInstance(instance_, nullptr);
            instance_ = nullptr;
        }
        initialized_ = false;
    }

private:
    void create_instance() {
        VkApplicationInfo app_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app_info.pApplicationName = "apkfast nativeapp";
        app_info.apiVersion = VK_API_VERSION_1_0;

        const char* extensions[] = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
        };
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        create.pApplicationInfo = &app_info;
        create.enabledExtensionCount = 2;
        create.ppEnabledExtensionNames = extensions;
        vk_check(vkCreateInstance(&create, nullptr, &instance_), "vkCreateInstance");
    }

    void pick_physical_device() {
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance_, &count, nullptr);
        if (count == 0) throw VulkanError("Nenhum dispositivo Vulkan encontrado");
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance_, &count, devices.data());
        physical_device_ = devices[0];  // simples: primeiro dispositivo
    }

    void create_device() {
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &count, families.data());

        queue_family_ = 0;
        for (uint32_t i = 0; i < count; ++i) {
            if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                queue_family_ = i;
                break;
            }
        }

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_info.queueFamilyIndex = queue_family_;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;

        const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        create.queueCreateInfoCount = 1;
        create.pQueueCreateInfos = &queue_info;
        create.enabledExtensionCount = 1;
        create.ppEnabledExtensionNames = extensions;
        vk_check(vkCreateDevice(physical_device_, &create, nullptr, &device_), "vkCreateDevice");
        vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    }

    void create_surface() {
        VkAndroidSurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
        info.window = window_;
        vk_check(vkCreateAndroidSurfaceKHR(instance_, &info, nullptr, &surface_),
                 "vkCreateAndroidSurfaceKHR");
    }

    void query_swapchain_support() {
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &capabilities_);
        uint32_t count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &count, nullptr);
        formats_.resize(count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &count, formats_.data());
        count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &count, nullptr);
        modes_.resize(count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &count, modes_.data());
    }

    void create_swapchain() {
        query_swapchain_support();
        VkSurfaceFormatKHR format = formats_.empty()
            ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
            : formats_[0];
        VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;

        VkExtent2D extent = capabilities_.currentExtent;
        if (extent.width == UINT32_MAX) {
            extent.width = static_cast<uint32_t>(ANativeWindow_getWidth(window_));
            extent.height = static_cast<uint32_t>(ANativeWindow_getHeight(window_));
        }

        uint32_t image_count = capabilities_.minImageCount + 1;
        if (capabilities_.maxImageCount > 0 && image_count > capabilities_.maxImageCount) {
            image_count = capabilities_.maxImageCount;
        }

        VkSwapchainCreateInfoKHR create{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        create.surface = surface_;
        create.minImageCount = image_count;
        create.imageFormat = format.format;
        create.imageColorSpace = format.colorSpace;
        create.imageExtent = extent;
        create.imageArrayLayers = 1;
        create.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        create.preTransform = capabilities_.currentTransform;
        create.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        create.presentMode = mode;
        create.clipped = VK_TRUE;
        vk_check(vkCreateSwapchainKHR(device_, &create, nullptr, &swapchain_),
                 "vkCreateSwapchainKHR");

        vkGetSwapchainImagesKHR(device_, swapchain_, &image_count_, nullptr);
        images_.resize(image_count_);
        vkGetSwapchainImagesKHR(device_, swapchain_, &image_count_, images_.data());

        image_views_.resize(image_count_);
        for (uint32_t i = 0; i < image_count_; ++i) {
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = images_[i];
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = format.format;
            view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            view.subresourceRange.levelCount = 1;
            view.subresourceRange.layerCount = 1;
            vk_check(vkCreateImageView(device_, &view, nullptr, &image_views_[i]),
                     "vkCreateImageView");
        }

        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(device_, &semaphore, nullptr, &image_available_);
        vkCreateSemaphore(device_, &semaphore, nullptr, &render_finished_);

        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(device_, &fence, nullptr, &fence_);
    }

    void create_command_buffers() {
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.queueFamilyIndex = queue_family_;
        vk_check(vkCreateCommandPool(device_, &pool, nullptr, &command_pool_),
                 "vkCreateCommandPool");

        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = command_pool_;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = image_count_;
        command_buffers_.resize(image_count_);
        vk_check(vkAllocateCommandBuffers(device_, &alloc, command_buffers_.data()),
                 "vkAllocateCommandBuffers");

        for (uint32_t i = 0; i < image_count_; ++i) {
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            vk_check(vkBeginCommandBuffer(command_buffers_[i], &begin), "vkBeginCommandBuffer");

            // Transição: UNDEFINED -> TRANSFER_DST para poder limpar a imagem.
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = images_[i];
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(command_buffers_[i], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                 0, nullptr, 0, nullptr, 1, &barrier);

            VkClearColorValue color = clear_;
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(command_buffers_[i], images_[i],
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);

            // Transição de volta para PRESENT_SRC.
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            vkCmdPipelineBarrier(command_buffers_[i], VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                                 0, nullptr, 0, nullptr, 1, &barrier);

            vk_check(vkEndCommandBuffer(command_buffers_[i]), "vkEndCommandBuffer");
        }
    }

    void destroy_swapchain() {
        if (!swapchain_) return;
        if (command_pool_) {
            vkDestroyCommandPool(device_, command_pool_, nullptr);
            command_pool_ = nullptr;
        }
        for (auto view : image_views_) vkDestroyImageView(device_, view, nullptr);
        image_views_.clear();
        if (fence_) { vkDestroyFence(device_, fence_, nullptr); fence_ = nullptr; }
        if (image_available_) { vkDestroySemaphore(device_, image_available_, nullptr); image_available_ = nullptr; }
        if (render_finished_) { vkDestroySemaphore(device_, render_finished_, nullptr); render_finished_ = nullptr; }
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = nullptr;
    }

    ANativeActivity* activity_ = nullptr;
    ANativeWindow* window_ = nullptr;
    VkInstance instance_ = nullptr;
    VkPhysicalDevice physical_device_ = nullptr;
    VkDevice device_ = nullptr;
    VkQueue queue_ = nullptr;
    uint32_t queue_family_ = 0;
    VkSurfaceKHR surface_ = nullptr;
    VkSwapchainKHR swapchain_ = nullptr;
    VkSurfaceCapabilitiesKHR capabilities_{};
    std::vector<VkSurfaceFormatKHR> formats_;
    std::vector<VkPresentModeKHR> modes_;
    uint32_t image_count_ = 0;
    std::vector<VkImage> images_;
    std::vector<VkImageView> image_views_;
    VkCommandPool command_pool_ = nullptr;
    std::vector<VkCommandBuffer> command_buffers_;
    VkSemaphore image_available_ = nullptr;
    VkSemaphore render_finished_ = nullptr;
    VkFence fence_ = nullptr;
    VkClearColorValue clear_{{0.08f, 0.08f, 0.12f, 1.0f}};
    bool initialized_ = false;
    bool swapchain_ok_ = false;
};

}  // namespace nativeapp
