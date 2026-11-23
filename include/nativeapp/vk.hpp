// nativeapp/vk.hpp — Vulkan de alto nível para NativeActivity.
//
// Camadas:
//   VulkanScene  — instance/device/surface/swapchain/depth/frames in-flight
//   VkPipeline   — builder de graphics pipeline (shaders SPIR-V + layout)
//   VkBuffer     — vertex/index buffer com staging
//
// Compatível com a interface Scene (draw/present/set_clear_color).

#pragma once

#include <android/log.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

#include "assets.hpp"
#include "graphics_types.hpp"
#include "math.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
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

/// Configuração de um graphics pipeline.
struct PipelineConfig {
    std::vector<uint32_t> vertex_spv;
    std::vector<uint32_t> fragment_spv;
    bool depth_test = true;
    bool blend = true;
    Topology topology = Topology::Triangles;
    VkFormat color_format = VK_FORMAT_B8G8R8A8_UNORM;
    VkFormat depth_format = VK_FORMAT_D16_UNORM;
};

/// Builder de graphics pipeline reutilizável.
class GraphicsPipeline {
public:
    GraphicsPipeline() = default;
    ~GraphicsPipeline() { destroy(); }
    GraphicsPipeline(const GraphicsPipeline&) = delete;
    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;

    void build(VkDevice device, VkRenderPass render_pass,
               VkDescriptorSetLayout descriptor_layout, const PipelineConfig& config) {
        destroy();
        if (!device || !render_pass || config.vertex_spv.empty() ||
            config.fragment_spv.empty()) {
            throw VulkanError("GraphicsPipeline: device/render_pass/shaders inválidos");
        }
        device_ = device;

        // Handles locais: se qualquer etapa falhar, nada fica vazando e o
        // objeto permanece utilizável para outra chamada de build().
        VkShaderModule vertex_module = VK_NULL_HANDLE;
        VkShaderModule fragment_module = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        try {
            vertex_module = create_shader_module(device_, config.vertex_spv);
            fragment_module = create_shader_module(device_, config.fragment_spv);

            std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
            stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            stages[0].module = vertex_module;
            stages[0].pName = "main";
            stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[1].module = fragment_module;
            stages[1].pName = "main";

            VkVertexInputBindingDescription binding{};
            binding.binding = 0;
            binding.stride = sizeof(Vertex);
            binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

            std::array<VkVertexInputAttributeDescription, 4> attributes{};
            attributes[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, x)};
            attributes[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, nx)};
            attributes[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, u)};
            attributes[3] = {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, r)};

            VkPipelineVertexInputStateCreateInfo vertex_input{
                VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            vertex_input.vertexBindingDescriptionCount = 1;
            vertex_input.pVertexBindingDescriptions = &binding;
            vertex_input.vertexAttributeDescriptionCount = 4;
            vertex_input.pVertexAttributeDescriptions = attributes.data();

            VkPipelineInputAssemblyStateCreateInfo input_assembly{
                VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            input_assembly.topology = vk_topology(config.topology);

            VkViewport viewport{};
            VkRect2D scissor{};
            VkPipelineViewportStateCreateInfo viewport_state{
                VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            viewport_state.viewportCount = 1;
            viewport_state.pViewports = &viewport;
            viewport_state.scissorCount = 1;
            viewport_state.pScissors = &scissor;

            // A viewport é definida por vkCmdSetViewport/Scissor a cada frame
            // (a swapchain pode mudar de tamanho sem recriar o pipeline).
            const VkDynamicState dynamic_states[] = {
                VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dynamic_state{
                VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
            dynamic_state.dynamicStateCount = 2;
            dynamic_state.pDynamicStates = dynamic_states;

            VkPipelineRasterizationStateCreateInfo rasterization{
                VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            rasterization.polygonMode = VK_POLYGON_MODE_FILL;
            rasterization.cullMode = VK_CULL_MODE_BACK_BIT;
            rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rasterization.lineWidth = 1.0f;

            VkPipelineMultisampleStateCreateInfo multisample{
                VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

            VkPipelineDepthStencilStateCreateInfo depth_stencil{
                VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            depth_stencil.depthTestEnable = config.depth_test ? VK_TRUE : VK_FALSE;
            depth_stencil.depthWriteEnable = config.depth_test ? VK_TRUE : VK_FALSE;
            depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS;

            VkPipelineColorBlendAttachmentState blend_attachment{};
            blend_attachment.colorWriteMask =
                VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            blend_attachment.blendEnable = config.blend ? VK_TRUE : VK_FALSE;
            blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
            blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;

            VkPipelineColorBlendStateCreateInfo color_blend{
                VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            color_blend.attachmentCount = 1;
            color_blend.pAttachments = &blend_attachment;

            VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            layout_info.setLayoutCount = descriptor_layout ? 1 : 0;
            layout_info.pSetLayouts = descriptor_layout ? &descriptor_layout : nullptr;

            vk_check(vkCreatePipelineLayout(device_, &layout_info, nullptr, &layout),
                     "vkCreatePipelineLayout");

            VkGraphicsPipelineCreateInfo pipeline_info{
                VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            pipeline_info.stageCount = 2;
            pipeline_info.pStages = stages.data();
            pipeline_info.pVertexInputState = &vertex_input;
            pipeline_info.pInputAssemblyState = &input_assembly;
            pipeline_info.pViewportState = &viewport_state;
            pipeline_info.pRasterizationState = &rasterization;
            pipeline_info.pMultisampleState = &multisample;
            // Sempre fornece o estado de depth (desabilitado quando config.depth_test
            // é false): o render pass da cena sempre tem anexo de profundidade.
            pipeline_info.pDepthStencilState = &depth_stencil;
            pipeline_info.pColorBlendState = &color_blend;
            pipeline_info.pDynamicState = &dynamic_state;
            pipeline_info.layout = layout;
            pipeline_info.renderPass = render_pass;
            pipeline_info.subpass = 0;

            vk_check(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info,
                                               nullptr, &pipeline),
                     "vkCreateGraphicsPipelines");
        } catch (...) {
            if (pipeline) vkDestroyPipeline(device_, pipeline, nullptr);
            if (layout) vkDestroyPipelineLayout(device_, layout, nullptr);
            if (vertex_module) vkDestroyShaderModule(device_, vertex_module, nullptr);
            if (fragment_module) vkDestroyShaderModule(device_, fragment_module, nullptr);
            device_ = VK_NULL_HANDLE;
            throw;
        }

        vkDestroyShaderModule(device_, vertex_module, nullptr);
        vkDestroyShaderModule(device_, fragment_module, nullptr);
        layout_ = layout;
        pipeline_ = pipeline;
    }

    void bind(VkCommandBuffer command_buffer) const {
        if (pipeline_) {
            vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        }
    }

    VkPipelineLayout layout() const { return layout_; }
    VkPipeline pipeline() const { return pipeline_; }

    void destroy() {
        if (!device_) return;
        if (pipeline_) { vkDestroyPipeline(device_, pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
        if (layout_) { vkDestroyPipelineLayout(device_, layout_, nullptr); layout_ = VK_NULL_HANDLE; }
        device_ = VK_NULL_HANDLE;
    }

private:
    static VkPrimitiveTopology vk_topology(Topology topology) {
        switch (topology) {
            case Topology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            case Topology::TriangleFan: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
            case Topology::Lines: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
            case Topology::LineStrip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
            case Topology::Points: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
            default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        }
    }

    static VkShaderModule create_shader_module(VkDevice device,
                                               const std::vector<uint32_t>& code) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = code.size() * sizeof(uint32_t);
        info.pCode = code.data();
        VkShaderModule module = VK_NULL_HANDLE;
        vk_check(vkCreateShaderModule(device, &info, nullptr, &module),
                 "vkCreateShaderModule");
        return module;
    }

    VkDevice device_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
};

/// Vertex/index buffer com upload direto para memória HOST_VISIBLE/COHERENT
/// (os parâmetros de fila/pool são mantidos para um futuro staging).
class GpuBuffer {
public:
    GpuBuffer() = default;
    ~GpuBuffer() { destroy(); }
    GpuBuffer(const GpuBuffer&) = delete;
    GpuBuffer& operator=(const GpuBuffer&) = delete;
    GpuBuffer(GpuBuffer&& other) noexcept { move_from(other); }
    GpuBuffer& operator=(GpuBuffer&& other) noexcept {
        if (this != &other) { destroy(); move_from(other); }
        return *this;
    }

    void upload(VkDevice device, VkPhysicalDevice physical_device, VkQueue queue,
                VkCommandPool command_pool, const MeshData& mesh) {
        (void)queue;         // upload direto; ainda não há staging buffer
        (void)command_pool;
        destroy();
        if (!device || !physical_device) {
            throw VulkanError("GpuBuffer: device/physical_device inválido");
        }
        const size_t max_u32 = std::numeric_limits<uint32_t>::max();
        if (mesh.vertices.size() > max_u32 || mesh.indices.size() > max_u32) {
            throw VulkanError("GpuBuffer: mesh excede o limite de uint32_t");
        }
        device_ = device;
        vertex_count_ = static_cast<uint32_t>(mesh.vertices.size());
        index_count_ = static_cast<uint32_t>(mesh.indices.size());
        topology_ = mesh.topology;

        if (!mesh.vertices.empty()) {
            create_buffer(physical_device, mesh.vertices.size() * sizeof(Vertex),
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, mesh.vertices.data(),
                          vertex_buffer_, vertex_memory_);
        }
        if (!mesh.indices.empty()) {
            create_buffer(physical_device, mesh.indices.size() * sizeof(uint16_t),
                          VK_BUFFER_USAGE_INDEX_BUFFER_BIT, mesh.indices.data(),
                          index_buffer_, index_memory_);
        }
    }

    void bind(VkCommandBuffer command_buffer) const {
        if (vertex_buffer_) {
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(command_buffer, 0, 1, &vertex_buffer_, &offset);
        }
        if (index_buffer_) {
            vkCmdBindIndexBuffer(command_buffer, index_buffer_, 0, VK_INDEX_TYPE_UINT16);
        }
    }

    void draw(VkCommandBuffer command_buffer) const {
        if (index_buffer_) {
            vkCmdDrawIndexed(command_buffer, index_count_, 1, 0, 0, 0);
        } else if (vertex_buffer_) {
            vkCmdDraw(command_buffer, vertex_count_, 1, 0, 0);
        }
    }

    void destroy() {
        if (!device_) return;
        if (vertex_buffer_) vkDestroyBuffer(device_, vertex_buffer_, nullptr);
        if (index_buffer_) vkDestroyBuffer(device_, index_buffer_, nullptr);
        if (vertex_memory_) vkFreeMemory(device_, vertex_memory_, nullptr);
        if (index_memory_) vkFreeMemory(device_, index_memory_, nullptr);
        vertex_buffer_ = index_buffer_ = VK_NULL_HANDLE;
        vertex_memory_ = index_memory_ = VK_NULL_HANDLE;
        vertex_count_ = index_count_ = 0;
        device_ = VK_NULL_HANDLE;
    }

private:
    void move_from(GpuBuffer& other) noexcept {
        device_ = other.device_;
        vertex_buffer_ = other.vertex_buffer_;
        index_buffer_ = other.index_buffer_;
        vertex_memory_ = other.vertex_memory_;
        index_memory_ = other.index_memory_;
        vertex_count_ = other.vertex_count_;
        index_count_ = other.index_count_;
        topology_ = other.topology_;
        other.device_ = VK_NULL_HANDLE;
        other.vertex_buffer_ = other.index_buffer_ = VK_NULL_HANDLE;
        other.vertex_memory_ = other.index_memory_ = VK_NULL_HANDLE;
        other.vertex_count_ = other.index_count_ = 0;
        other.topology_ = Topology::Triangles;
    }

    void create_buffer(VkPhysicalDevice physical_device, VkDeviceSize size,
                       VkBufferUsageFlags usage, const void* data,
                       VkBuffer& buffer, VkDeviceMemory& memory) {
        if (size == 0 || !data) throw VulkanError("GpuBuffer: dados vazios");
        VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_info.size = size;
        buffer_info.usage = usage;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vk_check(vkCreateBuffer(device_, &buffer_info, nullptr, &buffer), "vkCreateBuffer");

        try {
            VkMemoryRequirements requirements;
            vkGetBufferMemoryRequirements(device_, buffer, &requirements);
            VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            alloc.allocationSize = requirements.size;
            alloc.memoryTypeIndex = find_memory_type(
                physical_device, requirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            vk_check(vkAllocateMemory(device_, &alloc, nullptr, &memory),
                     "vkAllocateMemory");

            void* mapped = VK_NULL_HANDLE;
            vk_check(vkMapMemory(device_, memory, 0, size, 0, &mapped), "vkMapMemory");
            if (!mapped) throw VulkanError("vkMapMemory retornou ponteiro nulo");
            std::memcpy(mapped, data, static_cast<size_t>(size));
            vkUnmapMemory(device_, memory);
            vk_check(vkBindBufferMemory(device_, buffer, memory, 0),
                     "vkBindBufferMemory");
        } catch (...) {
            if (memory) { vkFreeMemory(device_, memory, nullptr); memory = VK_NULL_HANDLE; }
            if (buffer) { vkDestroyBuffer(device_, buffer, nullptr); buffer = VK_NULL_HANDLE; }
            throw;
        }
    }

    uint32_t find_memory_type(VkPhysicalDevice device, uint32_t type_filter,
                              VkMemoryPropertyFlags properties) const {
        VkPhysicalDeviceMemoryProperties memory_properties;
        vkGetPhysicalDeviceMemoryProperties(device, &memory_properties);
        for (uint32_t i = 0; i < memory_properties.memoryTypeCount && i < 32; ++i) {
            if ((type_filter & (1u << i)) &&
                (memory_properties.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }
        throw VulkanError("Nenhum memory type adequado");
    }

    VkDevice device_ = VK_NULL_HANDLE;
    VkBuffer vertex_buffer_ = VK_NULL_HANDLE;
    VkBuffer index_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory vertex_memory_ = VK_NULL_HANDLE;
    VkDeviceMemory index_memory_ = VK_NULL_HANDLE;
    uint32_t vertex_count_ = 0;
    uint32_t index_count_ = 0;
    Topology topology_ = Topology::Triangles;
};

// ============================================================================
// VulkanScene — implementa Scene
// ============================================================================

class VulkanScene : public Scene {
public:
    static constexpr int kMaxFramesInFlight = 2;

    VulkanScene() = default;
    ~VulkanScene() override { destroy(); }
    VulkanScene(const VulkanScene&) = delete;
    VulkanScene& operator=(const VulkanScene&) = delete;

    void init(ANativeActivity* activity) {
        if (initialized_) return;
        activity_ = activity;
        Assets::instance(activity).set_activity(activity);
        try {
            create_instance();
            pick_physical_device();
            create_device();
        } catch (...) {
            if (device_) { vkDestroyDevice(device_, nullptr); device_ = VK_NULL_HANDLE; }
            if (instance_) { vkDestroyInstance(instance_, nullptr); instance_ = VK_NULL_HANDLE; }
            physical_device_ = VK_NULL_HANDLE;
            queue_ = VK_NULL_HANDLE;
            throw;
        }
        initialized_ = true;
    }

    void init(ANativeActivity* activity, ANativeWindow* window) {
        init(activity);
        attach(window);
    }

    void attach(ANativeWindow* window) {
        if (!initialized_ || !window) return;
        if (surface_ && window_ == window && swapchain_) return;
        invalidate();
        window_ = window;
        try {
            create_surface();
            create_swapchain();
            create_command_buffers();
        } catch (...) {
            invalidate();  // limpa swapchain/superfície parciais
            throw;
        }
        swapchain_ok_ = true;
    }

    void invalidate() {
        if (!initialized_) return;
        if (device_ && vkDeviceWaitIdle(device_) != VK_SUCCESS) {
            // Device lost: a espera falha, mas a limpeza abaixo ainda é segura.
            __android_log_print(ANDROID_LOG_WARN, "nativeapp-vk",
                                "vkDeviceWaitIdle falhou em invalidate");
        }
        destroy_swapchain();
        if (surface_) {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }
        window_ = VK_NULL_HANDLE;
        swapchain_ok_ = false;
    }

    void set_clear_color(float r, float g, float b, float a = 1.0f) override {
        clear_ = {{r, g, b, a}};
    }

    void set_present_mode(VkPresentModeKHR mode) {
        present_mode_ = mode;
        if (initialized_ && window_) recreate_swapchain();
    }

    void set_validation(bool enabled) { validation_ = enabled; }

    void resize(int, int) override {
        if (initialized_ && window_) recreate_swapchain();
    }

    void begin_frame() override {}  // clear acontece no render pass
    void present() override { render(); }

    /// Configura um pipeline de geometria (remove o modo "só clear").
    void set_pipeline(PipelineConfig config) {
        if (!initialized_ || !render_pass_) return;
        // O formato da attachment é definido pela swapchain, não pelo chamador.
        if (swapchain_) config.color_format = color_format_;
        config.depth_format = depth_format_;
        pipeline_config_ = std::move(config);
        pipeline_.build(device_, render_pass_, VK_NULL_HANDLE, pipeline_config_);
    }

    /// Desenha uma mesh (requer set_pipeline()).
    void draw(const MeshData& mesh, const Material& material,
              const float* model = nullptr) override {
        if (!pipeline_.pipeline() || !swapchain_ok_) return;
        (void)material;
        (void)model;  // uniforms via descriptor sets em versão futura
        // A mesh antiga pode estar em uso por frames in-flight: espera todos
        // antes de substituí-la (senão destroy() correria com o GPU).
        for (VkFence fence : in_flight_fences_) {
            if (fence) {
                const VkResult result =
                    vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);
                if (result == VK_ERROR_DEVICE_LOST) { handle_device_loss(); return; }
                if (result != VK_SUCCESS) return;
            }
        }
        GpuBuffer buffers;
        buffers.upload(device_, physical_device_, queue_, command_pool_, mesh);
        current_mesh_ = std::move(buffers);
        has_mesh_ = true;
    }

    void render() {
        if (!initialized_ || !swapchain_ok_ || !window_) return;

        const uint32_t frame = current_frame_ % kMaxFramesInFlight;
        VkResult result = vkWaitForFences(device_, 1, &in_flight_fences_[frame],
                                          VK_TRUE, 1000000000);
        if (result == VK_ERROR_DEVICE_LOST) { handle_device_loss(); return; }
        if (result == VK_TIMEOUT) {
            // Fence ainda em uso: não resetar nem submeter neste frame.
            __android_log_print(ANDROID_LOG_WARN, "nativeapp-vk",
                                "fence do frame %u ainda ocupada", frame);
            return;
        }
        if (result != VK_SUCCESS) { vk_check(result, "vkWaitForFences"); return; }

        uint32_t image_index = 0;
        result = vkAcquireNextImageKHR(
            device_, swapchain_, UINT64_MAX, image_available_[frame],
            VK_NULL_HANDLE, &image_index);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            recreate_swapchain();
            return;
        }
        if (result == VK_ERROR_DEVICE_LOST) { handle_device_loss(); return; }
        vk_check(result, "vkAcquireNextImageKHR");
        if (image_index >= image_count_) return;  // defesa contra índice inválido

        // A imagem pode ainda estar em uso por outro frame. Espera a fence
        // dona dela ANTES de resetar a fence deste frame: resetar primeiro
        // transformaria essa espera em deadlock (a fence já está limpa).
        if (images_in_flight_[image_index] != VK_NULL_HANDLE) {
            result = vkWaitForFences(device_, 1, &images_in_flight_[image_index],
                                     VK_TRUE, UINT64_MAX);
            if (result == VK_ERROR_DEVICE_LOST) { handle_device_loss(); return; }
            if (result != VK_SUCCESS) {
                vk_check(result, "vkWaitForFences(imagem)");
                return;
            }
        }
        images_in_flight_[image_index] = in_flight_fences_[frame];
        vk_check(vkResetFences(device_, 1, &in_flight_fences_[frame]),
                 "vkResetFences");

        vk_check(vkResetCommandBuffer(command_buffers_[frame], 0),
                 "vkResetCommandBuffer");
        record_command_buffer(command_buffers_[frame], image_index);

        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &image_available_[frame];
        submit.pWaitDstStageMask = &wait_stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_buffers_[frame];
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &render_finished_[frame];
        result = vkQueueSubmit(queue_, 1, &submit, in_flight_fences_[frame]);
        if (result == VK_ERROR_DEVICE_LOST) { handle_device_loss(); return; }
        vk_check(result, "vkQueueSubmit");

        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &render_finished_[frame];
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain_;
        present.pImageIndices = &image_index;
        result = vkQueuePresentKHR(queue_, &present);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            recreate_swapchain();
            return;
        }
        if (result == VK_ERROR_DEVICE_LOST) { handle_device_loss(); return; }
        vk_check(result, "vkQueuePresentKHR");

        current_frame_++;
    }

    void destroy() {
        if (!initialized_) return;
        invalidate();
        if (render_pass_) {
            vkDestroyRenderPass(device_, render_pass_, nullptr);
            render_pass_ = VK_NULL_HANDLE;
        }
        pipeline_.destroy();
        current_mesh_.destroy();
        if (device_) { vkDestroyDevice(device_, nullptr); device_ = VK_NULL_HANDLE; }
        if (instance_) { vkDestroyInstance(instance_, nullptr); instance_ = VK_NULL_HANDLE; }
        queue_ = VK_NULL_HANDLE;
        physical_device_ = VK_NULL_HANDLE;
        has_mesh_ = false;
        swapchain_ok_ = false;
        current_frame_ = 0;
        initialized_ = false;
    }

private:
    /// Device loss invalida tudo; a cena volta ao estado pré-init() e o app
    /// pode chamar init(activity, window) novamente.
    void handle_device_loss() {
        __android_log_print(ANDROID_LOG_ERROR, "nativeapp-vk",
                            "dispositivo Vulkan perdido — reinicializando estado");
        destroy();
    }

    void create_instance() {
        VkApplicationInfo app_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app_info.pApplicationName = "apkfast nativeapp";
        app_info.apiVersion = VK_API_VERSION_1_0;

        std::vector<const char*> extensions = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
        };

        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        create.pApplicationInfo = &app_info;
        create.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        create.ppEnabledExtensionNames = extensions.data();
        vk_check(vkCreateInstance(&create, nullptr, &instance_), "vkCreateInstance");
        if (validation_) {
            // A API não habilita layers: avisa para não dar a falsa impressão
            // de que a validação está ativa.
            __android_log_print(ANDROID_LOG_WARN, "nativeapp-vk",
                                "set_validation(true): layers de validação não habilitadas");
        }
    }

    void pick_physical_device() {
        uint32_t count = 0;
        vk_check(vkEnumeratePhysicalDevices(instance_, &count, nullptr),
                 "vkEnumeratePhysicalDevices");
        if (count == 0) throw VulkanError("Nenhum dispositivo Vulkan encontrado");
        std::vector<VkPhysicalDevice> devices(count);
        vk_check(vkEnumeratePhysicalDevices(instance_, &count, devices.data()),
                 "vkEnumeratePhysicalDevices");
        // Prefere dispositivo discreto, senão usa o primeiro.
        physical_device_ = devices[0];
        for (auto device : devices) {
            VkPhysicalDeviceProperties properties;
            vkGetPhysicalDeviceProperties(device, &properties);
            if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                physical_device_ = device;
                break;
            }
        }
    }

    void create_device() {
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &count, nullptr);
        if (count == 0) throw VulkanError("Nenhuma família de filas Vulkan");
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &count, families.data());

        bool found_graphics = false;
        queue_family_ = 0;
        for (uint32_t i = 0; i < count; ++i) {
            if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                queue_family_ = i;
                found_graphics = true;
                break;
            }
        }
        if (!found_graphics) throw VulkanError("Nenhuma família de filas com GRAPHICS_BIT");

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

    void create_swapchain() {
        VkSurfaceCapabilitiesKHR capabilities{};
        vk_check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_,
                                                           &capabilities),
                 "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

        uint32_t format_count = 0;
        vk_check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_,
                                                      &format_count, nullptr),
                 "vkGetPhysicalDeviceSurfaceFormatsKHR");
        std::vector<VkSurfaceFormatKHR> formats(format_count);
        if (format_count > 0) {
            vk_check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_,
                                                          &format_count, formats.data()),
                     "vkGetPhysicalDeviceSurfaceFormatsKHR");
        }
        if (formats.empty()) {
            throw VulkanError("Nenhum formato de superfície disponível");
        }
        const VkSurfaceFormatKHR format = formats[0];
        color_format_ = format.format;
        pipeline_config_.color_format = color_format_;

        uint32_t mode_count = 0;
        vk_check(vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_,
                                                           &mode_count, nullptr),
                 "vkGetPhysicalDeviceSurfacePresentModesKHR");
        std::vector<VkPresentModeKHR> modes(mode_count);
        if (mode_count > 0) {
            vk_check(vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_,
                                                               &mode_count, modes.data()),
                     "vkGetPhysicalDeviceSurfacePresentModesKHR");
        }
        // FIFO é obrigatório pela especificação; fallback seguro se o modo
        // solicitado não existir no dispositivo.
        VkPresentModeKHR mode = present_mode_;
        if (std::find(modes.begin(), modes.end(), mode) == modes.end()) {
            mode = VK_PRESENT_MODE_FIFO_KHR;
        }

        VkExtent2D extent = capabilities.currentExtent;
        if (extent.width == UINT32_MAX || extent.height == UINT32_MAX ||
            extent.width == 0 || extent.height == 0) {
            const int w = ANativeWindow_getWidth(window_);
            const int h = ANativeWindow_getHeight(window_);
            extent.width = w > 0 ? static_cast<uint32_t>(w) : 1u;
            extent.height = h > 0 ? static_cast<uint32_t>(h) : 1u;
        }
        extent_ = extent;  // usado pelo render pass e pela viewport

        uint32_t image_count = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount > 0 && image_count > capabilities.maxImageCount) {
            image_count = capabilities.maxImageCount;
        }
        if (image_count == 0) image_count = 1;

        VkSwapchainCreateInfoKHR create{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        create.surface = surface_;
        create.minImageCount = image_count;
        create.imageFormat = format.format;
        create.imageColorSpace = format.colorSpace;
        create.imageExtent = extent;
        create.imageArrayLayers = 1;
        create.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        create.preTransform = capabilities.currentTransform;
        create.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        create.presentMode = mode;
        create.clipped = VK_TRUE;
        vk_check(vkCreateSwapchainKHR(device_, &create, nullptr, &swapchain_),
                 "vkCreateSwapchainKHR");

        uint32_t queried = 0;
        vk_check(vkGetSwapchainImagesKHR(device_, swapchain_, &queried, nullptr),
                 "vkGetSwapchainImagesKHR");
        if (queried == 0) throw VulkanError("Swapchain sem imagens");
        images_.assign(queried, VK_NULL_HANDLE);
        uint32_t written = queried;
        vk_check(vkGetSwapchainImagesKHR(device_, swapchain_, &written, images_.data()),
                 "vkGetSwapchainImagesKHR");
        if (written == 0 || written > queried) {
            throw VulkanError("vkGetSwapchainImagesKHR: contagem inconsistente");
        }
        image_count_ = written;

        image_views_.assign(image_count_, VK_NULL_HANDLE);
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

        create_depth_resources(extent);
        create_render_pass();
        create_framebuffers(extent);
        create_sync_objects();
    }

    void create_depth_resources(VkExtent2D extent) {
        VkFormat candidates[] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT,
                                 VK_FORMAT_D16_UNORM};
        depth_format_ = VK_FORMAT_UNDEFINED;
        for (auto candidate : candidates) {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(physical_device_, candidate, &properties);
            if (properties.optimalTilingFeatures &
                VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
                depth_format_ = candidate;
                break;
            }
        }
        if (depth_format_ == VK_FORMAT_UNDEFINED) {
            throw VulkanError("Nenhum formato de profundidade suportado");
        }
        pipeline_config_.depth_format = depth_format_;

        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = depth_format_;
        image.extent = {extent.width, extent.height, 1};
        image.mipLevels = 1;
        image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        vk_check(vkCreateImage(device_, &image, nullptr, &depth_image_), "vkCreateImage");

        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(device_, depth_image_, &requirements);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = requirements.size;
        alloc.memoryTypeIndex = find_memory_type(
            requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vk_check(vkAllocateMemory(device_, &alloc, nullptr, &depth_memory_),
                 "vkAllocateMemory");
        vkBindImageMemory(device_, depth_image_, depth_memory_, 0);

        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = depth_image_;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = depth_format_;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        vk_check(vkCreateImageView(device_, &view, nullptr, &depth_view_),
                 "vkCreateImageView");
    }

    void create_render_pass() {
        VkAttachmentDescription color{};
        color.format = color_format_;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentDescription depth{};
        depth.format = depth_format_;
        depth.samples = VK_SAMPLE_COUNT_1_BIT;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        std::array<VkAttachmentDescription, 2> attachments = {color, depth};
        VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depth_ref{1,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_ref;
        subpass.pDepthStencilAttachment = &depth_ref;

        // Garante que as transições de layout das attachments aconteçam
        // antes do primeiro uso do subpass.
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        info.attachmentCount = 2;
        info.pAttachments = attachments.data();
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        info.dependencyCount = 1;
        info.pDependencies = &dependency;
        vk_check(vkCreateRenderPass(device_, &info, nullptr, &render_pass_),
                 "vkCreateRenderPass");
    }

    void create_framebuffers(VkExtent2D extent) {
        framebuffers_.assign(image_count_, VK_NULL_HANDLE);
        for (uint32_t i = 0; i < image_count_; ++i) {
            std::array<VkImageView, 2> views = {image_views_[i], depth_view_};
            VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            info.renderPass = render_pass_;
            info.attachmentCount = 2;
            info.pAttachments = views.data();
            info.width = extent.width;
            info.height = extent.height;
            info.layers = 1;
            vk_check(vkCreateFramebuffer(device_, &info, nullptr, &framebuffers_[i]),
                     "vkCreateFramebuffer");
        }
    }

    void create_sync_objects() {
        const size_t count = kMaxFramesInFlight;
        image_available_.resize(count);
        render_finished_.resize(count);
        in_flight_fences_.resize(count);
        images_in_flight_.assign(image_count_, VK_NULL_HANDLE);

        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        for (size_t i = 0; i < count; ++i) {
            vk_check(vkCreateSemaphore(device_, &semaphore, nullptr,
                                       &image_available_[i]),
                     "vkCreateSemaphore(image_available)");
            vk_check(vkCreateSemaphore(device_, &semaphore, nullptr,
                                       &render_finished_[i]),
                     "vkCreateSemaphore(render_finished)");
            vk_check(vkCreateFence(device_, &fence, nullptr, &in_flight_fences_[i]),
                     "vkCreateFence");
        }
    }

    void create_command_buffers() {
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.queueFamilyIndex = queue_family_;
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        vk_check(vkCreateCommandPool(device_, &pool, nullptr, &command_pool_),
                 "vkCreateCommandPool");

        command_buffers_.resize(kMaxFramesInFlight);
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = command_pool_;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = kMaxFramesInFlight;
        vk_check(vkAllocateCommandBuffers(device_, &alloc, command_buffers_.data()),
                 "vkAllocateCommandBuffers");
    }

    void record_command_buffer(VkCommandBuffer command_buffer, uint32_t image_index) {
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vk_check(vkBeginCommandBuffer(command_buffer, &begin), "vkBeginCommandBuffer");

        VkClearValue clear_values[2]{};
        clear_values[0].color = clear_;
        clear_values[1].depthStencil = {1.0f, 0};

        VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        pass.renderPass = render_pass_;
        pass.framebuffer = framebuffers_[image_index];
        pass.renderArea.offset = {0, 0};
        // Usa a extent da swapchain: a janela pode mudar entre resize() e a
        // recriação da swapchain, e um renderArea maior que o framebuffer é
        // inválido.
        pass.renderArea.extent = extent_;
        pass.clearValueCount = 2;
        pass.pClearValues = clear_values;

        vkCmdBeginRenderPass(command_buffer, &pass, VK_SUBPASS_CONTENTS_INLINE);

        // Pipelines usam VK_DYNAMIC_STATE_VIEWPORT/SCISSOR.
        VkViewport viewport{0.0f, 0.0f,
                            static_cast<float>(extent_.width),
                            static_cast<float>(extent_.height), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, extent_};
        vkCmdSetViewport(command_buffer, 0, 1, &viewport);
        vkCmdSetScissor(command_buffer, 0, 1, &scissor);

        if (pipeline_.pipeline() && has_mesh_) {
            pipeline_.bind(command_buffer);
            current_mesh_.bind(command_buffer);
            current_mesh_.draw(command_buffer);
        }

        vkCmdEndRenderPass(command_buffer);
        vk_check(vkEndCommandBuffer(command_buffer), "vkEndCommandBuffer");
    }

    uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties) const {
        VkPhysicalDeviceMemoryProperties memory_properties;
        vkGetPhysicalDeviceMemoryProperties(physical_device_, &memory_properties);
        for (uint32_t i = 0; i < memory_properties.memoryTypeCount && i < 32; ++i) {
            if ((type_filter & (1u << i)) &&
                (memory_properties.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }
        throw VulkanError("Nenhum memory type adequado");
    }

    void destroy_swapchain() {
        if (!swapchain_) return;
        if (device_) vkDeviceWaitIdle(device_);

        for (auto fence : in_flight_fences_) if (fence) vkDestroyFence(device_, fence, nullptr);
        for (auto semaphore : image_available_) if (semaphore) vkDestroySemaphore(device_, semaphore, nullptr);
        for (auto semaphore : render_finished_) if (semaphore) vkDestroySemaphore(device_, semaphore, nullptr);
        in_flight_fences_.clear();
        image_available_.clear();
        render_finished_.clear();
        images_in_flight_.clear();
        command_buffers_.clear();

        if (command_pool_) { vkDestroyCommandPool(device_, command_pool_, nullptr); command_pool_ = VK_NULL_HANDLE; }
        for (auto framebuffer : framebuffers_) vkDestroyFramebuffer(device_, framebuffer, nullptr);
        framebuffers_.clear();
        if (render_pass_) { vkDestroyRenderPass(device_, render_pass_, nullptr); render_pass_ = VK_NULL_HANDLE; }
        if (depth_view_) { vkDestroyImageView(device_, depth_view_, nullptr); depth_view_ = VK_NULL_HANDLE; }
        if (depth_image_) { vkDestroyImage(device_, depth_image_, nullptr); depth_image_ = VK_NULL_HANDLE; }
        if (depth_memory_) { vkFreeMemory(device_, depth_memory_, nullptr); depth_memory_ = VK_NULL_HANDLE; }
        for (auto view : image_views_) vkDestroyImageView(device_, view, nullptr);
        image_views_.clear();
        images_.clear();
        image_count_ = 0;
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }

    void recreate_swapchain() {
        if (!initialized_ || !window_) return;
        ANativeWindow* saved_window = window_;
        if (ANativeWindow_getWidth(saved_window) <= 0 ||
            ANativeWindow_getHeight(saved_window) <= 0) return;
        invalidate();
        window_ = saved_window;  // preserva a janela entre recreações
        try {
            create_surface();
            create_swapchain();
            create_command_buffers();
            // Sem pipeline custom o modo clear dos exemplos antigos continua
            // válido; com SPIR-V, reconstrói o pipeline para o novo render pass.
            if (!pipeline_config_.vertex_spv.empty()) {
                pipeline_config_.color_format = color_format_;
                pipeline_.build(device_, render_pass_, VK_NULL_HANDLE, pipeline_config_);
            }
        } catch (...) {
            swapchain_ok_ = false;
            invalidate();
            throw;
        }
        swapchain_ok_ = true;
    }

    ANativeActivity* activity_ = VK_NULL_HANDLE;
    ANativeWindow* window_ = VK_NULL_HANDLE;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat color_format_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkFormat depth_format_ = VK_FORMAT_D16_UNORM;
    VkImage depth_image_ = VK_NULL_HANDLE;
    VkDeviceMemory depth_memory_ = VK_NULL_HANDLE;
    VkImageView depth_view_ = VK_NULL_HANDLE;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkExtent2D extent_{1, 1};
    uint32_t image_count_ = 0;
    std::vector<VkImage> images_;
    std::vector<VkImageView> image_views_;
    std::vector<VkFramebuffer> framebuffers_;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> command_buffers_;
    std::vector<VkSemaphore> image_available_;
    std::vector<VkSemaphore> render_finished_;
    std::vector<VkFence> in_flight_fences_;
    std::vector<VkFence> images_in_flight_;
    uint32_t current_frame_ = 0;
    VkClearColorValue clear_{{0.08f, 0.08f, 0.12f, 1.0f}};
    VkPresentModeKHR present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
    bool validation_ = false;
    bool initialized_ = false;
    bool swapchain_ok_ = false;

    GraphicsPipeline pipeline_;
    PipelineConfig pipeline_config_;
    GpuBuffer current_mesh_;
    bool has_mesh_ = false;
};

}  // namespace nativeapp
