// nativeapp/vk.hpp — Vulkan de alto nível para NativeActivity.
//
// Camadas:
//   VulkanScene  — instance/device/surface/swapchain/depth/frames in-flight,
//                  UBO por frame, descritores, texturas e texto
//   VkPipeline   — builder de graphics pipeline (shaders SPIR-V embutidos)
//   GpuBuffer    — vertex/index buffer com upload HOST_VISIBLE
//   TextureVk    — imagem RGBA em DEVICE_LOCAL via staging + sampler
//
// Compatível com a interface Scene (draw/present/set_clear_color) e com a
// ergonomia de GLES2Scene: draw(MeshData, Material, model) desenha geometria
// com cor/textura/depth_test/blend, e load_font()/draw_text() funcionam.

#pragma once

#include <android/log.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

#include "assets.hpp"
#include "font.hpp"
#include "graphics_types.hpp"
#include "image_codecs.hpp"
#include "math.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
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

/// Handle opaco de um conjunto de descritores de usuário (0 = nenhum).
using CustomUniformHandle = std::size_t;

/// Descrição de um binding de uniforms customizados. Apenas
/// VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER e VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
/// são suportados; `size` é o tamanho em bytes do UBO (ignorado por sampler).
struct CustomUniformBinding {
    uint32_t binding = 0;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    VkShaderStageFlags stages =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    std::size_t size = 0;
};

class VulkanUploadTracker;

namespace vk_detail {

/// Mesma política usada pelos buffers da cena: escolhe a primeira memory type
/// compatível com os bits exigidos pelo recurso e com as propriedades pedidas.
inline uint32_t find_memory_type(VkPhysicalDevice device, uint32_t type_filter,
                                 VkMemoryPropertyFlags properties) {
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

/// Transição de layout de uma imagem de texture (usada no upload via staging).
inline void transition_image_layout(VkCommandBuffer command_buffer, VkImage image,
                                    VkImageLayout old_layout, VkImageLayout new_layout) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags source_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags destination_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED &&
        new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
               new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        source_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destination_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else {
        throw VulkanError("Transição de layout de imagem não suportada");
    }
    vkCmdPipelineBarrier(command_buffer, source_stage, destination_stage, 0,
                         0, nullptr, 0, nullptr, 1, &barrier);
}

/// Vértice do pipeline de texto: x, y, u, v (mesmo layout do shader de texto).
struct GlyphVertex {
    float x = 0, y = 0, u = 0, v = 0;
};

/// Projeção 2D em pixels com origem no canto superior esquerdo.
///
/// math.hpp::mat4_ortho2d() usa bottom=height/top=0 porque o NDC do GL tem
/// y=+1 no topo. No NDC do Vulkan y=+1 está na BASE: para produzir a mesma
/// imagem de GLES2Scene (y=0 no topo) é preciso trocar bottom/top. Usar a
/// matriz do GL aqui espelha verticalmente todo o conteúdo 2D e é a causa
/// de texto de cabeça para baixo e geometria na posição errada.
inline Mat4 ortho2d_screen(float width, float height) {
    // y=0 → NDC -1 (topo no Vulkan); y=height → NDC +1 (base).
    return mat4_ortho(0.0f, width, 0.0f, height, -1.0f, 1.0f);
}

}  // namespace vk_detail

/// Coordena uploads de textura sem `vkQueueWaitIdle()` na cena: cada upload
/// sinaliza um semáforo que o próximo submit de render espera (estágio de
/// fragment shader) e uma fence que dona o lifetime dos recursos de staging.
/// Texturas substituídas são aposentadas aqui e liberadas somente depois que
/// nenhum frame in-flight pode mais amostrá-las.
class VulkanUploadTracker {
public:
    struct Record {
        VkDevice device = VK_NULL_HANDLE;
        VkCommandPool command_pool = VK_NULL_HANDLE;
        VkCommandBuffer command = VK_NULL_HANDLE;
        VkBuffer staging_buffer = VK_NULL_HANDLE;
        VkDeviceMemory staging_memory = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        bool owns_fence = true;
        VkSemaphore ready = VK_NULL_HANDLE;
        VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory image_memory = VK_NULL_HANDLE;
        uint64_t submitted_before = 0;
        bool ready_consumed = false;
    };

    /// Staging + semáforo de um upload recém-submetido.
    void track_upload(Record record) {
        record.submitted_before = submissions_;
        records_.push_back(record);
    }

    /// Recursos de uma textura que foi substituída/destruída.
    void retire_texture(Record record) {
        record.submitted_before = submissions_;
        records_.push_back(record);
    }

    void note_submission() { ++submissions_; }

    std::vector<VkSemaphore> pending_semaphores() const {
        std::vector<VkSemaphore> result;
        for (const Record& record : records_) {
            if (record.ready && !record.ready_consumed) result.push_back(record.ready);
        }
        return result;
    }

    void mark_consumed() {
        for (Record& record : records_) {
            if (record.ready) record.ready_consumed = true;
        }
    }

