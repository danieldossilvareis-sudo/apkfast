"""Testes estáticos da paridade Vulkan × GLES2 (sem GPU e sem SDK)."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "include" / "nativeapp" / "vk.hpp"
EXAMPLE = ROOT / "examples" / "nativeapp-vulkan" / "jni" / "main.cpp"
DOCS = ROOT / "docs" / "graphics-vulkan.md"


class VulkanParityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.header = HEADER.read_text(encoding="utf-8")
        cls.example = EXAMPLE.read_text(encoding="utf-8")
        cls.docs = DOCS.read_text(encoding="utf-8")

    def assert_all(self, source: str, *tokens: str) -> None:
        for token in tokens:
            with self.subTest(token=token):
                self.assertIn(token, source)

    def test_public_api_existe(self) -> None:
        self.assert_all(
            self.header,
            "class TextureVk",
            "bool load_texture(",
            "bool load_font(",
            "void draw_text(",
            "bool has_font() const",
            "void set_projection(",
        )

    def test_ubo_com_uniforms_da_cena(self) -> None:
        self.assert_all(
            self.header,
            "struct alignas(16) SceneUniforms",
            "u_projection",
            "u_model",
            "u_color",
            "u_use_texture",
            "VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC",
            "uniform_buffers_",
            "vkCmdBindDescriptorSets",
        )

    def test_descritor_de_imagem_e_sampler(self) -> None:
        self.assert_all(
            self.header,
            "VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER",
            "VK_FILTER_LINEAR",
            "VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE",
            "VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL",
            "white_texture_",
        )

    def test_textura_com_decode_do_gl_e_staging(self) -> None:
        self.assert_all(
            self.header,
            "decode_asset_image",
            "VK_BUFFER_USAGE_TRANSFER_SRC_BIT",
            "vkCmdCopyBufferToImage",
            "vkQueueWaitIdle",
            "vkDestroySampler",
        )

    def test_material_e_model_sao_aplicados(self) -> None:
        self.assertNotIn("(void)material", self.header)
        self.assertNotIn("(void)model", self.header)
        self.assert_all(
            self.header,
            "material.color",
            "material.texture",
            "material.depth_test",
            "material.blend",
            "item.model",
            "struct PipelineKey",
            "text_layout",
        )

    def test_fonte_e_quads_de_glyph(self) -> None:
        self.assert_all(
            self.header,
            "FontAtlas",
            "GlyphVertex",
            "font_->quad(",
            "font_texture_",
            "VK_CULL_MODE_NONE",
        )

    def test_shaders_embutidos_com_fonte(self) -> None:
        self.assert_all(
            self.header,
            "glslangValidator",
            "#version 450",
            "layout(set = 0, binding = 0) uniform SceneUniforms",
            "layout(set = 1, binding = 0) uniform sampler2D u_texture;",
            "v_uv = a_uv;",
            "decode_asset_image",
            "kGeometryVertSpv[]",
            "kGeometryFragSpv[]",
            "kTextVertSpv[]",
            "kTextFragSpv[]",
        )
        words = re.findall(r"0x[0-9a-f]{8}u", self.header)
        self.assertGreater(len(words), 1000, "arrays SPIR-V ausentes ou truncados")

    def test_exemplo_demonstra_paridade(self) -> None:
        self.assert_all(
            self.example,
            "set_clear_color",
            'load_texture("sprite.png")',
            'load_font("font.ttf"',
            'draw_text("VULKAN OK"',
            "scene_.draw(triangle, colored",
            "scene_.render()",
        )

    def test_instancing_real_com_divisor_de_instancia(self) -> None:
        self.assertRegex(self.header, r"draw_instanced\s*\(")
        self.assert_all(
            self.header,
            "std::vector<Mat4>",
            "upload_instances",
            "vkCmdDrawIndexed",
            "instanced = true",
            "VK_VERTEX_INPUT_RATE_INSTANCE",
        )

    def test_hooks_de_validation_layer(self) -> None:
        self.assert_all(
            self.header,
            "void set_validation(bool enabled)",
            "bool validation_enabled() const",
            "validation_active_",
            "VK_LAYER_KHRONOS_validation",
            "VK_LAYER_LUNARG_standard_validation",
            "vkEnumerateInstanceLayerProperties",
            "VK_EXT_DEBUG_UTILS_EXTENSION_NAME",
            "PFN_vkCreateDebugUtilsMessengerEXT",
            "VkDebugUtilsMessengerCreateInfoEXT",
            "VulkanScene::on_validation_message",
            "PFN_vkDestroyDebugUtilsMessengerEXT",
            "destroy_debug_messenger()",
        )
        # Já não é apenas um aviso: a layer entra na criação da instância.
        self.assertIn("create.ppEnabledLayerNames", self.header)
        self.assertNotIn("layers de validação não habilitadas", self.header)

    def test_api_de_uniforms_e_shader_customizado(self) -> None:
        self.assert_all(
            self.header,
            "using CustomUniformHandle",
            "struct CustomUniformBinding",
            "CustomUniformHandle register_custom_uniforms(",
            "bool write_uniform(",
            "bool bind_uniform_texture(",
            "VkDescriptorSetLayout custom_uniform_layout(",
            "VkDescriptorSet custom_uniform_descriptor(",
            "void destroy_custom_uniforms(",
            "void set_pipeline(PipelineConfig config, CustomUniformHandle",
            "VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER",
            "VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER",
        )

    def test_upload_assincrono_com_fence_e_semaphore(self) -> None:
        self.assert_all(
            self.header,
            "class VulkanUploadTracker",
            "VkSemaphore ready",
            "VkFence fence",
            "void track_upload(",
            "std::vector<VkSemaphore> pending_semaphores() const",
            "void mark_consumed()",
            "void note_submission()",
            "vkCreateFence(device_, &fence_info",
            "vkCreateSemaphore(device_, &semaphore_info",
            "upload_tracker_.pending_semaphores()",
            "VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT",
        )
        # O upload da cena não pode mais parar a fila inteira.
        self.assertNotIn("vkQueueWaitIdle(queue_)", self.header)
        self.assertNotIn("vkQueueWaitIdle(ctx.queue)", self.header)

    def test_render_legado_em_ndc(self) -> None:
        self.assert_all(
            self.header,
            "void set_legacy_render(bool enabled)",
            "bool legacy_render() const",
            "void render_legacy()",
            "if (legacy_render_)",
            "uniforms.projection = legacy_render_ ? mat4_identity() : projection_;",
            "enqueue_draw(MeshData::triangle(), Material{}",
            "void present() override { render(); }",
        )

    def test_documentacao_tabela_de_paridade(self) -> None:
        self.assert_all(
            self.docs,
            "load_font(",
            "draw_text(",
            "TextureVk",
            "Material",
            "GLES2Scene",
            "VulkanScene",
        )
        self.assertRegex(self.docs, r"\|\s*Recurso\s*\|.*GLES", re.IGNORECASE)


if __name__ == "__main__":
    unittest.main()