    /// Chamado com a fila de frames parada (todas as fences sinalizadas).
    /// A extra de `submissions_` garante que pelo menos um render consumiu o
    /// semáforo/descriptor aposentado antes da liberação.
    void reap(bool frames_idle) {
        if (!device_) return;
        for (size_t i = 0; i < records_.size();) {
            Record& record = records_[i];
            const VkResult status = record.fence
                ? vkGetFenceStatus(device_, record.fence) : VK_SUCCESS;
            // Com uploads a cada frame as fences nunca ficam todas ociosas ao
            // mesmo tempo. Depois de kMaxFramesInFlight+1 submits, o frame que
            // podia amostrar o recurso aposentado já foi reutilizado (a cena
            // espera a fence antes de reutilizar), então é seguro liberar.
            const bool past_in_flight =
                submissions_ >= record.submitted_before + 3;
            if (status != VK_SUCCESS || submissions_ <= record.submitted_before ||
                (!frames_idle && !past_in_flight)) {
                ++i;
                continue;
            }
            destroy_record(record);
            records_.erase(records_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }

    /// Chamado sob vkDeviceWaitIdle() em destroy()/invalidate() completa.
    void force_cleanup() {
        for (Record& record : records_) destroy_record(record);
        records_.clear();
    }

    /// Device lost: handles antigos não podem ser tocados.
    void reset() { records_.clear(); }

    void set_device(VkDevice device) { device_ = device; }
    bool empty() const { return records_.empty(); }

private:
    static void destroy_record(Record& record) {
        if (!record.device) return;
        if (record.command) {
            vkFreeCommandBuffers(record.device, record.command_pool, 1, &record.command);
        }
        if (record.staging_memory) vkFreeMemory(record.device, record.staging_memory, nullptr);
        if (record.staging_buffer) vkDestroyBuffer(record.device, record.staging_buffer, nullptr);
        if (record.descriptor && record.descriptor_pool) {
            vkFreeDescriptorSets(record.device, record.descriptor_pool, 1, &record.descriptor);
        }
        if (record.sampler) vkDestroySampler(record.device, record.sampler, nullptr);
        if (record.view) vkDestroyImageView(record.device, record.view, nullptr);
        if (record.image) vkDestroyImage(record.device, record.image, nullptr);
        if (record.image_memory) vkFreeMemory(record.device, record.image_memory, nullptr);
        if (record.ready) vkDestroySemaphore(record.device, record.ready, nullptr);
        if (record.fence && record.owns_fence) {
            vkDestroyFence(record.device, record.fence, nullptr);
        }
    }

    VkDevice device_ = VK_NULL_HANDLE;
    std::vector<Record> records_;
    uint64_t submissions_ = 0;
};

/// Configuração de um graphics pipeline.
struct PipelineConfig {
    std::vector<uint32_t> vertex_spv;
    std::vector<uint32_t> fragment_spv;
    bool depth_test = true;
    bool blend = true;
    Topology topology = Topology::Triangles;
    VkFormat color_format = VK_FORMAT_B8G8R8A8_UNORM;
    VkFormat depth_format = VK_FORMAT_D16_UNORM;
    // Culling desligado por default para casar com GLES2Scene (que não cull).
    VkCullModeFlags cull_mode = VK_CULL_MODE_NONE;
    // true = layout de texto (location 0/1 = vec2 pos/uv); false = Vertex.
    bool text_layout = false;
    // true = binding 1 com inputRate=INSTANCE e mat4 nas locations 4..7.
    bool instanced = false;
    // Handle registrado por register_custom_uniforms(); 0 = sets embutidos.
    CustomUniformHandle custom_uniforms = 0;
};

/// Builder de graphics pipeline reutilizável.
class GraphicsPipeline {
public:
    GraphicsPipeline() = default;
    ~GraphicsPipeline() { destroy(); }
    GraphicsPipeline(const GraphicsPipeline&) = delete;
    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;

    /// API legada (um layout opcional). Mantida para apps que já usavam
    /// set_pipeline() com SPIR-V próprio.
    void build(VkDevice device, VkRenderPass render_pass,
               VkDescriptorSetLayout descriptor_layout, const PipelineConfig& config) {
        if (descriptor_layout) {
            build_with_layouts(device, render_pass, &descriptor_layout, 1, config);
        } else {
            build_with_layouts(device, render_pass, nullptr, 0, config);
        }
    }

    /// API da cena: permite os dois descriptor sets (UBO e sampler) usados
    /// pelos pipelines embutidos.
    void build_with_layouts(VkDevice device, VkRenderPass render_pass,
                            const VkDescriptorSetLayout* layouts, uint32_t layout_count,
                            const PipelineConfig& config) {
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

            std::array<VkVertexInputBindingDescription, 2> bindings{};
            bindings[0].binding = 0;
            bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
            uint32_t binding_count = 1;
            std::array<VkVertexInputAttributeDescription, 8> attributes{};
            uint32_t attribute_count = 0;

            if (config.text_layout) {
                bindings[0].stride = sizeof(vk_detail::GlyphVertex);
                attributes[attribute_count++] = {0, 0, VK_FORMAT_R32G32_SFLOAT,
                                                 offsetof(vk_detail::GlyphVertex, x)};
                attributes[attribute_count++] = {1, 0, VK_FORMAT_R32G32_SFLOAT,
                                                 offsetof(vk_detail::GlyphVertex, u)};
            } else {
                bindings[0].stride = sizeof(Vertex);
                attributes[attribute_count++] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT,
                                                 offsetof(Vertex, x)};
                attributes[attribute_count++] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT,
                                                 offsetof(Vertex, nx)};
                attributes[attribute_count++] = {2, 0, VK_FORMAT_R32G32_SFLOAT,
                                                 offsetof(Vertex, u)};
                attributes[attribute_count++] = {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                                                 offsetof(Vertex, r)};
                if (config.instanced) {
                    // Instancing real: uma matriz por instância não um loop de
                    // vkCmdDraw. locations 4..7 = 4 colunas vec4 (std140).
                    bindings[1] = {1, static_cast<uint32_t>(sizeof(Mat4)),
                                   VK_VERTEX_INPUT_RATE_INSTANCE};
                    binding_count = 2;
                    const uint32_t column_offsets[4] = {0, 16, 32, 48};
                    for (uint32_t column = 0; column < 4; ++column) {
                        attributes[attribute_count++] = {
                            4 + column, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                            column_offsets[column]};
                    }
                }
            }

            VkPipelineVertexInputStateCreateInfo vertex_input{
                VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            vertex_input.vertexBindingDescriptionCount = binding_count;
            vertex_input.pVertexBindingDescriptions = bindings.data();
            vertex_input.vertexAttributeDescriptionCount = attribute_count;
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

            // A viewport é definida por vkCmdSetViewport/Scissor a cada frame.
            const VkDynamicState dynamic_states[] = {
                VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dynamic_state{
                VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
            dynamic_state.dynamicStateCount = 2;
            dynamic_state.pDynamicStates = dynamic_states;

            VkPipelineRasterizationStateCreateInfo rasterization{
                VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            rasterization.polygonMode = VK_POLYGON_MODE_FILL;
            rasterization.cullMode = config.cull_mode;
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
            layout_info.setLayoutCount = layout_count;
            layout_info.pSetLayouts = layout_count ? layouts : nullptr;

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
            // Sempre fornece o estado de depth (desabilitado quando
            // config.depth_test é false): o render pass sempre tem o anexo.
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

/// Vertex/index buffer com upload direto para memória HOST_VISIBLE/COHERENT.
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
        (void)queue;         // upload direto; texturas é que usam staging
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
            // Mesma politica de GLES2Scene::Mesh::upload(): indice fora do
            // intervalo de vertices cai para drawArrays, com erro claro.
            size_t invalid_index = 0;
            bool invalid = false;
            for (uint16_t index : mesh.indices) {
                if (static_cast<size_t>(index) >= mesh.vertices.size()) {
                    invalid_index = index;
                    invalid = true;
                    break;
                }
            }
            if (invalid) {
                __android_log_print(ANDROID_LOG_WARN, "nativeapp-vk",
                                    "indice uint16 invalido (%u >= %zu vertices); "
                                    "desenhando com drawArrays",
                                    static_cast<unsigned>(invalid_index),
                                    mesh.vertices.size());
            } else {
                create_buffer(physical_device, mesh.indices.size() * sizeof(uint16_t),
                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT, mesh.indices.data(),
                              index_buffer_, index_memory_);
            }
        }
    }

    /// Upload de vértices com layout próprio (usado pelo atlas de texto).
    void upload_raw(VkDevice device, VkPhysicalDevice physical_device,
                    const void* vertices, size_t vertex_count, size_t stride) {
        destroy();
        if (!device || !physical_device || !vertices || vertex_count == 0 || stride == 0) {
            return;
        }
        if (vertex_count > std::numeric_limits<uint32_t>::max()) {
            throw VulkanError("GpuBuffer: vértices excedem o limite de uint32_t");
        }
        device_ = device;
        vertex_count_ = static_cast<uint32_t>(vertex_count);
        topology_ = Topology::Triangles;
        create_buffer(physical_device, vertex_count * stride,
                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices,
                      vertex_buffer_, vertex_memory_);
    }

    /// Matrizes de instância em memória HOST_VISIBLE, binding 1 com divisor 1.
    /// Chamado uma vez por lote; N instâncias viram um único vkCmdDraw*.
    void upload_instances(VkDevice device, VkPhysicalDevice physical_device,
                          const std::vector<Mat4>& models) {
        if (!device || !physical_device || models.empty()) return;
        if (models.size() > std::numeric_limits<uint32_t>::max()) {
            throw VulkanError("GpuBuffer: instâncias excedem o limite de uint32_t");
        }
        destroy_instances();
        instance_device_ = device;
        instance_count_ = static_cast<uint32_t>(models.size());
        create_buffer(physical_device, models.size() * sizeof(Mat4),
                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, models.data(),
                      instance_buffer_, instance_memory_);
    }

    void bind(VkCommandBuffer command_buffer) const {
        if (vertex_buffer_) {
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(command_buffer, 0, 1, &vertex_buffer_, &offset);
        }
        if (instance_buffer_) {
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(command_buffer, 1, 1, &instance_buffer_, &offset);
        }
        if (index_buffer_) {
            vkCmdBindIndexBuffer(command_buffer, index_buffer_, 0, VK_INDEX_TYPE_UINT16);
        }
    }

    void draw(VkCommandBuffer command_buffer) const {
        const uint32_t instances = instance_count_ ? instance_count_ : 1;
        if (index_buffer_) {
            vkCmdDrawIndexed(command_buffer, index_count_, instances, 0, 0, 0);
        } else if (vertex_buffer_) {
            vkCmdDraw(command_buffer, vertex_count_, instances, 0, 0);
        }
    }

    bool has_vertices() const { return vertex_buffer_ != VK_NULL_HANDLE; }

    void destroy() {
        destroy_instances();
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
    void destroy_instances() {
        if (!instance_device_) return;
        if (instance_buffer_) vkDestroyBuffer(instance_device_, instance_buffer_, nullptr);
        if (instance_memory_) vkFreeMemory(instance_device_, instance_memory_, nullptr);
        instance_buffer_ = VK_NULL_HANDLE;
        instance_memory_ = VK_NULL_HANDLE;
        instance_count_ = 0;
        instance_device_ = VK_NULL_HANDLE;
    }

    void move_from(GpuBuffer& other) noexcept {
        device_ = other.device_;
        vertex_buffer_ = other.vertex_buffer_;
        index_buffer_ = other.index_buffer_;
        vertex_memory_ = other.vertex_memory_;
        index_memory_ = other.index_memory_;
        vertex_count_ = other.vertex_count_;
        index_count_ = other.index_count_;
        topology_ = other.topology_;
        instance_device_ = other.instance_device_;
        instance_buffer_ = other.instance_buffer_;
        instance_memory_ = other.instance_memory_;
        instance_count_ = other.instance_count_;
        other.device_ = VK_NULL_HANDLE;
        other.vertex_buffer_ = VK_NULL_HANDLE;
        other.index_buffer_ = VK_NULL_HANDLE;
        other.vertex_memory_ = VK_NULL_HANDLE;
        other.index_memory_ = VK_NULL_HANDLE;
        other.vertex_count_ = other.index_count_ = 0;
        other.topology_ = Topology::Triangles;
        other.instance_device_ = VK_NULL_HANDLE;
        other.instance_buffer_ = VK_NULL_HANDLE;
        other.instance_memory_ = VK_NULL_HANDLE;
        other.instance_count_ = 0;
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

            void* mapped = nullptr;
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
        return vk_detail::find_memory_type(device, type_filter, properties);
    }

    VkDevice device_ = VK_NULL_HANDLE;
    VkBuffer vertex_buffer_ = VK_NULL_HANDLE;
    VkBuffer index_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory vertex_memory_ = VK_NULL_HANDLE;
    VkDeviceMemory index_memory_ = VK_NULL_HANDLE;
    uint32_t vertex_count_ = 0;
    uint32_t index_count_ = 0;
    Topology topology_ = Topology::Triangles;
    VkDevice instance_device_ = VK_NULL_HANDLE;
    VkBuffer instance_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory instance_memory_ = VK_NULL_HANDLE;
    uint32_t instance_count_ = 0;
};

// ============================================================================
// Shaders embutidos (GLSL 450 → SPIR-V via glslangValidator)
// ============================================================================
//
// Os quatro sources abaixo são a fonte canônica dos arrays SPIR-V incorporados.
// Para regenerar (fora do build, glslangValidator não é dependência do apkfast):
//
//   glslangValidator -V -S vert geometry.vert -o geometry.vert.spv
//   glslangValidator -V -S frag geometry.frag -o geometry.frag.spv
//   glslangValidator -V -S vert text.vert -o text.vert.spv
//   glslangValidator -V -S frag text.frag -o text.frag.spv
//
// Observação: o upload preserva a ordem das linhas retornada por
// decode_asset_image()/FontAtlas. Os vertex shaders usam v_uv = a_uv, ou seja,
// os UVs de MeshData::quad() e do FontAtlas são aplicados exatamente como no
// GLES2Scene (inclusive a convenção de orientação já existente no GL).
//
// --- geometry.vert -----------------------------------------------------------
// #version 450
// layout(location = 0) in vec3 a_position;
// layout(location = 1) in vec3 a_normal;
// layout(location = 2) in vec2 a_uv;
// layout(location = 3) in vec4 a_color;
// layout(set = 0, binding = 0) uniform SceneUniforms {
//     mat4 u_projection; mat4 u_model; vec4 u_color; float u_use_texture;
// } ubo;
// layout(location = 0) out vec2 v_uv;
// layout(location = 1) out vec4 v_color;
// void main() {
//     v_uv = a_uv;
//     v_color = a_color;
//     gl_Position = ubo.u_projection * ubo.u_model * vec4(a_position, 1.0);
// }
//
// --- geometry.frag -----------------------------------------------------------
// #version 450
// layout(location = 0) in vec2 v_uv;
// layout(location = 1) in vec4 v_color;
// layout(set = 0, binding = 0) uniform SceneUniforms {
//     mat4 u_projection; mat4 u_model; vec4 u_color; float u_use_texture;
// } ubo;
// layout(set = 1, binding = 0) uniform sampler2D u_texture;
// layout(location = 0) out vec4 out_color;
// void main() {
//     vec4 base = ubo.u_use_texture > 0.5 ? texture(u_texture, v_uv) : vec4(1.0);
//     out_color = base * v_color * ubo.u_color;
// }
//
// --- text.vert ---------------------------------------------------------------
// #version 450
// layout(location = 0) in vec2 a_position;
// layout(location = 1) in vec2 a_uv;
// layout(set = 0, binding = 0) uniform SceneUniforms {
//     mat4 u_projection; mat4 u_model; vec4 u_color; float u_use_texture;
// } ubo;
// layout(location = 0) out vec2 v_uv;
// void main() {
//     v_uv = a_uv;
//     gl_Position = ubo.u_projection * ubo.u_model * vec4(a_position, 0.0, 1.0);
// }
//
// --- text.frag ---------------------------------------------------------------
// #version 450
// layout(location = 0) in vec2 v_uv;
// layout(set = 0, binding = 0) uniform SceneUniforms {
//     mat4 u_projection; mat4 u_model; vec4 u_color; float u_use_texture;
// } ubo;
// layout(set = 1, binding = 0) uniform sampler2D u_texture;
// layout(location = 0) out vec4 out_color;
// void main() {
//     float alpha = texture(u_texture, v_uv).a;
//     out_color = vec4(ubo.u_color.rgb, ubo.u_color.a * alpha);
// }

// --- instanced.vert ---------------------------------------------------------
// #version 450
// layout(location = 0) in vec3 a_position;
// layout(location = 1) in vec3 a_normal;
// layout(location = 2) in vec2 a_uv;
// layout(location = 3) in vec4 a_color;
// layout(location = 4) in vec4 i_model0;
// layout(location = 5) in vec4 i_model1;
// layout(location = 6) in vec4 i_model2;
// layout(location = 7) in vec4 i_model3;
// layout(set = 0, binding = 0) uniform SceneUniforms {
//     mat4 u_projection; mat4 u_model; vec4 u_color; float u_use_texture;
// } ubo;
// layout(location = 0) out vec2 v_uv;
// layout(location = 1) out vec4 v_color;
// void main() {
//     v_uv = a_uv;
//     v_color = a_color;
//     mat4 model = mat4(i_model0, i_model1, i_model2, i_model3);
//     gl_Position = ubo.u_projection * model * vec4(a_position, 1.0);
// }
//
namespace vk_detail {
// kGeometryInstancedVertSpv: 636 palavras, gerado com glslangValidator -V --target-env vulkan1.0.
static constexpr uint32_t kGeometryInstancedVertSpv[] = {
    0x07230203u, 0x00010000u, 0x0008000bu, 0x00000050u, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000bu,
    0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
    0x0010000fu, 0x00000000u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00000009u, 0x0000000bu, 0x0000000fu,
    0x00000011u, 0x00000016u, 0x00000018u, 0x0000001au, 0x0000001cu, 0x0000003au, 0x00000047u, 0x0000004fu,
    0x00030003u, 0x00000002u, 0x000001c2u, 0x00040005u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00040005u,
    0x00000009u, 0x76755f76u, 0x00000000u, 0x00040005u, 0x0000000bu, 0x76755f61u, 0x00000000u, 0x00040005u,
    0x0000000fu, 0x6f635f76u, 0x00726f6cu, 0x00040005u, 0x00000011u, 0x6f635f61u, 0x00726f6cu, 0x00040005u,
    0x00000015u, 0x65646f6du, 0x0000006cu, 0x00050005u, 0x00000016u, 0x6f6d5f69u, 0x306c6564u, 0x00000000u,
    0x00050005u, 0x00000018u, 0x6f6d5f69u, 0x316c6564u, 0x00000000u, 0x00050005u, 0x0000001au, 0x6f6d5f69u,
    0x326c6564u, 0x00000000u, 0x00050005u, 0x0000001cu, 0x6f6d5f69u, 0x336c6564u, 0x00000000u, 0x00060005u,
    0x00000038u, 0x505f6c67u, 0x65567265u, 0x78657472u, 0x00000000u, 0x00060006u, 0x00000038u, 0x00000000u,
    0x505f6c67u, 0x7469736fu, 0x006e6f69u, 0x00070006u, 0x00000038u, 0x00000001u, 0x505f6c67u, 0x746e696fu,
    0x657a6953u, 0x00000000u, 0x00070006u, 0x00000038u, 0x00000002u, 0x435f6c67u, 0x4470696cu, 0x61747369u,
    0x0065636eu, 0x00070006u, 0x00000038u, 0x00000003u, 0x435f6c67u, 0x446c6c75u, 0x61747369u, 0x0065636eu,
    0x00030005u, 0x0000003au, 0x00000000u, 0x00060005u, 0x0000003du, 0x6e656353u, 0x696e5565u, 0x6d726f66u,
    0x00000073u, 0x00070006u, 0x0000003du, 0x00000000u, 0x72705f75u, 0x63656a6fu, 0x6e6f6974u, 0x00000000u,
    0x00050006u, 0x0000003du, 0x00000001u, 0x6f6d5f75u, 0x006c6564u, 0x00050006u, 0x0000003du, 0x00000002u,
    0x6f635f75u, 0x00726f6cu, 0x00070006u, 0x0000003du, 0x00000003u, 0x73755f75u, 0x65745f65u, 0x72757478u,
    0x00000065u, 0x00030005u, 0x0000003fu, 0x006f6275u, 0x00050005u, 0x00000047u, 0x6f705f61u, 0x69746973u,
    0x00006e6fu, 0x00050005u, 0x0000004fu, 0x6f6e5f61u, 0x6c616d72u, 0x00000000u, 0x00040047u, 0x00000009u,
    0x0000001eu, 0x00000000u, 0x00040047u, 0x0000000bu, 0x0000001eu, 0x00000002u, 0x00040047u, 0x0000000fu,
    0x0000001eu, 0x00000001u, 0x00040047u, 0x00000011u, 0x0000001eu, 0x00000003u, 0x00040047u, 0x00000016u,
    0x0000001eu, 0x00000004u, 0x00040047u, 0x00000018u, 0x0000001eu, 0x00000005u, 0x00040047u, 0x0000001au,
    0x0000001eu, 0x00000006u, 0x00040047u, 0x0000001cu, 0x0000001eu, 0x00000007u, 0x00030047u, 0x00000038u,
    0x00000002u, 0x00050048u, 0x00000038u, 0x00000000u, 0x0000000bu, 0x00000000u, 0x00050048u, 0x00000038u,
    0x00000001u, 0x0000000bu, 0x00000001u, 0x00050048u, 0x00000038u, 0x00000002u, 0x0000000bu, 0x00000003u,
    0x00050048u, 0x00000038u, 0x00000003u, 0x0000000bu, 0x00000004u, 0x00030047u, 0x0000003du, 0x00000002u,
    0x00040048u, 0x0000003du, 0x00000000u, 0x00000005u, 0x00050048u, 0x0000003du, 0x00000000u, 0x00000007u,
    0x00000010u, 0x00050048u, 0x0000003du, 0x00000000u, 0x00000023u, 0x00000000u, 0x00040048u, 0x0000003du,
    0x00000001u, 0x00000005u, 0x00050048u, 0x0000003du, 0x00000001u, 0x00000007u, 0x00000010u, 0x00050048u,
    0x0000003du, 0x00000001u, 0x00000023u, 0x00000040u, 0x00050048u, 0x0000003du, 0x00000002u, 0x00000023u,
    0x00000080u, 0x00050048u, 0x0000003du, 0x00000003u, 0x00000023u, 0x00000090u, 0x00040047u, 0x0000003fu,
    0x00000021u, 0x00000000u, 0x00040047u, 0x0000003fu, 0x00000022u, 0x00000000u, 0x00040047u, 0x00000047u,
    0x0000001eu, 0x00000000u, 0x00040047u, 0x0000004fu, 0x0000001eu, 0x00000001u, 0x00020013u, 0x00000002u,
    0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u, 0x00000006u, 0x00000020u, 0x00040017u, 0x00000007u,
    0x00000006u, 0x00000002u, 0x00040020u, 0x00000008u, 0x00000003u, 0x00000007u, 0x0004003bu, 0x00000008u,
    0x00000009u, 0x00000003u, 0x00040020u, 0x0000000au, 0x00000001u, 0x00000007u, 0x0004003bu, 0x0000000au,
    0x0000000bu, 0x00000001u, 0x00040017u, 0x0000000du, 0x00000006u, 0x00000004u, 0x00040020u, 0x0000000eu,
    0x00000003u, 0x0000000du, 0x0004003bu, 0x0000000eu, 0x0000000fu, 0x00000003u, 0x00040020u, 0x00000010u,
    0x00000001u, 0x0000000du, 0x0004003bu, 0x00000010u, 0x00000011u, 0x00000001u, 0x00040018u, 0x00000013u,
    0x0000000du, 0x00000004u, 0x00040020u, 0x00000014u, 0x00000007u, 0x00000013u, 0x0004003bu, 0x00000010u,
    0x00000016u, 0x00000001u, 0x0004003bu, 0x00000010u, 0x00000018u, 0x00000001u, 0x0004003bu, 0x00000010u,
    0x0000001au, 0x00000001u, 0x0004003bu, 0x00000010u, 0x0000001cu, 0x00000001u, 0x0004002bu, 0x00000006u,
    0x0000001eu, 0x3f800000u, 0x0004002bu, 0x00000006u, 0x0000001fu, 0x00000000u, 0x00040015u, 0x00000035u,
    0x00000020u, 0x00000000u, 0x0004002bu, 0x00000035u, 0x00000036u, 0x00000001u, 0x0004001cu, 0x00000037u,
    0x00000006u, 0x00000036u, 0x0006001eu, 0x00000038u, 0x0000000du, 0x00000006u, 0x00000037u, 0x00000037u,
    0x00040020u, 0x00000039u, 0x00000003u, 0x00000038u, 0x0004003bu, 0x00000039u, 0x0000003au, 0x00000003u,
    0x00040015u, 0x0000003bu, 0x00000020u, 0x00000001u, 0x0004002bu, 0x0000003bu, 0x0000003cu, 0x00000000u,
    0x0006001eu, 0x0000003du, 0x00000013u, 0x00000013u, 0x0000000du, 0x00000006u, 0x00040020u, 0x0000003eu,
    0x00000002u, 0x0000003du, 0x0004003bu, 0x0000003eu, 0x0000003fu, 0x00000002u, 0x00040020u, 0x00000040u,
    0x00000002u, 0x00000013u, 0x00040017u, 0x00000045u, 0x00000006u, 0x00000003u, 0x00040020u, 0x00000046u,
    0x00000001u, 0x00000045u, 0x0004003bu, 0x00000046u, 0x00000047u, 0x00000001u, 0x0004003bu, 0x00000046u,
    0x0000004fu, 0x00000001u, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u,
    0x00000005u, 0x0004003bu, 0x00000014u, 0x00000015u, 0x00000007u, 0x0004003du, 0x00000007u, 0x0000000cu,
    0x0000000bu, 0x0003003eu, 0x00000009u, 0x0000000cu, 0x0004003du, 0x0000000du, 0x00000012u, 0x00000011u,
    0x0003003eu, 0x0000000fu, 0x00000012u, 0x0004003du, 0x0000000du, 0x00000017u, 0x00000016u, 0x0004003du,
    0x0000000du, 0x00000019u, 0x00000018u, 0x0004003du, 0x0000000du, 0x0000001bu, 0x0000001au, 0x0004003du,
    0x0000000du, 0x0000001du, 0x0000001cu, 0x00050051u, 0x00000006u, 0x00000020u, 0x00000017u, 0x00000000u,
    0x00050051u, 0x00000006u, 0x00000021u, 0x00000017u, 0x00000001u, 0x00050051u, 0x00000006u, 0x00000022u,
    0x00000017u, 0x00000002u, 0x00050051u, 0x00000006u, 0x00000023u, 0x00000017u, 0x00000003u, 0x00050051u,
    0x00000006u, 0x00000024u, 0x00000019u, 0x00000000u, 0x00050051u, 0x00000006u, 0x00000025u, 0x00000019u,
    0x00000001u, 0x00050051u, 0x00000006u, 0x00000026u, 0x00000019u, 0x00000002u, 0x00050051u, 0x00000006u,
    0x00000027u, 0x00000019u, 0x00000003u, 0x00050051u, 0x00000006u, 0x00000028u, 0x0000001bu, 0x00000000u,
    0x00050051u, 0x00000006u, 0x00000029u, 0x0000001bu, 0x00000001u, 0x00050051u, 0x00000006u, 0x0000002au,
    0x0000001bu, 0x00000002u, 0x00050051u, 0x00000006u, 0x0000002bu, 0x0000001bu, 0x00000003u, 0x00050051u,
    0x00000006u, 0x0000002cu, 0x0000001du, 0x00000000u, 0x00050051u, 0x00000006u, 0x0000002du, 0x0000001du,
    0x00000001u, 0x00050051u, 0x00000006u, 0x0000002eu, 0x0000001du, 0x00000002u, 0x00050051u, 0x00000006u,
    0x0000002fu, 0x0000001du, 0x00000003u, 0x00070050u, 0x0000000du, 0x00000030u, 0x00000020u, 0x00000021u,
    0x00000022u, 0x00000023u, 0x00070050u, 0x0000000du, 0x00000031u, 0x00000024u, 0x00000025u, 0x00000026u,
    0x00000027u, 0x00070050u, 0x0000000du, 0x00000032u, 0x00000028u, 0x00000029u, 0x0000002au, 0x0000002bu,
    0x00070050u, 0x0000000du, 0x00000033u, 0x0000002cu, 0x0000002du, 0x0000002eu, 0x0000002fu, 0x00070050u,
    0x00000013u, 0x00000034u, 0x00000030u, 0x00000031u, 0x00000032u, 0x00000033u, 0x0003003eu, 0x00000015u,
    0x00000034u, 0x00050041u, 0x00000040u, 0x00000041u, 0x0000003fu, 0x0000003cu, 0x0004003du, 0x00000013u,
    0x00000042u, 0x00000041u, 0x0004003du, 0x00000013u, 0x00000043u, 0x00000015u, 0x00050092u, 0x00000013u,
    0x00000044u, 0x00000042u, 0x00000043u, 0x0004003du, 0x00000045u, 0x00000048u, 0x00000047u, 0x00050051u,
    0x00000006u, 0x00000049u, 0x00000048u, 0x00000000u, 0x00050051u, 0x00000006u, 0x0000004au, 0x00000048u,
    0x00000001u, 0x00050051u, 0x00000006u, 0x0000004bu, 0x00000048u, 0x00000002u, 0x00070050u, 0x0000000du,
    0x0000004cu, 0x00000049u, 0x0000004au, 0x0000004bu, 0x0000001eu, 0x00050091u, 0x0000000du, 0x0000004du,
    0x00000044u, 0x0000004cu, 0x00050041u, 0x0000000eu, 0x0000004eu, 0x0000003au, 0x0000003cu, 0x0003003eu,
    0x0000004eu, 0x0000004du, 0x000100fdu, 0x00010038u,
};

// kGeometryVertSpv: 471 palavras, gerado por glslangValidator -V (GLSL 450).
static constexpr uint32_t kGeometryVertSpv[] = {
    0x07230203u, 0x00010000u, 0x0008000bu, 0x00000032u, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000bu,
    0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
    0x000c000fu, 0x00000000u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00000009u, 0x0000000bu, 0x0000000fu,
    0x00000011u, 0x00000018u, 0x00000028u, 0x00000031u, 0x00030003u, 0x00000002u, 0x000001c2u, 0x00040005u,
    0x00000004u, 0x6e69616du, 0x00000000u, 0x00040005u, 0x00000009u, 0x76755f76u, 0x00000000u, 0x00040005u,
    0x0000000bu, 0x76755f61u, 0x00000000u, 0x00040005u, 0x0000000fu, 0x6f635f76u, 0x00726f6cu, 0x00040005u,
    0x00000011u, 0x6f635f61u, 0x00726f6cu, 0x00060005u, 0x00000016u, 0x505f6c67u, 0x65567265u, 0x78657472u,
    0x00000000u, 0x00060006u, 0x00000016u, 0x00000000u, 0x505f6c67u, 0x7469736fu, 0x006e6f69u, 0x00070006u,
    0x00000016u, 0x00000001u, 0x505f6c67u, 0x746e696fu, 0x657a6953u, 0x00000000u, 0x00070006u, 0x00000016u,
    0x00000002u, 0x435f6c67u, 0x4470696cu, 0x61747369u, 0x0065636eu, 0x00070006u, 0x00000016u, 0x00000003u,
    0x435f6c67u, 0x446c6c75u, 0x61747369u, 0x0065636eu, 0x00030005u, 0x00000018u, 0x00000000u, 0x00060005u,
    0x0000001cu, 0x6e656353u, 0x696e5565u, 0x6d726f66u, 0x00000073u, 0x00070006u, 0x0000001cu, 0x00000000u,
    0x72705f75u, 0x63656a6fu, 0x6e6f6974u, 0x00000000u, 0x00050006u, 0x0000001cu, 0x00000001u, 0x6f6d5f75u,
    0x006c6564u, 0x00050006u, 0x0000001cu, 0x00000002u, 0x6f635f75u, 0x00726f6cu, 0x00070006u, 0x0000001cu,
    0x00000003u, 0x73755f75u, 0x65745f65u, 0x72757478u, 0x00000065u, 0x00030005u, 0x0000001eu, 0x006f6275u,
    0x00050005u, 0x00000028u, 0x6f705f61u, 0x69746973u, 0x00006e6fu, 0x00050005u, 0x00000031u, 0x6f6e5f61u,
    0x6c616d72u, 0x00000000u, 0x00040047u, 0x00000009u, 0x0000001eu, 0x00000000u, 0x00040047u, 0x0000000bu,
    0x0000001eu, 0x00000002u, 0x00040047u, 0x0000000fu, 0x0000001eu, 0x00000001u, 0x00040047u, 0x00000011u,
    0x0000001eu, 0x00000003u, 0x00030047u, 0x00000016u, 0x00000002u, 0x00050048u, 0x00000016u, 0x00000000u,
    0x0000000bu, 0x00000000u, 0x00050048u, 0x00000016u, 0x00000001u, 0x0000000bu, 0x00000001u, 0x00050048u,
    0x00000016u, 0x00000002u, 0x0000000bu, 0x00000003u, 0x00050048u, 0x00000016u, 0x00000003u, 0x0000000bu,
    0x00000004u, 0x00030047u, 0x0000001cu, 0x00000002u, 0x00040048u, 0x0000001cu, 0x00000000u, 0x00000005u,
    0x00050048u, 0x0000001cu, 0x00000000u, 0x00000007u, 0x00000010u, 0x00050048u, 0x0000001cu, 0x00000000u,
    0x00000023u, 0x00000000u, 0x00040048u, 0x0000001cu, 0x00000001u, 0x00000005u, 0x00050048u, 0x0000001cu,
    0x00000001u, 0x00000007u, 0x00000010u, 0x00050048u, 0x0000001cu, 0x00000001u, 0x00000023u, 0x00000040u,
    0x00050048u, 0x0000001cu, 0x00000002u, 0x00000023u, 0x00000080u, 0x00050048u, 0x0000001cu, 0x00000003u,
    0x00000023u, 0x00000090u, 0x00040047u, 0x0000001eu, 0x00000021u, 0x00000000u, 0x00040047u, 0x0000001eu,
    0x00000022u, 0x00000000u, 0x00040047u, 0x00000028u, 0x0000001eu, 0x00000000u, 0x00040047u, 0x00000031u,
    0x0000001eu, 0x00000001u, 0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u,
    0x00000006u, 0x00000020u, 0x00040017u, 0x00000007u, 0x00000006u, 0x00000002u, 0x00040020u, 0x00000008u,
    0x00000003u, 0x00000007u, 0x0004003bu, 0x00000008u, 0x00000009u, 0x00000003u, 0x00040020u, 0x0000000au,
    0x00000001u, 0x00000007u, 0x0004003bu, 0x0000000au, 0x0000000bu, 0x00000001u, 0x00040017u, 0x0000000du,
    0x00000006u, 0x00000004u, 0x00040020u, 0x0000000eu, 0x00000003u, 0x0000000du, 0x0004003bu, 0x0000000eu,
    0x0000000fu, 0x00000003u, 0x00040020u, 0x00000010u, 0x00000001u, 0x0000000du, 0x0004003bu, 0x00000010u,
    0x00000011u, 0x00000001u, 0x00040015u, 0x00000013u, 0x00000020u, 0x00000000u, 0x0004002bu, 0x00000013u,
    0x00000014u, 0x00000001u, 0x0004001cu, 0x00000015u, 0x00000006u, 0x00000014u, 0x0006001eu, 0x00000016u,
    0x0000000du, 0x00000006u, 0x00000015u, 0x00000015u, 0x00040020u, 0x00000017u, 0x00000003u, 0x00000016u,
    0x0004003bu, 0x00000017u, 0x00000018u, 0x00000003u, 0x00040015u, 0x00000019u, 0x00000020u, 0x00000001u,
    0x0004002bu, 0x00000019u, 0x0000001au, 0x00000000u, 0x00040018u, 0x0000001bu, 0x0000000du, 0x00000004u,
    0x0006001eu, 0x0000001cu, 0x0000001bu, 0x0000001bu, 0x0000000du, 0x00000006u, 0x00040020u, 0x0000001du,
    0x00000002u, 0x0000001cu, 0x0004003bu, 0x0000001du, 0x0000001eu, 0x00000002u, 0x00040020u, 0x0000001fu,
    0x00000002u, 0x0000001bu, 0x0004002bu, 0x00000019u, 0x00000022u, 0x00000001u, 0x00040017u, 0x00000026u,
    0x00000006u, 0x00000003u, 0x00040020u, 0x00000027u, 0x00000001u, 0x00000026u, 0x0004003bu, 0x00000027u,
    0x00000028u, 0x00000001u, 0x0004002bu, 0x00000006u, 0x0000002au, 0x3f800000u, 0x0004003bu, 0x00000027u,
    0x00000031u, 0x00000001u, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u,
    0x00000005u, 0x0004003du, 0x00000007u, 0x0000000cu, 0x0000000bu, 0x0003003eu, 0x00000009u, 0x0000000cu,
    0x0004003du, 0x0000000du, 0x00000012u, 0x00000011u, 0x0003003eu, 0x0000000fu, 0x00000012u, 0x00050041u,
    0x0000001fu, 0x00000020u, 0x0000001eu, 0x0000001au, 0x0004003du, 0x0000001bu, 0x00000021u, 0x00000020u,
    0x00050041u, 0x0000001fu, 0x00000023u, 0x0000001eu, 0x00000022u, 0x0004003du, 0x0000001bu, 0x00000024u,
    0x00000023u, 0x00050092u, 0x0000001bu, 0x00000025u, 0x00000021u, 0x00000024u, 0x0004003du, 0x00000026u,
    0x00000029u, 0x00000028u, 0x00050051u, 0x00000006u, 0x0000002bu, 0x00000029u, 0x00000000u, 0x00050051u,
    0x00000006u, 0x0000002cu, 0x00000029u, 0x00000001u, 0x00050051u, 0x00000006u, 0x0000002du, 0x00000029u,
    0x00000002u, 0x00070050u, 0x0000000du, 0x0000002eu, 0x0000002bu, 0x0000002cu, 0x0000002du, 0x0000002au,
    0x00050091u, 0x0000000du, 0x0000002fu, 0x00000025u, 0x0000002eu, 0x00050041u, 0x0000000eu, 0x00000030u,
    0x00000018u, 0x0000001au, 0x0003003eu, 0x00000030u, 0x0000002fu, 0x000100fdu, 0x00010038u,
};

// kGeometryFragSpv: 381 palavras, gerado por glslangValidator -V (GLSL 450).
static constexpr uint32_t kGeometryFragSpv[] = {
    0x07230203u, 0x00010000u, 0x0008000bu, 0x00000033u, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000bu,
    0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
    0x0008000fu, 0x00000004u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00000020u, 0x00000028u, 0x0000002bu,
    0x00030010u, 0x00000004u, 0x00000007u, 0x00030003u, 0x00000002u, 0x000001c2u, 0x00040005u, 0x00000004u,
    0x6e69616du, 0x00000000u, 0x00040005u, 0x00000009u, 0x65736162u, 0x00000000u, 0x00060005u, 0x0000000bu,
    0x6e656353u, 0x696e5565u, 0x6d726f66u, 0x00000073u, 0x00070006u, 0x0000000bu, 0x00000000u, 0x72705f75u,
    0x63656a6fu, 0x6e6f6974u, 0x00000000u, 0x00050006u, 0x0000000bu, 0x00000001u, 0x6f6d5f75u, 0x006c6564u,
    0x00050006u, 0x0000000bu, 0x00000002u, 0x6f635f75u, 0x00726f6cu, 0x00070006u, 0x0000000bu, 0x00000003u,
    0x73755f75u, 0x65745f65u, 0x72757478u, 0x00000065u, 0x00030005u, 0x0000000du, 0x006f6275u, 0x00050005u,
    0x0000001cu, 0x65745f75u, 0x72757478u, 0x00000065u, 0x00040005u, 0x00000020u, 0x76755f76u, 0x00000000u,
    0x00050005u, 0x00000028u, 0x5f74756fu, 0x6f6c6f63u, 0x00000072u, 0x00040005u, 0x0000002bu, 0x6f635f76u,
    0x00726f6cu, 0x00030047u, 0x0000000bu, 0x00000002u, 0x00040048u, 0x0000000bu, 0x00000000u, 0x00000005u,
    0x00050048u, 0x0000000bu, 0x00000000u, 0x00000007u, 0x00000010u, 0x00050048u, 0x0000000bu, 0x00000000u,
    0x00000023u, 0x00000000u, 0x00040048u, 0x0000000bu, 0x00000001u, 0x00000005u, 0x00050048u, 0x0000000bu,
    0x00000001u, 0x00000007u, 0x00000010u, 0x00050048u, 0x0000000bu, 0x00000001u, 0x00000023u, 0x00000040u,
    0x00050048u, 0x0000000bu, 0x00000002u, 0x00000023u, 0x00000080u, 0x00050048u, 0x0000000bu, 0x00000003u,
    0x00000023u, 0x00000090u, 0x00040047u, 0x0000000du, 0x00000021u, 0x00000000u, 0x00040047u, 0x0000000du,
    0x00000022u, 0x00000000u, 0x00040047u, 0x0000001cu, 0x00000021u, 0x00000000u, 0x00040047u, 0x0000001cu,
    0x00000022u, 0x00000001u, 0x00040047u, 0x00000020u, 0x0000001eu, 0x00000000u, 0x00040047u, 0x00000028u,
    0x0000001eu, 0x00000000u, 0x00040047u, 0x0000002bu, 0x0000001eu, 0x00000001u, 0x00020013u, 0x00000002u,
    0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u, 0x00000006u, 0x00000020u, 0x00040017u, 0x00000007u,
    0x00000006u, 0x00000004u, 0x00040020u, 0x00000008u, 0x00000007u, 0x00000007u, 0x00040018u, 0x0000000au,
    0x00000007u, 0x00000004u, 0x0006001eu, 0x0000000bu, 0x0000000au, 0x0000000au, 0x00000007u, 0x00000006u,
    0x00040020u, 0x0000000cu, 0x00000002u, 0x0000000bu, 0x0004003bu, 0x0000000cu, 0x0000000du, 0x00000002u,
    0x00040015u, 0x0000000eu, 0x00000020u, 0x00000001u, 0x0004002bu, 0x0000000eu, 0x0000000fu, 0x00000003u,
    0x00040020u, 0x00000010u, 0x00000002u, 0x00000006u, 0x0004002bu, 0x00000006u, 0x00000013u, 0x3f000000u,
    0x00020014u, 0x00000014u, 0x00090019u, 0x00000019u, 0x00000006u, 0x00000001u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000001u, 0x00000000u, 0x0003001bu, 0x0000001au, 0x00000019u, 0x00040020u, 0x0000001bu,
    0x00000000u, 0x0000001au, 0x0004003bu, 0x0000001bu, 0x0000001cu, 0x00000000u, 0x00040017u, 0x0000001eu,
    0x00000006u, 0x00000002u, 0x00040020u, 0x0000001fu, 0x00000001u, 0x0000001eu, 0x0004003bu, 0x0000001fu,
    0x00000020u, 0x00000001u, 0x0004002bu, 0x00000006u, 0x00000024u, 0x3f800000u, 0x0007002cu, 0x00000007u,
    0x00000025u, 0x00000024u, 0x00000024u, 0x00000024u, 0x00000024u, 0x00040020u, 0x00000027u, 0x00000003u,
    0x00000007u, 0x0004003bu, 0x00000027u, 0x00000028u, 0x00000003u, 0x00040020u, 0x0000002au, 0x00000001u,
    0x00000007u, 0x0004003bu, 0x0000002au, 0x0000002bu, 0x00000001u, 0x0004002bu, 0x0000000eu, 0x0000002eu,
    0x00000002u, 0x00040020u, 0x0000002fu, 0x00000002u, 0x00000007u, 0x00050036u, 0x00000002u, 0x00000004u,
    0x00000000u, 0x00000003u, 0x000200f8u, 0x00000005u, 0x0004003bu, 0x00000008u, 0x00000009u, 0x00000007u,
    0x0004003bu, 0x00000008u, 0x00000016u, 0x00000007u, 0x00050041u, 0x00000010u, 0x00000011u, 0x0000000du,
    0x0000000fu, 0x0004003du, 0x00000006u, 0x00000012u, 0x00000011u, 0x000500bau, 0x00000014u, 0x00000015u,
    0x00000012u, 0x00000013u, 0x000300f7u, 0x00000018u, 0x00000000u, 0x000400fau, 0x00000015u, 0x00000017u,
    0x00000023u, 0x000200f8u, 0x00000017u, 0x0004003du, 0x0000001au, 0x0000001du, 0x0000001cu, 0x0004003du,
    0x0000001eu, 0x00000021u, 0x00000020u, 0x00050057u, 0x00000007u, 0x00000022u, 0x0000001du, 0x00000021u,
    0x0003003eu, 0x00000016u, 0x00000022u, 0x000200f9u, 0x00000018u, 0x000200f8u, 0x00000023u, 0x0003003eu,
    0x00000016u, 0x00000025u, 0x000200f9u, 0x00000018u, 0x000200f8u, 0x00000018u, 0x0004003du, 0x00000007u,
    0x00000026u, 0x00000016u, 0x0003003eu, 0x00000009u, 0x00000026u, 0x0004003du, 0x00000007u, 0x00000029u,
    0x00000009u, 0x0004003du, 0x00000007u, 0x0000002cu, 0x0000002bu, 0x00050085u, 0x00000007u, 0x0000002du,
    0x00000029u, 0x0000002cu, 0x00050041u, 0x0000002fu, 0x00000030u, 0x0000000du, 0x0000002eu, 0x0004003du,
    0x00000007u, 0x00000031u, 0x00000030u, 0x00050085u, 0x00000007u, 0x00000032u, 0x0000002du, 0x00000031u,
    0x0003003eu, 0x00000028u, 0x00000032u, 0x000100fdu, 0x00010038u,
};

// kTextVertSpv: 411 palavras, gerado por glslangValidator -V (GLSL 450).
static constexpr uint32_t kTextVertSpv[] = {
    0x07230203u, 0x00010000u, 0x0008000bu, 0x0000002bu, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000bu,
    0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
    0x0009000fu, 0x00000000u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00000009u, 0x0000000bu, 0x00000013u,
    0x00000021u, 0x00030003u, 0x00000002u, 0x000001c2u, 0x00040005u, 0x00000004u, 0x6e69616du, 0x00000000u,
    0x00040005u, 0x00000009u, 0x76755f76u, 0x00000000u, 0x00040005u, 0x0000000bu, 0x76755f61u, 0x00000000u,
    0x00060005u, 0x00000011u, 0x505f6c67u, 0x65567265u, 0x78657472u, 0x00000000u, 0x00060006u, 0x00000011u,
    0x00000000u, 0x505f6c67u, 0x7469736fu, 0x006e6f69u, 0x00070006u, 0x00000011u, 0x00000001u, 0x505f6c67u,
    0x746e696fu, 0x657a6953u, 0x00000000u, 0x00070006u, 0x00000011u, 0x00000002u, 0x435f6c67u, 0x4470696cu,
    0x61747369u, 0x0065636eu, 0x00070006u, 0x00000011u, 0x00000003u, 0x435f6c67u, 0x446c6c75u, 0x61747369u,
    0x0065636eu, 0x00030005u, 0x00000013u, 0x00000000u, 0x00060005u, 0x00000017u, 0x6e656353u, 0x696e5565u,
    0x6d726f66u, 0x00000073u, 0x00070006u, 0x00000017u, 0x00000000u, 0x72705f75u, 0x63656a6fu, 0x6e6f6974u,
    0x00000000u, 0x00050006u, 0x00000017u, 0x00000001u, 0x6f6d5f75u, 0x006c6564u, 0x00050006u, 0x00000017u,
    0x00000002u, 0x6f635f75u, 0x00726f6cu, 0x00070006u, 0x00000017u, 0x00000003u, 0x73755f75u, 0x65745f65u,
    0x72757478u, 0x00000065u, 0x00030005u, 0x00000019u, 0x006f6275u, 0x00050005u, 0x00000021u, 0x6f705f61u,
    0x69746973u, 0x00006e6fu, 0x00040047u, 0x00000009u, 0x0000001eu, 0x00000000u, 0x00040047u, 0x0000000bu,
    0x0000001eu, 0x00000001u, 0x00030047u, 0x00000011u, 0x00000002u, 0x00050048u, 0x00000011u, 0x00000000u,
    0x0000000bu, 0x00000000u, 0x00050048u, 0x00000011u, 0x00000001u, 0x0000000bu, 0x00000001u, 0x00050048u,
    0x00000011u, 0x00000002u, 0x0000000bu, 0x00000003u, 0x00050048u, 0x00000011u, 0x00000003u, 0x0000000bu,
    0x00000004u, 0x00030047u, 0x00000017u, 0x00000002u, 0x00040048u, 0x00000017u, 0x00000000u, 0x00000005u,
    0x00050048u, 0x00000017u, 0x00000000u, 0x00000007u, 0x00000010u, 0x00050048u, 0x00000017u, 0x00000000u,
    0x00000023u, 0x00000000u, 0x00040048u, 0x00000017u, 0x00000001u, 0x00000005u, 0x00050048u, 0x00000017u,
    0x00000001u, 0x00000007u, 0x00000010u, 0x00050048u, 0x00000017u, 0x00000001u, 0x00000023u, 0x00000040u,
    0x00050048u, 0x00000017u, 0x00000002u, 0x00000023u, 0x00000080u, 0x00050048u, 0x00000017u, 0x00000003u,
    0x00000023u, 0x00000090u, 0x00040047u, 0x00000019u, 0x00000021u, 0x00000000u, 0x00040047u, 0x00000019u,
    0x00000022u, 0x00000000u, 0x00040047u, 0x00000021u, 0x0000001eu, 0x00000000u, 0x00020013u, 0x00000002u,
    0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u, 0x00000006u, 0x00000020u, 0x00040017u, 0x00000007u,
    0x00000006u, 0x00000002u, 0x00040020u, 0x00000008u, 0x00000003u, 0x00000007u, 0x0004003bu, 0x00000008u,
    0x00000009u, 0x00000003u, 0x00040020u, 0x0000000au, 0x00000001u, 0x00000007u, 0x0004003bu, 0x0000000au,
    0x0000000bu, 0x00000001u, 0x00040017u, 0x0000000du, 0x00000006u, 0x00000004u, 0x00040015u, 0x0000000eu,
    0x00000020u, 0x00000000u, 0x0004002bu, 0x0000000eu, 0x0000000fu, 0x00000001u, 0x0004001cu, 0x00000010u,
    0x00000006u, 0x0000000fu, 0x0006001eu, 0x00000011u, 0x0000000du, 0x00000006u, 0x00000010u, 0x00000010u,
    0x00040020u, 0x00000012u, 0x00000003u, 0x00000011u, 0x0004003bu, 0x00000012u, 0x00000013u, 0x00000003u,
    0x00040015u, 0x00000014u, 0x00000020u, 0x00000001u, 0x0004002bu, 0x00000014u, 0x00000015u, 0x00000000u,
    0x00040018u, 0x00000016u, 0x0000000du, 0x00000004u, 0x0006001eu, 0x00000017u, 0x00000016u, 0x00000016u,
    0x0000000du, 0x00000006u, 0x00040020u, 0x00000018u, 0x00000002u, 0x00000017u, 0x0004003bu, 0x00000018u,
    0x00000019u, 0x00000002u, 0x00040020u, 0x0000001au, 0x00000002u, 0x00000016u, 0x0004002bu, 0x00000014u,
    0x0000001du, 0x00000001u, 0x0004003bu, 0x0000000au, 0x00000021u, 0x00000001u, 0x0004002bu, 0x00000006u,
    0x00000023u, 0x00000000u, 0x0004002bu, 0x00000006u, 0x00000024u, 0x3f800000u, 0x00040020u, 0x00000029u,
    0x00000003u, 0x0000000du, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u,
    0x00000005u, 0x0004003du, 0x00000007u, 0x0000000cu, 0x0000000bu, 0x0003003eu, 0x00000009u, 0x0000000cu,
    0x00050041u, 0x0000001au, 0x0000001bu, 0x00000019u, 0x00000015u, 0x0004003du, 0x00000016u, 0x0000001cu,
    0x0000001bu, 0x00050041u, 0x0000001au, 0x0000001eu, 0x00000019u, 0x0000001du, 0x0004003du, 0x00000016u,
    0x0000001fu, 0x0000001eu, 0x00050092u, 0x00000016u, 0x00000020u, 0x0000001cu, 0x0000001fu, 0x0004003du,
    0x00000007u, 0x00000022u, 0x00000021u, 0x00050051u, 0x00000006u, 0x00000025u, 0x00000022u, 0x00000000u,
    0x00050051u, 0x00000006u, 0x00000026u, 0x00000022u, 0x00000001u, 0x00070050u, 0x0000000du, 0x00000027u,
    0x00000025u, 0x00000026u, 0x00000023u, 0x00000024u, 0x00050091u, 0x0000000du, 0x00000028u, 0x00000020u,
    0x00000027u, 0x00050041u, 0x00000029u, 0x0000002au, 0x00000013u, 0x00000015u, 0x0003003eu, 0x0000002au,
    0x00000028u, 0x000100fdu, 0x00010038u,
};

// kTextFragSpv: 346 palavras, gerado por glslangValidator -V (GLSL 450).
static constexpr uint32_t kTextFragSpv[] = {
    0x07230203u, 0x00010000u, 0x0008000bu, 0x0000002du, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000bu,
    0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
    0x0007000fu, 0x00000004u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00000010u, 0x00000018u, 0x00030010u,
    0x00000004u, 0x00000007u, 0x00030003u, 0x00000002u, 0x000001c2u, 0x00040005u, 0x00000004u, 0x6e69616du,
    0x00000000u, 0x00040005u, 0x00000008u, 0x68706c61u, 0x00000061u, 0x00050005u, 0x0000000cu, 0x65745f75u,
    0x72757478u, 0x00000065u, 0x00040005u, 0x00000010u, 0x76755f76u, 0x00000000u, 0x00050005u, 0x00000018u,
    0x5f74756fu, 0x6f6c6f63u, 0x00000072u, 0x00060005u, 0x0000001au, 0x6e656353u, 0x696e5565u, 0x6d726f66u,
    0x00000073u, 0x00070006u, 0x0000001au, 0x00000000u, 0x72705f75u, 0x63656a6fu, 0x6e6f6974u, 0x00000000u,
    0x00050006u, 0x0000001au, 0x00000001u, 0x6f6d5f75u, 0x006c6564u, 0x00050006u, 0x0000001au, 0x00000002u,
    0x6f635f75u, 0x00726f6cu, 0x00070006u, 0x0000001au, 0x00000003u, 0x73755f75u, 0x65745f65u, 0x72757478u,
    0x00000065u, 0x00030005u, 0x0000001cu, 0x006f6275u, 0x00040047u, 0x0000000cu, 0x00000021u, 0x00000000u,
    0x00040047u, 0x0000000cu, 0x00000022u, 0x00000001u, 0x00040047u, 0x00000010u, 0x0000001eu, 0x00000000u,
    0x00040047u, 0x00000018u, 0x0000001eu, 0x00000000u, 0x00030047u, 0x0000001au, 0x00000002u, 0x00040048u,
    0x0000001au, 0x00000000u, 0x00000005u, 0x00050048u, 0x0000001au, 0x00000000u, 0x00000007u, 0x00000010u,
    0x00050048u, 0x0000001au, 0x00000000u, 0x00000023u, 0x00000000u, 0x00040048u, 0x0000001au, 0x00000001u,
    0x00000005u, 0x00050048u, 0x0000001au, 0x00000001u, 0x00000007u, 0x00000010u, 0x00050048u, 0x0000001au,
    0x00000001u, 0x00000023u, 0x00000040u, 0x00050048u, 0x0000001au, 0x00000002u, 0x00000023u, 0x00000080u,
    0x00050048u, 0x0000001au, 0x00000003u, 0x00000023u, 0x00000090u, 0x00040047u, 0x0000001cu, 0x00000021u,
    0x00000000u, 0x00040047u, 0x0000001cu, 0x00000022u, 0x00000000u, 0x00020013u, 0x00000002u, 0x00030021u,
    0x00000003u, 0x00000002u, 0x00030016u, 0x00000006u, 0x00000020u, 0x00040020u, 0x00000007u, 0x00000007u,
    0x00000006u, 0x00090019u, 0x00000009u, 0x00000006u, 0x00000001u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000001u, 0x00000000u, 0x0003001bu, 0x0000000au, 0x00000009u, 0x00040020u, 0x0000000bu, 0x00000000u,
    0x0000000au, 0x0004003bu, 0x0000000bu, 0x0000000cu, 0x00000000u, 0x00040017u, 0x0000000eu, 0x00000006u,
    0x00000002u, 0x00040020u, 0x0000000fu, 0x00000001u, 0x0000000eu, 0x0004003bu, 0x0000000fu, 0x00000010u,
    0x00000001u, 0x00040017u, 0x00000012u, 0x00000006u, 0x00000004u, 0x00040015u, 0x00000014u, 0x00000020u,
    0x00000000u, 0x0004002bu, 0x00000014u, 0x00000015u, 0x00000003u, 0x00040020u, 0x00000017u, 0x00000003u,
    0x00000012u, 0x0004003bu, 0x00000017u, 0x00000018u, 0x00000003u, 0x00040018u, 0x00000019u, 0x00000012u,
    0x00000004u, 0x0006001eu, 0x0000001au, 0x00000019u, 0x00000019u, 0x00000012u, 0x00000006u, 0x00040020u,
    0x0000001bu, 0x00000002u, 0x0000001au, 0x0004003bu, 0x0000001bu, 0x0000001cu, 0x00000002u, 0x00040015u,
    0x0000001du, 0x00000020u, 0x00000001u, 0x0004002bu, 0x0000001du, 0x0000001eu, 0x00000002u, 0x00040017u,
    0x0000001fu, 0x00000006u, 0x00000003u, 0x00040020u, 0x00000020u, 0x00000002u, 0x00000012u, 0x00040020u,
    0x00000024u, 0x00000002u, 0x00000006u, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u,
    0x000200f8u, 0x00000005u, 0x0004003bu, 0x00000007u, 0x00000008u, 0x00000007u, 0x0004003du, 0x0000000au,
    0x0000000du, 0x0000000cu, 0x0004003du, 0x0000000eu, 0x00000011u, 0x00000010u, 0x00050057u, 0x00000012u,
    0x00000013u, 0x0000000du, 0x00000011u, 0x00050051u, 0x00000006u, 0x00000016u, 0x00000013u, 0x00000003u,
    0x0003003eu, 0x00000008u, 0x00000016u, 0x00050041u, 0x00000020u, 0x00000021u, 0x0000001cu, 0x0000001eu,
    0x0004003du, 0x00000012u, 0x00000022u, 0x00000021u, 0x0008004fu, 0x0000001fu, 0x00000023u, 0x00000022u,
    0x00000022u, 0x00000000u, 0x00000001u, 0x00000002u, 0x00060041u, 0x00000024u, 0x00000025u, 0x0000001cu,
    0x0000001eu, 0x00000015u, 0x0004003du, 0x00000006u, 0x00000026u, 0x00000025u, 0x0004003du, 0x00000006u,
    0x00000027u, 0x00000008u, 0x00050085u, 0x00000006u, 0x00000028u, 0x00000026u, 0x00000027u, 0x00050051u,
    0x00000006u, 0x00000029u, 0x00000023u, 0x00000000u, 0x00050051u, 0x00000006u, 0x0000002au, 0x00000023u,
    0x00000001u, 0x00050051u, 0x00000006u, 0x0000002bu, 0x00000023u, 0x00000002u, 0x00070050u, 0x00000012u,
    0x0000002cu, 0x00000029u, 0x0000002au, 0x0000002bu, 0x00000028u, 0x0003003eu, 0x00000018u, 0x0000002cu,
    0x000100fdu, 0x00010038u,
};
}  // namespace vk_detail

/// Contexto necessário para uploads de textura (device + fila + descritores).
/// `tracker` é preenchido por VulkanScene::upload_context(): sem ele o upload
/// cai para o caminho manual (fence esperada na própria thread), mas nunca
/// usa `vkQueueWaitIdle()`.
struct VulkanUploadContext {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout sampler_layout = VK_NULL_HANDLE;
    VulkanUploadTracker* tracker = nullptr;
    void* upload_userdata = nullptr;
    /// Alocador da cena: cria um pool maior quando o atual esgota.
    bool (*allocate_descriptor_set)(void*, VkDescriptorSetLayout,
                                    VkDescriptorPool*, VkDescriptorSet*) = nullptr;
};

/// UBO std140. Tamanho = 64 + 64 + 16 + 16 = 160 bytes.
struct alignas(16) SceneUniforms {
    Mat4 projection = mat4_identity();
    Mat4 model = mat4_identity();
    float color[4] = {1, 1, 1, 1};
    float use_texture = 0.0f;
    float padding[3] = {0, 0, 0};
};

/// Textura Vulkan: PNG/JPEG/WebP/SVG via decode_asset_image(), upload com
/// staging buffer (HOST_VISIBLE → DEVICE_LOCAL) e sampler linear/clamp.
///
/// O upload é assíncrono quando o contexto vem da cena: a cópia sinaliza um
/// semáforo esperado pelo próximo submit de render e aposenta a imagem
/// anterior no tracker. Assim trocar frames de WebP/SMIL não bloqueia a fila
/// nem corre com frames in-flight.
class TextureVk {
public:
    TextureVk() = default;
    ~TextureVk() { destroy(); }
    TextureVk(const TextureVk&) = delete;
    TextureVk& operator=(const TextureVk&) = delete;

    /// Mesmo decode usado por Texture (GL): formato detectado por magic bytes.
    bool load(const VulkanUploadContext& ctx, const std::string& path,
              int svg_w = 0, int svg_h = 0) {
        DecodedImage image;
        if (!decode_asset_image(path, image, svg_w, svg_h)) return false;
        return load_rgba(ctx, image.rgba.data(), image.width, image.height);
    }

    bool load_svg_asset(const VulkanUploadContext& ctx, const std::string& path,
                        int width = 0, int height = 0) {
        return load(ctx, path, width, height);
    }

    /// Sobe RGBA já decodificado (usado também pelo atlas de fonte).
    bool load_rgba(const VulkanUploadContext& ctx, const uint8_t* rgba,
                   int width, int height) {
        release(&ctx);  // aposenta a imagem anterior sem parar a fila
        if (!ctx.device || !ctx.physical_device || !ctx.queue || !ctx.command_pool ||
            !rgba || width <= 0 || height <= 0) {
            return false;
        }
        const size_t row_bytes = static_cast<size_t>(width) * 4u;
        if (row_bytes == 0 ||
            static_cast<size_t>(height) > std::numeric_limits<size_t>::max() / row_bytes) {
            return false;
        }
        const size_t image_bytes = row_bytes * static_cast<size_t>(height);

        device_ = ctx.device;
        VkBuffer staging_buffer = VK_NULL_HANDLE;
        VkDeviceMemory staging_memory = VK_NULL_HANDLE;
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore ready = VK_NULL_HANDLE;
        bool submitted = false;
        auto release_staging = [&]() {
            if (command_buffer) {
                vkFreeCommandBuffers(device_, ctx.command_pool, 1, &command_buffer);
                command_buffer = VK_NULL_HANDLE;
            }
            if (staging_memory) {
                vkFreeMemory(device_, staging_memory, nullptr);
                staging_memory = VK_NULL_HANDLE;
            }
            if (staging_buffer) {
                vkDestroyBuffer(device_, staging_buffer, nullptr);
                staging_buffer = VK_NULL_HANDLE;
            }
            if (ready) {
                vkDestroySemaphore(device_, ready, nullptr);
                ready = VK_NULL_HANDLE;
            }
            if (fence) {
                vkDestroyFence(device_, fence, nullptr);
                fence = VK_NULL_HANDLE;
            }
        };

        try {
            VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            image_info.imageType = VK_IMAGE_TYPE_2D;
            image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
            image_info.extent = {static_cast<uint32_t>(width),
                                 static_cast<uint32_t>(height), 1};
            image_info.mipLevels = 1;
            image_info.arrayLayers = 1;
            image_info.samples = VK_SAMPLE_COUNT_1_BIT;
            image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
            image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            vk_check(vkCreateImage(device_, &image_info, nullptr, &image_),
                     "vkCreateImage(texture)");

            VkMemoryRequirements requirements{};
            vkGetImageMemoryRequirements(device_, image_, &requirements);
            VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            alloc.allocationSize = requirements.size;
            alloc.memoryTypeIndex = vk_detail::find_memory_type(
                ctx.physical_device, requirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            vk_check(vkAllocateMemory(device_, &alloc, nullptr, &image_memory_),
                     "vkAllocateMemory(texture)");
            vk_check(vkBindImageMemory(device_, image_, image_memory_, 0),
                     "vkBindImageMemory(texture)");

            VkBufferCreateInfo staging_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            staging_info.size = image_bytes;
            staging_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            staging_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            vk_check(vkCreateBuffer(device_, &staging_info, nullptr, &staging_buffer),
                     "vkCreateBuffer(staging)");

            VkMemoryRequirements staging_requirements{};
            vkGetBufferMemoryRequirements(device_, staging_buffer, &staging_requirements);
            VkMemoryAllocateInfo staging_alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            staging_alloc.allocationSize = staging_requirements.size;
            staging_alloc.memoryTypeIndex = vk_detail::find_memory_type(
                ctx.physical_device, staging_requirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            vk_check(vkAllocateMemory(device_, &staging_alloc, nullptr, &staging_memory),
                      "vkAllocateMemory(staging)");
            vk_check(vkBindBufferMemory(device_, staging_buffer, staging_memory, 0),
                     "vkBindBufferMemory(staging)");

            void* mapped = nullptr;
            vk_check(vkMapMemory(device_, staging_memory, 0, image_bytes, 0, &mapped),
                     "vkMapMemory(staging)");
            if (!mapped) throw VulkanError("vkMapMemory(staging) retornou nulo");
            uint8_t* destination = static_cast<uint8_t*>(mapped);
            // bufferRowLength = 0 exige linhas contíguas. rowPitch de uma
            // imagem TILING_OPTIMAL aqui deixaria linhas vazias (sprite
            // achatado). Espelha as linhas para casar com a convenção GL.
            for (int y = 0; y < height; ++y) {
                const size_t source_row = static_cast<size_t>(height - 1 - y);
                std::memcpy(destination + row_bytes * static_cast<size_t>(y),
                            rgba + row_bytes * source_row, row_bytes);
            }
            vkUnmapMemory(device_, staging_memory);

            VkCommandBufferAllocateInfo command_alloc{
                VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            command_alloc.commandPool = ctx.command_pool;
            command_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            command_alloc.commandBufferCount = 1;
            vk_check(vkAllocateCommandBuffers(device_, &command_alloc, &command_buffer),
                     "vkAllocateCommandBuffers(staging)");

            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vk_check(vkBeginCommandBuffer(command_buffer, &begin),
                     "vkBeginCommandBuffer(staging)");
            vk_detail::transition_image_layout(command_buffer, image_,
                                               VK_IMAGE_LAYOUT_UNDEFINED,
                                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy copy{};
            copy.bufferOffset = 0;
            copy.bufferRowLength = 0;
            copy.bufferImageHeight = 0;
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.mipLevel = 0;
            copy.imageSubresource.baseArrayLayer = 0;
            copy.imageSubresource.layerCount = 1;
            copy.imageExtent = {static_cast<uint32_t>(width),
                                static_cast<uint32_t>(height), 1};
            vkCmdCopyBufferToImage(command_buffer, staging_buffer, image_,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            vk_detail::transition_image_layout(command_buffer, image_,
                                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            vk_check(vkEndCommandBuffer(command_buffer), "vkEndCommandBuffer(staging)");

            VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            vk_check(vkCreateFence(device_, &fence_info, nullptr, &fence),
                     "vkCreateFence(upload)");
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &command_buffer;
            if (ctx.tracker) {
                VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
                vk_check(vkCreateSemaphore(device_, &semaphore_info, nullptr, &ready),
                         "vkCreateSemaphore(upload)");
                submit.signalSemaphoreCount = 1;
                submit.pSignalSemaphores = &ready;
            }
            vk_check(vkQueueSubmit(ctx.queue, 1, &submit, fence),
                     "vkQueueSubmit(staging)");
            submitted = true;

            if (ctx.tracker) {
                // A cena espera `ready` antes de amostrar e libera os handles
                // quando a fence sinaliza. Nenhuma espera de fila aqui.
                VulkanUploadTracker::Record record;
                record.device = device_;
                record.command_pool = ctx.command_pool;
                record.command = command_buffer;
                record.staging_buffer = staging_buffer;
                record.staging_memory = staging_memory;
                record.fence = fence;
                record.ready = ready;
                ctx.tracker->track_upload(record);
                upload_fence_ = fence;
                tracker_ = ctx.tracker;
                command_buffer = VK_NULL_HANDLE;
                staging_buffer = VK_NULL_HANDLE;
                staging_memory = VK_NULL_HANDLE;
                fence = VK_NULL_HANDLE;
                ready = VK_NULL_HANDLE;
            } else {
                // Caminho manual (sem cena): espera só esta fence, nunca a fila.
                vk_check(vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX),
                         "vkWaitForFences(upload)");
                release_staging();
            }

            VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view_info.image = image_;
            view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
            view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            view_info.subresourceRange.levelCount = 1;
            view_info.subresourceRange.layerCount = 1;
            vk_check(vkCreateImageView(device_, &view_info, nullptr, &image_view_),
                     "vkCreateImageView(texture)");

            VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            sampler_info.magFilter = VK_FILTER_LINEAR;
            sampler_info.minFilter = VK_FILTER_LINEAR;
            sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sampler_info.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
            sampler_info.unnormalizedCoordinates = VK_FALSE;
            sampler_info.compareEnable = VK_FALSE;
            sampler_info.compareOp = VK_COMPARE_OP_ALWAYS;
            sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            sampler_info.minLod = 0.0f;
            sampler_info.maxLod = 0.0f;
            vk_check(vkCreateSampler(device_, &sampler_info, nullptr, &sampler_),
                     "vkCreateSampler(texture)");

            if (!ctx.descriptor_pool || !ctx.sampler_layout) {
                throw VulkanError("TextureVk: pool/layout de descritores inválido");
            }
            VkDescriptorPool allocated_pool = VK_NULL_HANDLE;
            if (ctx.allocate_descriptor_set &&
                ctx.allocate_descriptor_set(ctx.upload_userdata, ctx.sampler_layout,
                                            &allocated_pool, &descriptor_)) {
                descriptor_pool_ = allocated_pool;
            } else {
                VkDescriptorSetAllocateInfo descriptor_alloc{
                    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                descriptor_alloc.descriptorPool = ctx.descriptor_pool;
                descriptor_alloc.descriptorSetCount = 1;
                descriptor_alloc.pSetLayouts = &ctx.sampler_layout;
                vk_check(vkAllocateDescriptorSets(device_, &descriptor_alloc,
                                                  &descriptor_),
                         "vkAllocateDescriptorSets(texture)");
                descriptor_pool_ = ctx.descriptor_pool;
            }

            VkDescriptorImageInfo descriptor_image{};
            descriptor_image.sampler = sampler_;
            descriptor_image.imageView = image_view_;
            descriptor_image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = descriptor_;
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &descriptor_image;
            vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

            width_ = width;
            height_ = height;
            return true;
        } catch (const std::exception& error) {
            // No modo manual a fence já foi esperada; com tracker, os handles
            // de staging ficam com a cena e a imagem é aposentada nela.
            if (submitted && !ctx.tracker && fence) {
                vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);
            }
            release_staging();
            release(&ctx);
            __android_log_print(ANDROID_LOG_WARN, "nativeapp-vk",
                                "TextureVk: %s", error.what());
            return false;
        }
    }

    bool valid() const { return image_view_ != VK_NULL_HANDLE && descriptor_ != VK_NULL_HANDLE; }
    int width() const { return width_; }
    int height() const { return height_; }
    VkImageView view() const { return image_view_; }
    VkSampler sampler() const { return sampler_; }
    VkDescriptorSet descriptor() const { return descriptor_; }

    void destroy() { release(nullptr); }
    void destroy(const VulkanUploadContext& ctx) { release(&ctx); }

    /// Device lost: descarta os handles sem chamar Vulkan no device perdido.
    void release_without_vulkan() {
        device_ = VK_NULL_HANDLE;
        upload_fence_ = VK_NULL_HANDLE;
        tracker_ = nullptr;
        image_ = VK_NULL_HANDLE;
        image_memory_ = VK_NULL_HANDLE;
        image_view_ = VK_NULL_HANDLE;
        sampler_ = VK_NULL_HANDLE;
        descriptor_ = VK_NULL_HANDLE;
        descriptor_pool_ = VK_NULL_HANDLE;
        width_ = height_ = 0;
    }

private:
    void release(const VulkanUploadContext* ctx) {
        if (!device_) return;
        VulkanUploadTracker* tracker = ctx ? ctx->tracker : tracker_;
        if (upload_fence_ && tracker) {
            VulkanUploadTracker::Record record;
            record.device = device_;
            record.fence = upload_fence_;
            record.owns_fence = false;  // o upload que criou a fence a possui
            record.descriptor_pool = descriptor_pool_;
            record.descriptor = descriptor_;
            record.sampler = sampler_;
            record.view = image_view_;
            record.image = image_;
            record.image_memory = image_memory_;
            tracker->retire_texture(record);
        } else {
            if (upload_fence_) {
                vkWaitForFences(device_, 1, &upload_fence_, VK_TRUE, UINT64_MAX);
            }
            if (descriptor_ && descriptor_pool_) {
                vkFreeDescriptorSets(device_, descriptor_pool_, 1, &descriptor_);
            }
            if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
            if (image_view_) vkDestroyImageView(device_, image_view_, nullptr);
            if (image_) vkDestroyImage(device_, image_, nullptr);
            if (image_memory_) vkFreeMemory(device_, image_memory_, nullptr);
            if (upload_fence_) vkDestroyFence(device_, upload_fence_, nullptr);
        }
        release_without_vulkan();
    }

    VkDevice device_ = VK_NULL_HANDLE;
    VkFence upload_fence_ = VK_NULL_HANDLE;
    VulkanUploadTracker* tracker_ = nullptr;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory image_memory_ = VK_NULL_HANDLE;
    VkImageView image_view_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    int width_ = 0;
    int height_ = 0;
};

// ============================================================================
// VulkanScene — implementa Scene
// ============================================================================

class VulkanScene : public Scene {
public:
    static constexpr int kMaxFramesInFlight = 2;
    /// Capacidade inicial do UBO; cresce sob demanda em prepare_frame().
    static constexpr size_t kMaxDrawSlots = 64;
    static constexpr size_t kMaxTextures = 64;
    /// Reserva: white + fonte + texturas externas (WebP/SVG animados).
    static constexpr size_t kTextureDescriptorReserve = 4;
    static constexpr size_t kUboSetReserve = 4;

    VulkanScene() = default;
    ~VulkanScene() override { destroy(); }
    VulkanScene(const VulkanScene&) = delete;
    VulkanScene& operator=(const VulkanScene&) = delete;

    void init(ANativeActivity* activity) {
        if (initialized_) return;
        const bool recovering = device_lost_;
        activity_ = activity;
        Assets::instance(activity).set_activity(activity);
        try {
            create_instance();
            pick_physical_device();
            create_device();
            create_shared_gpu_resources();
        } catch (...) {
            destroy_shared_gpu_resources();
            if (device_) { vkDestroyDevice(device_, nullptr); device_ = VK_NULL_HANDLE; }
            if (instance_) { vkDestroyInstance(instance_, nullptr); instance_ = VK_NULL_HANDLE; }
            physical_device_ = VK_NULL_HANDLE;
            queue_ = VK_NULL_HANDLE;
            throw;
        }
        initialized_ = true;
        if (recovering) {
            device_lost_ = false;
            needs_asset_reload_ = false;
            __android_log_print(ANDROID_LOG_WARN, "nativeapp-vk",
                                "dispositivo Vulkan reinicializado");
        }
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
        if (!projection_set_) {
            const int w = ANativeWindow_getWidth(window_);
            const int h = ANativeWindow_getHeight(window_);
            if (w > 0 && h > 0) projection_ = vk_detail::ortho2d_screen(
                static_cast<float>(w), static_cast<float>(h));
        }
        rebuild_custom_pipeline();
        swapchain_ok_ = true;
    }

    void invalidate() {
        if (!initialized_) return;
        if (device_ && vkDeviceWaitIdle(device_) == VK_ERROR_DEVICE_LOST) {
            handle_device_loss();
            return;
        }
        pipelines_.clear();
        custom_pipeline_.destroy();
        for (auto& items : frame_items_) items.clear();
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

    /// Habilita as camadas reais de validação. Deve ser chamado ANTES de
    /// init(); se a camada não existir no dispositivo, a cena continua com
    /// degradation explícita no logcat e validation_enabled() == false.
    void set_validation(bool enabled) {
        validation_ = enabled;
        if (enabled && initialized_ && !validation_active_) {
            __android_log_print(ANDROID_LOG_WARN, "nativeapp-vk",
                                "set_validation(true) após init(): a instância já existe; "
                                "chame destroy() e init() para habilitar as layers");
        }
    }

    /// true quando VK_LAYER_KHRONOS_validation está efetivamente ativa.
    bool validation_enabled() const { return validation_active_; }

    /// Mesma convenção de coordenadas de tela do GLES2Scene.
    void resize(int width, int height) override {
        if (width > 0 && height > 0) {
            projection_ = vk_detail::ortho2d_screen(static_cast<float>(width),
                                                    static_cast<float>(height));
            projection_set_ = true;
        }
        if (initialized_ && window_) recreate_swapchain();
    }

    void set_projection(const Mat4& projection) {
        projection_ = projection;
        projection_set_ = true;
    }

    void begin_frame() override {}  // clear acontece no render pass
    void present() override { render(); }

    /// Modo legado equivalente a GLES2Scene::render(): com `true`, render()
    /// ignora itens enfileirados, limpa e desenha o triângulo em NDC com a
    /// projeção identidade (mesmo que set_projection() tenha sido usado).
    void set_legacy_render(bool enabled) { legacy_render_ = enabled; }
    bool legacy_render() const { return legacy_render_; }
    /// Atalho one-shot: render_legacy() executa o modo legado sem alterar o
    /// estado persistente (definido junto com render()).

    /// Configura um pipeline de geometria externo (modo legado). Os sets
    /// embutidos (UBO de cena + sampler) continuam ligados ao layout, então
    /// um SPIR-V que declare SceneUniforms recebe Material/model/color como
    /// no caminho embutido. A configuração sobrevive a resize()/invalidate().
    void set_pipeline(PipelineConfig config) {
        set_pipeline(std::move(config), 0);
    }

    /// Caminho novo para shaders de usuário: acrescenta o set 2 com bindings
    /// registrados por register_custom_uniforms(), sem tocar nos sets 0/1.
    void set_pipeline(PipelineConfig config, CustomUniformHandle custom_uniforms) {
        if (!initialized_) return;
        if (custom_uniforms && custom_uniforms_.find(custom_uniforms) ==
                                   custom_uniforms_.end()) {
            __android_log_print(ANDROID_LOG_WARN, "nativeapp-vk",
                                "handle de uniforms customizados %zu não registrado",
                                custom_uniforms);
            custom_uniforms = 0;
        }
        if (swapchain_) config.color_format = color_format_;
        config.depth_format = depth_format_;
        config.text_layout = false;
        config.custom_uniforms = custom_uniforms;
        pipeline_config_ = std::move(config);
        active_custom_uniforms_ = custom_uniforms;
        has_custom_pipeline_ = true;
        rebuild_custom_pipeline();
    }

    /// true depois de VK_ERROR_DEVICE_LOST; a cena nao renderiza ate init().
    bool device_lost() const { return device_lost_; }
    /// true quando fontes/texturas precisam ser recarregadas pelo app.
    bool needs_asset_reload() const { return needs_asset_reload_; }

    /// Contexto para TextureVk::load_rgba() de texturas externas (frames de
    /// WebP animado / SVG SMIL). O upload é assíncrono: o semáforo sinalizado
    /// é esperado pelo próximo submit de render() antes da amostragem.
    VulkanUploadContext upload_context() const { return make_upload_context(); }

    /// Pré-carrega uma textura de assets/ no cache por caminho. O cache não
    /// tem teto rígido: o pool de descritores cresce sob demanda.
    bool load_texture(const std::string& path, int svg_w = 0, int svg_h = 0) {
        return ensure_texture(path, svg_w, svg_h) != nullptr;
    }

    /// Invalidação explícita: remove uma entrada do cache (ou todas) e aposenta
    /// a imagem com segurança. Nunca descarta silenciosamente por limite.
    // invalidate_texture()/clear_textures()/reload_texture()/texture_count()
    // estão definidas mais adiante nesta classe (api de cache explícita).
    std::size_t texture_count() const { return textures_.size(); }

    /// Enfileira uma mesh. O upload e a gravação dos comandos occur em render().
    void draw(const MeshData& mesh, const Material& material,
              const float* model = nullptr) override {
        enqueue_draw(mesh, material, model, nullptr);
    }

    /// Equivalente a GLES2Scene::draw_with_texture(): usa uma TextureVk externa
    /// (por exemplo o frame atual de AnimatedImage) em vez do cache por caminho.
    void draw_with_texture(const MeshData& mesh, const Material& material,
                           const float* model, const TextureVk* texture_override) {
        enqueue_draw(mesh, material, model, texture_override);
    }

    // draw_instanced (...) e a API de uniforms customizados estão definidas
    // mais adiante nesta classe; a declaração pública única evita duplicação.

    /// Carrega uma fonte TTF/OTF de assets/ e sobe o atlas RGBA para a GPU.
    bool load_font(const std::string& path, int pixel_size = 24) {
        if (!initialized_) return false;
        FontAtlas atlas;
        if (!atlas.load(path, pixel_size) || !atlas.valid() ||
            atlas.atlas_size() <= 0 || !atlas.atlas()) {
            return false;
        }
        const size_t side = static_cast<size_t>(atlas.atlas_size());
        if (side > std::numeric_limits<size_t>::max() / 4) return false;
        const size_t count = side * side;
        if (count > std::numeric_limits<size_t>::max() / 4) return false;
        std::vector<uint8_t> rgba(count * 4, 255);
        const uint8_t* pixels = atlas.atlas();
        for (size_t i = 0; i < count; ++i) rgba[i * 4 + 3] = pixels[i];
        if (!font_texture_.load_rgba(upload_context(), rgba.data(),
                                     atlas.atlas_size(), atlas.atlas_size())) {
            return false;
        }
        font_ = std::make_unique<FontAtlas>(std::move(atlas));
        return true;
    }

    /// Desenha texto com a fonte carregada (coordenadas de tela, como no GL).
    void draw_text(const std::string& text, float x, float y, float scale = 1.0f,
                   float r = 1.0f, float g = 1.0f, float b = 1.0f, float a = 1.0f) {
        if (!initialized_ || !swapchain_ok_ || !has_font()) return;
        DrawItem item;
        item.text = true;
        item.depth_test = false;  // texto é 2D sobreposto
        item.blend = true;
        item.use_texture = 1.0f;
        item.descriptor = font_texture_.descriptor();
        item.color[0] = r; item.color[1] = g; item.color[2] = b; item.color[3] = a;

        // Mesma convenção de GLES2Scene::draw_text(): x/y são coordenadas de
        // tela (origem no topo) e FontAtlas devolve o quad já nesse espaço.
        // Qualquer "baseline = height - y" aqui era um hack para compensar a
        // projeção errada e não deve permanecer no backend.
        float cx = x;
        float cy = y;
        item.glyphs.reserve(text.size() * 6);
        for (char c : text) {
            const GlyphQuad quad = font_->quad(c, cx, cy, scale);
            if (quad.x1 <= quad.x0) continue;
            // O atlas é enviado na ordem de linhas do stb_truetype (linha 0
            // no topo), mas neste backend Vulkan t=0 amostra a linha inferior,
            // como no GL Texture2D. Espelhar V aqui devolve o glyph à
            // orientação do GLES2Scene sem mexer na geometria nem em font.hpp.
            const float v0 = 1.0f - quad.v0;
            const float v1 = 1.0f - quad.v1;
            item.glyphs.push_back({quad.x0, quad.y0, quad.u0, v0});
            item.glyphs.push_back({quad.x1, quad.y0, quad.u1, v0});
            item.glyphs.push_back({quad.x1, quad.y1, quad.u1, v1});
            item.glyphs.push_back({quad.x0, quad.y0, quad.u0, v0});
            item.glyphs.push_back({quad.x1, quad.y1, quad.u1, v1});
            item.glyphs.push_back({quad.x0, quad.y1, quad.u0, v1});
        }
        if (item.glyphs.empty()) return;
        pending_items_.push_back(std::move(item));
    }

    bool has_font() const { return font_ != nullptr && font_texture_.valid(); }

    void render() {
        if (device_lost_ || !initialized_ || !swapchain_ok_ || !window_) return;

        if (legacy_render_) {
            // Equivale a GLES2Scene::render(): desenha a mesh padrão em NDC,
            // ignorando projeção customizada e itens enfileirados.
            pending_items_.clear();
            enqueue_draw(MeshData::triangle(), Material{}, nullptr, nullptr);
        }

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

        bool frames_idle = true;
        for (VkFence fence : in_flight_fences_) {
            if (fence && vkGetFenceStatus(device_, fence) != VK_SUCCESS) {
                frames_idle = false;
                break;
            }
        }
        upload_tracker_.reap(frames_idle);

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
        // dona dela ANTES de resetar a fence deste frame.
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

        prepare_frame(frame);

        vk_check(vkResetCommandBuffer(command_buffers_[frame], 0),
                 "vkResetCommandBuffer");
        record_command_buffer(command_buffers_[frame], image_index, frame);

        // Uploads de textura sinalizam semáforos próprios: o fragment shader
        // não amostra uma imagem antes da cópia terminar, sem parar a fila.
        std::vector<VkSemaphore> waits = upload_tracker_.pending_semaphores();
        std::vector<VkPipelineStageFlags> wait_stages(
            waits.size(), VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        waits.insert(waits.begin(), image_available_[frame]);
        wait_stages.insert(wait_stages.begin(),
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = static_cast<uint32_t>(waits.size());
        submit.pWaitSemaphores = waits.data();
        submit.pWaitDstStageMask = wait_stages.data();
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_buffers_[frame];
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &render_finished_[frame];
        result = vkQueueSubmit(queue_, 1, &submit, in_flight_fences_[frame]);
        if (result == VK_ERROR_DEVICE_LOST) { handle_device_loss(); return; }
        vk_check(result, "vkQueueSubmit");
        upload_tracker_.mark_consumed();
        upload_tracker_.note_submission();

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

    void render_legacy() {
        const bool saved = legacy_render_;
        legacy_render_ = true;
        try {
            render();
        } catch (...) {
            legacy_render_ = saved;
            throw;
        }
        legacy_render_ = saved;
    }

    void destroy() {
        if (!initialized_) return;
        invalidate();
        destroy_shared_gpu_resources();
        if (render_pass_) {
            vkDestroyRenderPass(device_, render_pass_, nullptr);
            render_pass_ = VK_NULL_HANDLE;
        }
        if (device_) { vkDestroyDevice(device_, nullptr); device_ = VK_NULL_HANDLE; }
        destroy_debug_messenger();
        if (instance_) { vkDestroyInstance(instance_, nullptr); instance_ = VK_NULL_HANDLE; }
        validation_active_ = false;
        upload_tracker_.set_device(VK_NULL_HANDLE);
        queue_ = VK_NULL_HANDLE;
        physical_device_ = VK_NULL_HANDLE;
        has_custom_pipeline_ = false;
        active_custom_uniforms_ = 0;
        custom_pipeline_set_count_ = 0;
        legacy_render_ = false;
        projection_set_ = false;
        projection_ = mat4_identity();
        swapchain_ok_ = false;
        current_frame_ = 0;
        initialized_ = false;
    }

private:
    struct PipelineKey {
        bool depth_test = true;
        bool blend = true;
        bool text = false;
        bool instanced = false;
        Topology topology = Topology::Triangles;

        bool operator<(const PipelineKey& other) const {
            if (text != other.text) return text < other.text;
            if (instanced != other.instanced) return instanced < other.instanced;
            if (depth_test != other.depth_test) return depth_test < other.depth_test;
            if (blend != other.blend) return blend < other.blend;
            return static_cast<int>(topology) < static_cast<int>(other.topology);
        }
    };

    struct DrawItem {
        MeshData mesh;
        std::vector<vk_detail::GlyphVertex> glyphs;
        GpuBuffer buffers;
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
        VkDeviceSize uniform_offset = 0;
        float color[4] = {1, 1, 1, 1};
        float use_texture = 0.0f;
        Mat4 model = mat4_identity();
        bool depth_test = true;
        bool blend = true;
        bool text = false;
        bool instanced = false;
        CustomUniformHandle custom_uniforms = 0;
        std::vector<Mat4> instances;
        Topology topology = Topology::Triangles;
    };

    /// Estado de um conjunto de uniforms registrado pelo app. Cada binding
    /// UBO tem um buffer por frame para não sobrescrever frames in-flight.
    struct CustomUniformGroup {
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        struct Entry {
            CustomUniformBinding binding;
            std::size_t buffer_size = 0;
            std::array<VkBuffer, kMaxFramesInFlight> buffers{};
            std::array<VkDeviceMemory, kMaxFramesInFlight> memories{};
            std::array<void*, kMaxFramesInFlight> mapped{};
        };
        std::vector<Entry> entries;
        std::array<VkDescriptorSet, kMaxFramesInFlight> sets{};
        std::array<VkDescriptorPool, kMaxFramesInFlight> pools{};
    };

    void enqueue_draw(const MeshData& mesh, const Material& material,
                      const float* model, const TextureVk* texture_override) {
        if (!initialized_ || !swapchain_ok_ || mesh.vertices.empty()) return;
        DrawItem item;
        item.mesh = mesh;
        item.topology = mesh.topology;
        // Convencao unica: UVs de MeshData e de malhas customizadas sao os
        // mesmos do GL. A orientacao de amostragem e resolvida no upload.
        for (int i = 0; i < 4; ++i) item.color[i] = material.color[i];
        if (model) {
            std::memcpy(item.model.data(), model, sizeof(float) * 16);
        }
        item.depth_test = material.depth_test;
        item.blend = material.blend;
        // Texto usa sempre os layouts embutidos; geometria pode usar o set 2.
        if (has_custom_pipeline_) item.custom_uniforms = active_custom_uniforms_;

        if (texture_override && texture_override->valid()) {
            item.descriptor = texture_override->descriptor();
            item.use_texture = 1.0f;
        } else if (!material.texture.empty()) {
            if (TextureVk* texture = ensure_texture(material.texture)) {
                item.descriptor = texture->descriptor();
                item.use_texture = 1.0f;
            } else {
                item.descriptor = white_texture_.descriptor();
            }
        } else {
            item.descriptor = white_texture_.descriptor();
        }
        pending_items_.push_back(std::move(item));
    }

    /// Device loss invalida tudo; a cena volta ao estado pré-init() e o app
    /// deve chamar init() novamente e recarregar fontes/texturas.
    void handle_device_loss() {
        if (device_lost_) return;
        __android_log_print(ANDROID_LOG_ERROR, "nativeapp-vk",
                            "dispositivo Vulkan perdido: cena invalidada; "
                            "chame init() novamente e recarregue fontes/texturas");
        device_lost_ = true;
        needs_asset_reload_ = true;
        swapchain_ok_ = false;
        reset_after_device_loss();
    }

    /// Limpa apenas estado hospedeiro: em device lost, chamadas Vulkan sobre
    /// o device antigo nao sao seguras. Handles sao descartados, nao destruidos.
    void reset_after_device_loss() {
        pipelines_.clear();
        custom_pipeline_.destroy();
        pending_items_.clear();
        for (auto& items : frame_items_) items.clear();
        upload_tracker_.reset();
        for (auto& entry : textures_) entry.second->release_without_vulkan();
        textures_.clear();
        font_.reset();
        font_texture_.release_without_vulkan();
        white_texture_.release_without_vulkan();
        for (auto& group : custom_uniforms_) {
            for (auto& entry : group.second.entries) {
                for (size_t frame = 0; frame < kMaxFramesInFlight; ++frame) {
                    entry.buffers[frame] = VK_NULL_HANDLE;
                    entry.memories[frame] = VK_NULL_HANDLE;
                    entry.mapped[frame] = nullptr;
                }
            }
            for (size_t frame = 0; frame < kMaxFramesInFlight; ++frame) {
                group.second.sets[frame] = VK_NULL_HANDLE;
                group.second.pools[frame] = VK_NULL_HANDLE;
            }
            group.second.layout = VK_NULL_HANDLE;
        }
        custom_uniforms_.clear();
        active_custom_uniforms_ = 0;
        custom_pipeline_set_count_ = 0;
        destroy_debug_messenger();
        validation_active_ = false;
        in_flight_fences_.clear();
        image_available_.clear();
        render_finished_.clear();
        images_in_flight_.clear();
        command_buffers_.clear();
        framebuffers_.clear();
        image_views_.clear();
        images_.clear();
        command_pool_ = VK_NULL_HANDLE;
        render_pass_ = VK_NULL_HANDLE;
        depth_view_ = VK_NULL_HANDLE;
        depth_image_ = VK_NULL_HANDLE;
        depth_memory_ = VK_NULL_HANDLE;
        swapchain_ = VK_NULL_HANDLE;
        surface_ = VK_NULL_HANDLE;
        window_ = VK_NULL_HANDLE;
        image_count_ = 0;
        upload_pool_ = VK_NULL_HANDLE;
        ubo_layout_ = VK_NULL_HANDLE;
        sampler_layout_ = VK_NULL_HANDLE;
        descriptor_pool_ = VK_NULL_HANDLE;
        descriptor_pools_.clear();
        descriptor_capacity_ = 0;
        ubo_descriptor_capacity_ = 0;
        sampler_descriptor_capacity_ = 0;
        for (size_t frame = 0; frame < kMaxFramesInFlight; ++frame) {
            uniform_buffers_[frame] = VK_NULL_HANDLE;
            uniform_memories_[frame] = VK_NULL_HANDLE;
            uniform_mapped_[frame] = nullptr;
            ubo_sets_[frame] = VK_NULL_HANDLE;
            ubo_pools_[frame] = VK_NULL_HANDLE;
        }
        uniform_capacity_ = 0;
        device_ = VK_NULL_HANDLE;
        instance_ = VK_NULL_HANDLE;
        physical_device_ = VK_NULL_HANDLE;
        queue_ = VK_NULL_HANDLE;
        initialized_ = false;
        current_frame_ = 0;
    }

    VulkanUploadContext make_upload_context() const {
        VulkanUploadContext ctx;
        ctx.device = device_;
        ctx.physical_device = physical_device_;
        ctx.queue = queue_;
        ctx.command_pool = upload_pool_;
        ctx.descriptor_pool = descriptor_pool_;
        ctx.sampler_layout = sampler_layout_;
        ctx.tracker = &upload_tracker_;
        ctx.upload_userdata = const_cast<VulkanScene*>(this);
        ctx.allocate_descriptor_set = &VulkanScene::allocate_descriptor_set_thunk;
        return ctx;
    }

    static bool allocate_descriptor_set_thunk(void* userdata,
                                              VkDescriptorSetLayout layout,
                                              VkDescriptorPool* pool_out,
                                              VkDescriptorSet* set_out) {
        auto* scene = static_cast<VulkanScene*>(userdata);
        return scene && scene->allocate_descriptor_set(layout, *pool_out, *set_out);
    }

public:
    bool invalidate_texture(const std::string& path) {
        if (!initialized_) return false;
        auto it = textures_.find(path);
        if (it == textures_.end()) return false;
        it->second->destroy(upload_context());
        textures_.erase(it);
        return true;
    }

    void clear_textures() {
        if (initialized_) {
            const VulkanUploadContext ctx = upload_context();
            for (auto& entry : textures_) entry.second->destroy(ctx);
        }
        textures_.clear();
    }

    bool reload_texture(const std::string& path, int svg_w = 0, int svg_h = 0) {
        invalidate_texture(path);
        return load_texture(path, svg_w, svg_h);
    }

    TextureVk* ensure_texture(const std::string& path, int svg_w = 0, int svg_h = 0) {
        if (path.empty() || !initialized_) return nullptr;
        auto it = textures_.find(path);
        if (it != textures_.end()) return it->second.get();
        // Sem teto fixo: se o pool atual esgotar, allocate_descriptor_set()
        // cria um pool maior e os sets antigos continuam válidos.
        auto texture = std::make_unique<TextureVk>();
        if (!texture->load(upload_context(), path, svg_w, svg_h)) {
            __android_log_print(ANDROID_LOG_WARN, "nativeapp-vk",
                                "Textura nao carregada: %s", path.c_str());
            return nullptr;
        }
        it = textures_.emplace(path, std::move(texture)).first;
        return it->second.get();
    }

    void draw_instanced (const MeshData& mesh, const Material& material,
                         const std::vector<Mat4>& models) {
        enqueue_instanced(mesh, material, models);
    }

    void draw_instanced (const MeshData& mesh, const Material& material,
                         const float* model_matrices, std::size_t count) {
        if (!model_matrices || count == 0) return;
        if (count > std::numeric_limits<std::size_t>::max() / 16) return;
        std::vector<Mat4> models(count);
        std::memcpy(models.data(), model_matrices, count * sizeof(Mat4));
        enqueue_instanced(mesh, material, models);
    }

    void enqueue_instanced(const MeshData& mesh, const Material& material,
                           const std::vector<Mat4>& models) {
        if (!initialized_ || !swapchain_ok_ || mesh.vertices.empty() || models.empty()) return;
        enqueue_draw(mesh, material, nullptr, nullptr);
        if (pending_items_.empty()) return;
        DrawItem& item = pending_items_.back();
        item.instanced = true;
        item.instances = models;
    }

    CustomUniformHandle register_custom_uniforms(
        const std::vector<CustomUniformBinding>& bindings) {
        if (!initialized_ || !device_ || bindings.empty()) return 0;
        CustomUniformGroup group;
        std::vector<VkDescriptorSetLayoutBinding> layout_bindings;
        layout_bindings.reserve(bindings.size());
        for (const CustomUniformBinding& binding : bindings) {
            if (binding.type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
                binding.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                __android_log_print(ANDROID_LOG_WARN, "nativeapp-vk",
                                    "binding %u: tipo de descritor não suportado",
                                    binding.binding);
                destroy_custom_group(group);
                return 0;
            }
            VkDescriptorSetLayoutBinding layout_binding{};
            layout_binding.binding = binding.binding;
            layout_binding.descriptorType = binding.type;
            layout_binding.descriptorCount = 1;
            layout_binding.stageFlags = binding.stages;
            layout_bindings.push_back(layout_binding);
            if (binding.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                if (binding.size == 0) {
                    destroy_custom_group(group);
                    return 0;
                }
                CustomUniformGroup::Entry entry;
                entry.binding = binding;
                const VkDeviceSize alignment = std::max<VkDeviceSize>(
                    physical_device_
                        ? vk_ubo_alignment() : 256u, 1u);
                entry.buffer_size = static_cast<std::size_t>(
                    ((static_cast<VkDeviceSize>(binding.size) + alignment - 1) /
                     alignment) * alignment);
                group.entries.push_back(std::move(entry));
            }
        }
        VkDescriptorSetLayoutCreateInfo layout_info{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout_info.bindingCount = static_cast<uint32_t>(layout_bindings.size());
        layout_info.pBindings = layout_bindings.data();
        if (vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, &group.layout) !=
            VK_SUCCESS) {
            __android_log_print(ANDROID_LOG_WARN, "nativeapp-vk",
                                "register_custom_uniforms: falha ao criar layout");
            destroy_custom_group(group);
            return 0;
        }
        for (size_t frame = 0; frame < kMaxFramesInFlight; ++frame) {
            if (!allocate_descriptor_set(group.layout, group.pools[frame], group.sets[frame])) {
                __android_log_print(ANDROID_LOG_WARN, "nativeapp-vk",
                                    "register_custom_uniforms: falha ao alocar sets");
                destroy_custom_group(group);
                return 0;
            }
        }
        for (CustomUniformGroup::Entry& entry : group.entries) {
            for (size_t frame = 0; frame < kMaxFramesInFlight; ++frame) {
                VkBuffer buffer = VK_NULL_HANDLE;
                VkDeviceMemory memory = VK_NULL_HANDLE;
                VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                buffer_info.size = entry.buffer_size;
                buffer_info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
                buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                if (vkCreateBuffer(device_, &buffer_info, nullptr, &buffer) != VK_SUCCESS) {
                    destroy_custom_group(group);
                    return 0;
                }
                VkMemoryRequirements requirements{};
                vkGetBufferMemoryRequirements(device_, buffer, &requirements);
                VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                alloc.allocationSize = requirements.size;
                alloc.memoryTypeIndex = find_memory_type(
                    requirements.memoryTypeBits,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                if (vkAllocateMemory(device_, &alloc, nullptr, &memory) != VK_SUCCESS ||
                    vkBindBufferMemory(device_, buffer, memory, 0) != VK_SUCCESS ||
                    vkMapMemory(device_, memory, 0, entry.buffer_size, 0,
                                &entry.mapped[frame]) != VK_SUCCESS) {
                    if (memory) vkFreeMemory(device_, memory, nullptr);
                    vkDestroyBuffer(device_, buffer, nullptr);
                    destroy_custom_group(group);
                    return 0;
                }
                entry.buffers[frame] = buffer;
                entry.memories[frame] = memory;
                std::memset(entry.mapped[frame], 0, entry.buffer_size);
                VkDescriptorBufferInfo buffer_info_desc{};
                buffer_info_desc.buffer = buffer;
                buffer_info_desc.offset = 0;
                buffer_info_desc.range = entry.binding.size;
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = group.sets[frame];
                write.dstBinding = entry.binding.binding;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                write.pBufferInfo = &buffer_info_desc;
                vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
            }
        }
        // Samplers começam na textura branca para que o layout esteja completo.
        for (const CustomUniformBinding& binding : bindings) {
            if (binding.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
            VkDescriptorImageInfo image_info{};
            image_info.sampler = white_texture_.sampler();
            image_info.imageView = white_texture_.view();
            image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            for (size_t frame = 0; frame < kMaxFramesInFlight; ++frame) {
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = group.sets[frame];
                write.dstBinding = binding.binding;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.pImageInfo = &image_info;
                vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
            }
        }
        const CustomUniformHandle handle = next_custom_handle_++;
        custom_uniforms_.emplace(handle, std::move(group));
        return handle;
    }

    bool write_uniform(CustomUniformHandle handle, uint32_t binding,
                       const void* data, std::size_t size) {
        CustomUniformGroup* group = find_custom_group(handle);
        if (!group || !data) return false;
        for (CustomUniformGroup::Entry& entry : group->entries) {
            if (entry.binding.binding != binding ||
                entry.binding.type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
                size > entry.binding.size) {
                continue;
            }
            for (size_t frame = 0; frame < kMaxFramesInFlight; ++frame) {
                if (entry.mapped[frame]) {
                    std::memcpy(entry.mapped[frame], data, size);
                }
            }
            return true;
        }
        return false;
    }

    bool bind_uniform_texture(CustomUniformHandle handle, uint32_t binding,
                              const TextureVk& texture) {
        CustomUniformGroup* group = find_custom_group(handle);
        if (!group || !device_ || !texture.valid()) return false;
        bool found = false;
        for (const CustomUniformGroup::Entry& entry : group->entries) {
            if (entry.binding.binding != binding ||
                entry.binding.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                continue;
            }
            VkDescriptorImageInfo image_info{};
            image_info.sampler = texture.sampler();
            image_info.imageView = texture.view();
            image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            for (size_t frame = 0; frame < kMaxFramesInFlight; ++frame) {
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = group->sets[frame];
                write.dstBinding = binding;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.pImageInfo = &image_info;
                vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
            }
            found = true;
            break;
        }
        return found;
    }

    VkDescriptorSetLayout custom_uniform_layout(CustomUniformHandle handle) const {
        const CustomUniformGroup* group = find_custom_group(handle);
        return group ? group->layout : VK_NULL_HANDLE;
    }

    VkDescriptorSet custom_uniform_descriptor(CustomUniformHandle handle,
                                              uint32_t frame) const {
        const CustomUniformGroup* group = find_custom_group(handle);
        if (!group || frame >= kMaxFramesInFlight) return VK_NULL_HANDLE;
        return group->sets[frame];
    }

    void destroy_custom_uniforms(CustomUniformHandle handle) {
        auto it = custom_uniforms_.find(handle);
        if (it == custom_uniforms_.end()) return;
        if (device_) vkDeviceWaitIdle(device_);
        destroy_custom_group(it->second);
        if (active_custom_uniforms_ == handle) {
            active_custom_uniforms_ = 0;
            rebuild_custom_pipeline();
        }
        for (DrawItem& item : pending_items_) {
            if (item.custom_uniforms == handle) item.custom_uniforms = 0;
        }
        custom_uniforms_.erase(it);
    }

private:
    CustomUniformGroup* find_custom_group(CustomUniformHandle handle) {
        auto it = custom_uniforms_.find(handle);
        return it == custom_uniforms_.end() ? nullptr : &it->second;
    }

    const CustomUniformGroup* find_custom_group(CustomUniformHandle handle) const {
        auto it = custom_uniforms_.find(handle);
        return it == custom_uniforms_.end() ? nullptr : &it->second;
    }

    VkDeviceSize vk_ubo_alignment() const {
        if (!physical_device_) return 256u;
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical_device_, &properties);
        return std::max<VkDeviceSize>(
            properties.limits.minUniformBufferOffsetAlignment, 1u);
    }

    void destroy_custom_group(CustomUniformGroup& group) {
        if (device_) {
            for (CustomUniformGroup::Entry& entry : group.entries) {
                for (size_t frame = 0; frame < kMaxFramesInFlight; ++frame) {
                    if (entry.mapped[frame]) {
                        vkUnmapMemory(device_, entry.memories[frame]);
                        entry.mapped[frame] = nullptr;
                    }
                    if (entry.memories[frame]) {
                        vkFreeMemory(device_, entry.memories[frame], nullptr);
                        entry.memories[frame] = VK_NULL_HANDLE;
                    }
                    if (entry.buffers[frame]) {
                        vkDestroyBuffer(device_, entry.buffers[frame], nullptr);
                        entry.buffers[frame] = VK_NULL_HANDLE;
                    }
                }
            }
            for (size_t frame = 0; frame < kMaxFramesInFlight; ++frame) {
                if (group.sets[frame] && group.pools[frame]) {
                    vkFreeDescriptorSets(device_, group.pools[frame], 1, &group.sets[frame]);
                }
                group.sets[frame] = VK_NULL_HANDLE;
                group.pools[frame] = VK_NULL_HANDLE;
            }
            if (group.layout) vkDestroyDescriptorSetLayout(device_, group.layout, nullptr);
        }
        group.entries.clear();
        group.layout = VK_NULL_HANDLE;
    }

    bool create_descriptor_pool(uint32_t max_sets, uint32_t ubo_descriptors,
                                uint32_t sampler_descriptors) {
        VkDescriptorPoolSize sizes[3]{};
        sizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, ubo_descriptors};
        sizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sampler_descriptors};
        // Bindings de usuário usam UBO estático (set 2), não dinâmico.
        sizes[2] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, ubo_descriptors};
        VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        info.maxSets = max_sets;
        info.poolSizeCount = 3;
        info.pPoolSizes = sizes;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        if (vkCreateDescriptorPool(device_, &info, nullptr, &pool) != VK_SUCCESS) return false;
        descriptor_pools_.push_back(pool);
        descriptor_pool_ = pool;
        descriptor_capacity_ = max_sets;
        ubo_descriptor_capacity_ = ubo_descriptors;
        sampler_descriptor_capacity_ = sampler_descriptors;
        return true;
    }

    bool allocate_descriptor_set(VkDescriptorSetLayout layout,
                                 VkDescriptorPool& pool_out, VkDescriptorSet& set_out) {
        if (!device_ || !descriptor_pool_ || !layout) return false;
        for (int attempt = 0; attempt < 6; ++attempt) {
            VkDescriptorSetAllocateInfo info{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            info.descriptorPool = descriptor_pool_;
            info.descriptorSetCount = 1;
            info.pSetLayouts = &layout;
            const VkResult result = vkAllocateDescriptorSets(device_, &info, &set_out);
            if (result == VK_SUCCESS) {
                pool_out = descriptor_pool_;
                return true;
            }
            if (result != VK_ERROR_OUT_OF_POOL_MEMORY &&
                result != VK_ERROR_FRAGMENTED_POOL) {
                return false;
            }
            const uint32_t sets = std::max<uint32_t>(descriptor_capacity_ * 2, 8);
            const uint32_t ubos = std::max<uint32_t>(ubo_descriptor_capacity_ * 2, 4);
            const uint32_t samplers = std::max<uint32_t>(
                sampler_descriptor_capacity_ * 2, kMaxTextures);
            if (!create_descriptor_pool(sets, ubos, samplers)) return false;
            __android_log_print(ANDROID_LOG_INFO, "nativeapp-vk",
                                "pool de descritores ampliado para %u sets", sets);
        }
        return false;
    }

    void destroy_descriptor_pools() {
        if (!device_) return;
        for (VkDescriptorPool pool : descriptor_pools_) {
            if (pool) vkDestroyDescriptorPool(device_, pool, nullptr);
        }
        descriptor_pools_.clear();
        descriptor_pool_ = VK_NULL_HANDLE;
    }

    static VkBool32 VKAPI_CALL on_validation_message(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT,
        const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
        const int priority = severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT
            ? ANDROID_LOG_ERROR
            : (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                   ? ANDROID_LOG_WARN : ANDROID_LOG_INFO);
        __android_log_print(priority, "nativeapp-vk-validation", "%s",
                            data && data->pMessage ? data->pMessage : "(sem mensagem)");
        return VK_FALSE;
    }

    void destroy_debug_messenger() {
        if (!debug_messenger_ || !instance_) return;
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy) destroy(instance_, debug_messenger_, nullptr);
        debug_messenger_ = VK_NULL_HANDLE;
    }

    void create_instance() {
        VkApplicationInfo app_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app_info.pApplicationName = "apkfast nativeapp";
        app_info.apiVersion = VK_API_VERSION_1_0;

        std::vector<const char*> extensions = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
        };
        std::vector<const char*> layers;
        bool debug_utils = false;

        if (validation_) {
            uint32_t layer_count = 0;
            vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
            std::vector<VkLayerProperties> available(layer_count);
            if (layer_count) {
                vkEnumerateInstanceLayerProperties(&layer_count, available.data());
            }
            // Nome atual (KHRONOS) e o legado, que ainda existe em SDKs antigos.
            const char* candidates[] = {"VK_LAYER_KHRONOS_validation",
                                        "VK_LAYER_LUNARG_standard_validation"};
            for (const char* candidate : candidates) {
                for (const VkLayerProperties& layer : available) {
                    if (std::strcmp(layer.layerName, candidate) == 0) {
                        layers.push_back(candidate);
                        break;
                    }
                }
                if (!layers.empty()) break;
            }
            if (layers.empty()) {
                __android_log_print(ANDROID_LOG_WARN, "nativeapp-vk",
                                    "set_validation(true): nenhuma layer de validação "
                                    "disponível; executando sem validação");
            }

            uint32_t extension_count = 0;
            vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr);
            std::vector<VkExtensionProperties> instance_extensions(extension_count);
            if (extension_count) {
                vkEnumerateInstanceExtensionProperties(nullptr, &extension_count,
                                                       instance_extensions.data());
            }
            for (const VkExtensionProperties& extension : instance_extensions) {
                if (std::strcmp(extension.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) {
                    debug_utils = true;
                    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                    break;
                }
            }
            if (!debug_utils) {
                __android_log_print(ANDROID_LOG_WARN, "nativeapp-vk",
                                    "VK_EXT_debug_utils ausente: mensagens da layer "
                                    "não serão redirecionadas para o logcat");
            }
        }

        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        create.pApplicationInfo = &app_info;
        create.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        create.ppEnabledExtensionNames = extensions.data();
        create.enabledLayerCount = static_cast<uint32_t>(layers.size());
        create.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();
        vk_check(vkCreateInstance(&create, nullptr, &instance_), "vkCreateInstance");

        validation_active_ = !layers.empty();
        if (debug_utils) {
            auto create_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
            if (create_messenger) {
                VkDebugUtilsMessengerCreateInfoEXT messenger{
                    VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
                messenger.messageSeverity =
                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
                messenger.messageType =
                    VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                    VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                    VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
                messenger.pfnUserCallback = &VulkanScene::on_validation_message;
                if (create_messenger(instance_, &messenger, nullptr, &debug_messenger_) ==
                    VK_SUCCESS) {
                    __android_log_print(ANDROID_LOG_INFO, "nativeapp-vk",
                                        "validação Vulkan %s (logcat: nativeapp-vk-validation)",
                                        validation_active_ ? "ativada" : "indisponível");
                } else {
                    debug_messenger_ = VK_NULL_HANDLE;
                }
            }
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

    /// Recursos que não dependem da janela: upload pool, layouts, UBOs e a
    /// textura branca 1x1 usada quando Material::texture está vazio.
    void create_shared_gpu_resources() {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical_device_, &properties);
        const VkDeviceSize alignment = std::max<VkDeviceSize>(
            properties.limits.minUniformBufferOffsetAlignment, 1u);
        uniform_stride_ = ((static_cast<VkDeviceSize>(sizeof(SceneUniforms)) + alignment - 1) /
                           alignment) * alignment;

        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.queueFamilyIndex = queue_family_;
        vk_check(vkCreateCommandPool(device_, &pool, nullptr, &upload_pool_),
                 "vkCreateCommandPool(upload)");

        VkDescriptorSetLayoutBinding ubo_binding{};
        ubo_binding.binding = 0;
        ubo_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        ubo_binding.descriptorCount = 1;
        ubo_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo ubo_layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ubo_layout.bindingCount = 1;
        ubo_layout.pBindings = &ubo_binding;
        vk_check(vkCreateDescriptorSetLayout(device_, &ubo_layout, nullptr, &ubo_layout_),
                 "vkCreateDescriptorSetLayout(ubo)");

        VkDescriptorSetLayoutBinding sampler_binding{};
        sampler_binding.binding = 0;
        sampler_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sampler_binding.descriptorCount = 1;
        sampler_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo sampler_layout{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sampler_layout.bindingCount = 1;
        sampler_layout.pBindings = &sampler_binding;
        vk_check(vkCreateDescriptorSetLayout(device_, &sampler_layout, nullptr,
                                             &sampler_layout_),
                 "vkCreateDescriptorSetLayout(sampler)");

        upload_tracker_.set_device(device_);
        // Capacidade inicial apenas: allocate_descriptor_set() cria pools
        // maiores sob demanda e nenhum texture é descartado por limite.
        if (!create_descriptor_pool(
                static_cast<uint32_t>(kMaxFramesInFlight + kMaxTextures +
                                      kTextureDescriptorReserve + kUboSetReserve),
                static_cast<uint32_t>(kMaxFramesInFlight + kUboSetReserve),
                static_cast<uint32_t>(kMaxTextures + kTextureDescriptorReserve))) {
            throw VulkanError("Falha ao criar pool de descritores");
        }

        if (!grow_uniform_slots(kMaxDrawSlots)) {
            throw VulkanError("Falha ao criar UBO do frame");
        }

        const uint8_t white[4] = {255, 255, 255, 255};
        if (!white_texture_.load_rgba(upload_context(), white, 1, 1)) {
            throw VulkanError("Falha ao criar textura branca 1x1");
        }
    }

    /// (Re)cria os UBOs e seus descriptor sets com capacidade para `slots`
    /// desenhos. Nenhum draw é descartado por um teto fixo (G04).
    bool grow_uniform_slots(size_t slots) {
        if (slots <= uniform_capacity_ && uniform_buffers_[0]) return true;
        if (!device_ || !descriptor_pool_ || !ubo_layout_) return false;
        const size_t doubled = uniform_capacity_ ? uniform_capacity_ * 2 : kMaxDrawSlots;
        const size_t new_capacity = std::max(slots, doubled);
        const VkDeviceSize new_size = uniform_stride_ * new_capacity;
        if (new_size > static_cast<VkDeviceSize>(std::numeric_limits<uint32_t>::max())) {
            __android_log_print(ANDROID_LOG_ERROR, "nativeapp-vk",
                                "UBO excede o limite de dynamic offset uint32");
            return false;
        }
        // A substituição acontece entre frames: espera todos os in-flight.
        vkDeviceWaitIdle(device_);
        for (size_t frame = 0; frame < kMaxFramesInFlight; ++frame) {
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            void* mapped = nullptr;
            VkDescriptorSet set = VK_NULL_HANDLE;
            VkDescriptorPool set_pool = VK_NULL_HANDLE;
            try {
                VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                info.size = new_size;
                info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
                info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                vk_check(vkCreateBuffer(device_, &info, nullptr, &buffer),
                         "vkCreateBuffer(ubo)");
                VkMemoryRequirements requirements{};
                vkGetBufferMemoryRequirements(device_, buffer, &requirements);
                VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                alloc.allocationSize = requirements.size;
                alloc.memoryTypeIndex = find_memory_type(
                    requirements.memoryTypeBits,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                vk_check(vkAllocateMemory(device_, &alloc, nullptr, &memory),
                         "vkAllocateMemory(ubo)");
                vk_check(vkBindBufferMemory(device_, buffer, memory, 0),
                         "vkBindBufferMemory(ubo)");
                vk_check(vkMapMemory(device_, memory, 0, new_size, 0, &mapped),
                         "vkMapMemory(ubo)");
                if (!mapped) throw VulkanError("vkMapMemory(ubo) retornou nulo");

                if (!allocate_descriptor_set(ubo_layout_, set_pool, set)) {
                    throw VulkanError("vkAllocateDescriptorSets(ubo) falhou");
                }
                VkDescriptorBufferInfo buffer_info{};
                buffer_info.buffer = buffer;
                buffer_info.offset = 0;
                buffer_info.range = sizeof(SceneUniforms);
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = set;
                write.dstBinding = 0;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                write.pBufferInfo = &buffer_info;
                vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
            } catch (...) {
                if (set) vkFreeDescriptorSets(device_, set_pool, 1, &set);
                if (mapped) vkUnmapMemory(device_, memory);
                if (memory) vkFreeMemory(device_, memory, nullptr);
                if (buffer) vkDestroyBuffer(device_, buffer, nullptr);
                return false;
            }

            if (uniform_mapped_[frame]) vkUnmapMemory(device_, uniform_memories_[frame]);
            if (uniform_buffers_[frame]) vkDestroyBuffer(device_, uniform_buffers_[frame], nullptr);
            if (uniform_memories_[frame]) vkFreeMemory(device_, uniform_memories_[frame], nullptr);
            if (ubo_sets_[frame]) {
                vkFreeDescriptorSets(device_, ubo_pools_[frame], 1, &ubo_sets_[frame]);
            }
            uniform_buffers_[frame] = buffer;
            uniform_memories_[frame] = memory;
            uniform_mapped_[frame] = mapped;
            ubo_sets_[frame] = set;
            ubo_pools_[frame] = set_pool;
        }
        uniform_capacity_ = new_capacity;
        return true;
    }

    void destroy_shared_gpu_resources() {
        pipelines_.clear();
        custom_pipeline_.destroy();
        pending_items_.clear();
        for (auto& items : frame_items_) items.clear();
        if (!device_) return;
        vkDeviceWaitIdle(device_);
        // Aposenta antes de liberar os pools: os descriptors podem vir de
        // pools antigos criados quando o cache cresceu.
        const VulkanUploadContext ctx = upload_context();
        for (auto& entry : textures_) entry.second->destroy(ctx);
        textures_.clear();
        font_.reset();
        font_texture_.destroy(ctx);
        white_texture_.destroy(ctx);
        upload_tracker_.force_cleanup();
        for (auto& entry : custom_uniforms_) destroy_custom_group(entry.second);
        custom_uniforms_.clear();
        active_custom_uniforms_ = 0;
        for (size_t frame = 0; frame < kMaxFramesInFlight; ++frame) {
            if (uniform_mapped_[frame]) {
                vkUnmapMemory(device_, uniform_memories_[frame]);
                uniform_mapped_[frame] = nullptr;
            }
            if (uniform_buffers_[frame]) {
                vkDestroyBuffer(device_, uniform_buffers_[frame], nullptr);
                uniform_buffers_[frame] = VK_NULL_HANDLE;
            }
            if (uniform_memories_[frame]) {
                vkFreeMemory(device_, uniform_memories_[frame], nullptr);
                uniform_memories_[frame] = VK_NULL_HANDLE;
            }
            ubo_sets_[frame] = VK_NULL_HANDLE;  // liberadas com o pool
            ubo_pools_[frame] = VK_NULL_HANDLE;
        }
        destroy_descriptor_pools();
        if (ubo_layout_) {
            vkDestroyDescriptorSetLayout(device_, ubo_layout_, nullptr);
            ubo_layout_ = VK_NULL_HANDLE;
        }
        if (sampler_layout_) {
            vkDestroyDescriptorSetLayout(device_, sampler_layout_, nullptr);
            sampler_layout_ = VK_NULL_HANDLE;
        }
        if (upload_pool_) {
            vkDestroyCommandPool(device_, upload_pool_, nullptr);
            upload_pool_ = VK_NULL_HANDLE;
        }
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

    void rebuild_custom_pipeline() {
        custom_pipeline_set_count_ = 0;
        if (!has_custom_pipeline_ || !render_pass_ ||
            pipeline_config_.vertex_spv.empty() || pipeline_config_.fragment_spv.empty()) {
            return;
        }
        const CustomUniformGroup* group = find_custom_group(pipeline_config_.custom_uniforms);
        VkDescriptorSetLayout layouts[3] = {ubo_layout_, sampler_layout_, VK_NULL_HANDLE};
        uint32_t count = 2;
        if (group && group->layout) {
            layouts[2] = group->layout;
            count = 3;
        }
        custom_pipeline_.build_with_layouts(device_, render_pass_, layouts, count,
                                            pipeline_config_);
        custom_pipeline_set_count_ = count;
    }

    /// Faz o upload da geometria e escreve os uniforms do frame já sincronizado.
    void prepare_frame(uint32_t frame) {
        // A fence deste frame foi esperada em render(): os buffers antigos
        // podem ser destruídos com segurança.
        frame_items_[frame].clear();
        frame_items_[frame].reserve(pending_items_.size());
        if (pending_items_.size() > uniform_capacity_ &&
            !grow_uniform_slots(pending_items_.size())) {
            __android_log_print(ANDROID_LOG_ERROR, "nativeapp-vk",
                                "falha ao crescer UBO para %zu desenhos",
                                pending_items_.size());
            return;  // mantem a fila para a proxima tentativa
        }
        auto* mapped = static_cast<uint8_t*>(uniform_mapped_[frame]);
        size_t slot = 0;
        for (auto& item : pending_items_) {
            item.uniform_offset = static_cast<VkDeviceSize>(slot) * uniform_stride_;
            SceneUniforms uniforms{};
            // render() legado usa NDC (-0.5..0.5), igual a GLES2Scene::render().
            uniforms.projection = legacy_render_ ? mat4_identity() : projection_;
            uniforms.model = item.model;
            std::memcpy(uniforms.color, item.color, sizeof(item.color));
            uniforms.use_texture = item.use_texture;
            if (mapped) {
                std::memcpy(mapped + static_cast<size_t>(item.uniform_offset),
                            &uniforms, sizeof(uniforms));
            }
            try {
                if (item.text) {
                    item.buffers.upload_raw(device_, physical_device_,
                                            item.glyphs.data(), item.glyphs.size(),
                                            sizeof(vk_detail::GlyphVertex));
                } else {
                    item.buffers.upload(device_, physical_device_, queue_,
                                        command_pool_, item.mesh);
                    if (item.instanced) {
                        item.buffers.upload_instances(device_, physical_device_,
                                                      item.instances);
                    }
                }
            } catch (const std::exception& error) {
                __android_log_print(ANDROID_LOG_WARN, "nativeapp-vk",
                                    "upload de geometria falhou: %s", error.what());
            }
            frame_items_[frame].push_back(std::move(item));
            ++slot;
        }
        pending_items_.clear();
    }

    GraphicsPipeline* get_or_create_pipeline(const DrawItem& item) {
        const PipelineKey key{item.depth_test, item.blend, item.text,
                              item.instanced, item.topology};
        auto found = pipelines_.find(key);
        if (found != pipelines_.end()) return found->second.get();

        PipelineConfig config;
        if (item.text) {
            config.vertex_spv.assign(std::begin(vk_detail::kTextVertSpv),
                                     std::end(vk_detail::kTextVertSpv));
            config.fragment_spv.assign(std::begin(vk_detail::kTextFragSpv),
                                       std::end(vk_detail::kTextFragSpv));
        } else if (item.instanced) {
            config.vertex_spv.assign(std::begin(vk_detail::kGeometryInstancedVertSpv),
                                     std::end(vk_detail::kGeometryInstancedVertSpv));
            config.fragment_spv.assign(std::begin(vk_detail::kGeometryFragSpv),
                                       std::end(vk_detail::kGeometryFragSpv));
        } else {
            config.vertex_spv.assign(std::begin(vk_detail::kGeometryVertSpv),
                                     std::end(vk_detail::kGeometryVertSpv));
            config.fragment_spv.assign(std::begin(vk_detail::kGeometryFragSpv),
                                       std::end(vk_detail::kGeometryFragSpv));
        }
        config.depth_test = item.depth_test;
        config.blend = item.blend;
        config.topology = item.topology;
        config.text_layout = item.text;
        config.instanced = item.instanced;
        config.color_format = color_format_;
        config.depth_format = depth_format_;

        auto pipeline = std::make_unique<GraphicsPipeline>();
        const VkDescriptorSetLayout layouts[] = {ubo_layout_, sampler_layout_};
        pipeline->build_with_layouts(device_, render_pass_, layouts, 2, config);
        auto inserted = pipelines_.emplace(key, std::move(pipeline));
        return inserted.first->second.get();
    }

    void record_command_buffer(VkCommandBuffer command_buffer, uint32_t image_index,
                               uint32_t frame) {
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

        // O pipeline externo (legado) so se aplica a geometria. Itens de
        // texto SEMPRE usam o pipeline de texto com UBO + atlas (G02).
        for (const DrawItem& item : frame_items_[frame]) {
            if (!item.buffers.has_vertices()) continue;
            const bool builtin = item.text || !has_custom_pipeline_ ||
                                 !custom_pipeline_.pipeline();
            if (builtin) {
                if (!item.descriptor) continue;
                GraphicsPipeline* pipeline = get_or_create_pipeline(item);
                if (!pipeline || !pipeline->pipeline()) continue;
                pipeline->bind(command_buffer);
                const uint32_t dynamic_offset =
                    static_cast<uint32_t>(item.uniform_offset);
                vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        pipeline->layout(), 0, 1, &ubo_sets_[frame],
                                        1, &dynamic_offset);
                vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        pipeline->layout(), 1, 1, &item.descriptor,
                                        0, nullptr);
            } else {
                custom_pipeline_.bind(command_buffer);
                if (!item.descriptor) continue;
                const uint32_t dynamic_offset =
                    static_cast<uint32_t>(item.uniform_offset);
                vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        custom_pipeline_.layout(), 0, 1, &ubo_sets_[frame],
                                        1, &dynamic_offset);
                vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        custom_pipeline_.layout(), 1, 1, &item.descriptor,
                                        0, nullptr);
                if (custom_pipeline_set_count_ == 3) {
                    const VkDescriptorSet custom_set =
                        custom_uniform_descriptor(item.custom_uniforms, frame);
                    if (custom_set) {
                        vkCmdBindDescriptorSets(command_buffer,
                                                VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                custom_pipeline_.layout(), 2, 1,
                                                &custom_set, 0, nullptr);
                    }
                }
            }
            item.buffers.bind(command_buffer);
            item.buffers.draw(command_buffer);
        }

        vkCmdEndRenderPass(command_buffer);
        vk_check(vkEndCommandBuffer(command_buffer), "vkEndCommandBuffer");
    }

    uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties) const {
        return vk_detail::find_memory_type(physical_device_, type_filter, properties);
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
        window_ = saved_window;  // preserva a janela entre recriações
        try {
            create_surface();
            create_swapchain();
            create_command_buffers();
            rebuild_custom_pipeline();
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
    bool validation_active_ = false;
    VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
    bool legacy_render_ = false;
    bool device_lost_ = false;
    bool needs_asset_reload_ = false;
    bool initialized_ = false;
    bool swapchain_ok_ = false;

    // Recursos de device (criados em init, destruídos em destroy).
    mutable VulkanUploadTracker upload_tracker_;
    VkCommandPool upload_pool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout ubo_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout sampler_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> descriptor_pools_;
    uint32_t descriptor_capacity_ = 0;
    uint32_t ubo_descriptor_capacity_ = 0;
    uint32_t sampler_descriptor_capacity_ = 0;
    std::array<VkBuffer, kMaxFramesInFlight> uniform_buffers_{};
    std::array<VkDeviceMemory, kMaxFramesInFlight> uniform_memories_{};
    std::array<void*, kMaxFramesInFlight> uniform_mapped_{};
    std::array<VkDescriptorSet, kMaxFramesInFlight> ubo_sets_{};
    std::array<VkDescriptorPool, kMaxFramesInFlight> ubo_pools_{};
    VkDeviceSize uniform_stride_ = 256;
    size_t uniform_capacity_ = 0;

    TextureVk white_texture_;
    std::unordered_map<std::string, std::unique_ptr<TextureVk>> textures_;
    std::unique_ptr<FontAtlas> font_;
    TextureVk font_texture_;

    Mat4 projection_ = mat4_identity();
    bool projection_set_ = false;

    std::map<PipelineKey, std::unique_ptr<GraphicsPipeline>> pipelines_;
    GraphicsPipeline custom_pipeline_;
    PipelineConfig pipeline_config_;
    bool has_custom_pipeline_ = false;
    uint32_t custom_pipeline_set_count_ = 0;
    std::map<CustomUniformHandle, CustomUniformGroup> custom_uniforms_;
    CustomUniformHandle next_custom_handle_ = 1;
    CustomUniformHandle active_custom_uniforms_ = 0;
    std::vector<DrawItem> pending_items_;
    std::array<std::vector<DrawItem>, kMaxFramesInFlight> frame_items_;
};

}  // namespace nativeapp
