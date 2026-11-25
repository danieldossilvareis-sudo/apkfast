"""Testes estáticos da matriz de paridade GL × VK (sem GPU, SDK ou NDK).

A matriz é `docs/parity-matrix.md`. Os testes garantem que ela cobre todas as
capacidades auditadas, que os símbolos públicos esperados continuam existindo
nos headers e que os marcadores `[ambos]`/`[GL only]`/`[VK only]`/`[ausente]`
da matriz batem com a presença real desses símbolos. Gaps marcados como
`RESOLVIDO` só permanecem assim enquanto o símbolo correspondente existir no
header; se o código mudar, a matriz e este arquivo mudam juntos.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MATRIX = ROOT / "docs" / "parity-matrix.md"
GL = ROOT / "include" / "nativeapp" / "gl.hpp"
VK = ROOT / "include" / "nativeapp" / "vk.hpp"
TYPES = ROOT / "include" / "nativeapp" / "graphics_types.hpp"
FONT = ROOT / "include" / "nativeapp" / "font.hpp"
CODECS = ROOT / "include" / "nativeapp" / "image_codecs.hpp"
ANIM_IMAGE = ROOT / "include" / "nativeapp" / "animated_image.hpp"
ANIM_SVG = ROOT / "include" / "nativeapp" / "animated_svg.hpp"
IMAGE_DEMO = ROOT / "examples" / "image-demo" / "jni" / "main.cpp"
VK_EXAMPLE = ROOT / "examples" / "nativeapp-vulkan" / "jni" / "main.cpp"

CAPABILITIES = {
    "C01": "lifecycle",
    "C02": "clear",
    "C03": "geometria",
    "C04": "Material/cor",
    "C05": "textura",
    "C06": "SVG",
    "C07": "WebP animado",
    "C08": "fonte TTF/draw_text",
    "C09": "instancing",
    "C10": "depth",
    "C11": "blend",
    "C12": "cull",
    "C13": "projeção 2D/3D",
    "C14": "uniforms/model",
    "C15": "cache de textura",
    "C16": "múltiplos draws por frame",
    "C17": "resize",
    "C18": "device loss",
    "C19": "validation layers",
    "C20": "upload assíncrono",
    "C21": "API legada set_pipeline",
    "C22": "performance/limites",
}

MARKERS = {"[ambos]", "[GL only]", "[VK only]", "[ausente]", "[parcial]"}
CLASSIFICATIONS = (
    "BLOCKER VISUAL",
    "GAP DE API",
    "GAP DE QUALIDADE",
    "ACEITÁVEL COM JUSTIFICATIVA TÉCNICA",
)

# Gaps fechados nesta rodada. A matriz só pode continuar dizendo RESOLVIDO
# enquanto os testes abaixo encontrarem o símbolo no header correspondente.
RESOLVED_GAPS = {
    "G01", "G02", "G03", "G04", "G05", "G06", "G07", "G08", "G09",
    "G10", "G11", "G12", "G13", "G14", "G15", "G16",
}
OPEN_GAPS = set()

# Um único token determina o marcador das duas colunas. "both" exige [ambos]
# nas duas; "gl_only" exige [GL only] no GL e [ausente] no VK; "vk_only" é o
# espelho. Tokens prefixados por "re:" são expressões regulares (o header VK
# escreve `draw_instanced (` com espaço antes do '(').
SYMBOL_MARKERS = {
    "C02": [("set_clear_color(", "both")],
    "C05": [("decode_asset_image", "both")],
    "C06": [("load_svg_asset(", "both")],
    "C07": [("draw_with_texture(", "both")],
    "C08": [("draw_text(", "both")],
    "C09": [(r"re:draw_instanced\s*\(", "both")],
    "C10": [("depth_test", "both")],
    "C11": [("blend", "both")],
    "C12": [("cull_mode", "vk_only")],
    "C13": [("set_projection(", "both")],
    "C15": [("textures_", "both")],
    "C16": [("kMaxDrawSlots", "vk_only")],
    "C17": [("resize(int width, int height)", "both")],
    "C20": [("vkQueueWaitIdle", "vk_only")],
}

# capability -> (símbolo exclusivo do GL, símbolo exclusivo do VK)
SPLIT_SYMBOLS = {
    "C01": ("class GLContext", "void attach("),
    "C03": ("class Mesh {", "class GpuBuffer {"),
    "C14": ("set_uniform_mat4(", "SceneUniforms"),
    "C19": ("gl_check(", "set_validation("),
    "C21": ("set_shader(", "set_pipeline("),
}


def marker(cell: str) -> str:
    found = re.search(r"\[(?:ambos|GL only|VK only|ausente|parcial)\]", cell)
    return found.group(0) if found else ""


def has(source: str, token: str) -> bool:
    """Presença literal ou, com o prefixo `re:`, por expressão regular."""
    if token.startswith("re:"):
        return re.search(token[3:], source) is not None
    return token in source


class ParityMatrixTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.matrix = MATRIX.read_text(encoding="utf-8")
        cls.gl = GL.read_text(encoding="utf-8")
        cls.vk = VK.read_text(encoding="utf-8")
        cls.capability_rows: dict[str, list[str]] = {}
        cls.gap_rows: dict[str, list[str]] = {}
        for line in cls.matrix.splitlines():
            match = re.match(r"^\|\s*(C\d{2})\s*\|", line)
            if match:
                cells = [c.strip() for c in line.strip().strip("|").split("|")]
                cls.capability_rows[match.group(1)] = cells
                continue
            match = re.match(r"^\|\s*(G\d{2})\s*\|", line)
            if match:
                cls.gap_rows[match.group(1)] = [
                    c.strip() for c in line.strip().strip("|").split("|")
                ]

    def assert_all(self, source: str, *tokens: str) -> None:
        for token in tokens:
            with self.subTest(token=token):
                self.assertTrue(token in source, f"token ausente: {token}")

    def assert_regex_all(self, source: str, label: str, *patterns: str) -> None:
        for pattern in patterns:
            with self.subTest(label=label, pattern=pattern):
                self.assertRegex(source, pattern, f"regex ausente em {label}: {pattern}")

    def assert_in_both(self, gl_token: str, vk_token: str) -> None:
        self.assertTrue(gl_token in self.gl, f"token ausente em gl.hpp: {gl_token}")
        self.assertTrue(vk_token in self.vk, f"token ausente em vk.hpp: {vk_token}")

    def test_matriz_cobre_todas_as_capacidades(self) -> None:
        self.assertEqual(set(self.capability_rows), set(CAPABILITIES))
        for cap_id, name in CAPABILITIES.items():
            with self.subTest(cap=cap_id):
                cells = self.capability_rows[cap_id]
                self.assertGreaterEqual(len(cells), 6)
                self.assertEqual(cells[1], name)
                self.assertIn(marker(cells[2]), MARKERS)
                self.assertIn(marker(cells[3]), MARKERS)
                self.assertTrue(
                    any(value in cells[4] for value in CLASSIFICATIONS),
                    f"classificação ausente em {cap_id}: {cells[4]}",
                )
                self.assertTrue(cells[5], f"evidência ausente em {cap_id}")

    def test_tabelas_estao_bem_formadas(self) -> None:
        for cap_id, cells in self.capability_rows.items():
            with self.subTest(cap=cap_id):
                self.assertEqual(len(cells), 6, f"tabela quebrada em {cap_id}")
        for gap_id, cells in self.gap_rows.items():
            with self.subTest(gap=gap_id):
                self.assertEqual(len(cells), 8, f"tabela quebrada em {gap_id}")

    def test_quatro_classificacoes_sao_usadas(self) -> None:
        gap_classifications = " ".join(row[3] for row in self.gap_rows.values())
        for value in CLASSIFICATIONS:
            with self.subTest(classification=value):
                self.assertIn(value, gap_classifications)

    def test_plano_tem_ordem_dono_e_risco(self) -> None:
        self.assertIn("## Plano de eliminação", self.matrix)
        expected_gaps = {f"G{n:02d}" for n in range(1, 23)}
        self.assertEqual(set(self.gap_rows), expected_gaps)
        for gap_id, cells in self.gap_rows.items():
            with self.subTest(gap=gap_id):
                self.assertGreaterEqual(len(cells), 8)
                self.assertRegex(cells[4], r"^(P[0-3]|—)$")
                self.assertRegex(cells[6], r"^(baixo|médio|alto|nenhum \(documentado\))$")
                self.assertRegex(cells[5], r"\.(hpp|md)\b")
                self.assertTrue(cells[7], f"critério de pronto ausente em {gap_id}")
                self.assertTrue(
                    any(value in cells[3] for value in CLASSIFICATIONS),
                    f"classificação ausente em {gap_id}: {cells[3]}",
                )

    def test_gaps_referenciados_pela_matriz_existem_no_plano(self) -> None:
        referenced = set(re.findall(r"\bG\d{2}\b", self.matrix))
        self.assertTrue(referenced)
        self.assertLessEqual(referenced, set(self.gap_rows))

    def test_status_dos_gaps_bate_com_o_codigo(self) -> None:
        # Um gap só pode ser marcado RESOLVIDO se o cabeçalho ainda trouxer o
        # símbolo. Pendências não podem herdar o rótulo por acidente.
        for gap_id in sorted(RESOLVED_GAPS):
            with self.subTest(gap=gap_id, status="resolvido"):
                self.assertIn("RESOLVIDO", self.gap_rows[gap_id][1])
                self.assertEqual(self.gap_rows[gap_id][4], "—")
                self.assertIn("RESOLVIDO", self.gap_rows[gap_id][7])
        for gap_id in sorted(OPEN_GAPS):
            with self.subTest(gap=gap_id, status="pendente"):
                self.assertNotIn("RESOLVIDO", self.gap_rows[gap_id][1])
                self.assertRegex(self.gap_rows[gap_id][4], r"^P[0-3]$")
        self.assertEqual(
            set(self.gap_rows),
            RESOLVED_GAPS | OPEN_GAPS | {f"G{n:02d}" for n in range(17, 23)},
        )

    def test_fontes_de_verdade_sao_citadas(self) -> None:
        self.assert_all(
            self.matrix,
            "include/nativeapp/gl.hpp",
            "include/nativeapp/vk.hpp",
            "include/nativeapp/graphics_types.hpp",
            "include/nativeapp/font.hpp",
            "include/nativeapp/image_codecs.hpp",
            "include/nativeapp/animated_image.hpp",
            "include/nativeapp/animated_svg.hpp",
            "docs/graphics-opengl.md",
            "docs/graphics-vulkan.md",
            "docs/image-formats.md",
            "examples/image-demo",
            "examples/nativeapp-vulkan",
        )

    def test_api_comum_existe_nos_dois_headers(self) -> None:
        self.assert_in_both("class GLES2Scene : public Scene",
                            "class VulkanScene : public Scene")
        for token in (
            "bool load_font(",
            "void draw_text(",
            "bool has_font() const",
            "void set_clear_color(",
            "void resize(int width, int height)",
            "void begin_frame()",
            "void present()",
            "void draw(",
            "void render(",
        ):
            with self.subTest(token=token):
                self.assertTrue(token in self.gl, f"token ausente em gl.hpp: {token}")
                self.assertTrue(token in self.vk, f"token ausente em vk.hpp: {token}")

    def test_simbolos_da_matriz_batem_com_os_headers(self) -> None:
        for cap_id, checks in SYMBOL_MARKERS.items():
            for token, expected in checks:
                with self.subTest(cap=cap_id, token=token):
                    cells = self.capability_rows[cap_id]
                    in_gl, in_vk = has(self.gl, token), has(self.vk, token)
                    want_gl, want_vk = {
                        "both": ("[ambos]", "[ambos]"),
                        "gl_only": ("[GL only]", "[ausente]"),
                        "vk_only": ("[ausente]", "[VK only]"),
                    }[expected]
                    self.assertEqual(in_gl, expected in ("both", "gl_only"))
                    self.assertEqual(in_vk, expected in ("both", "vk_only"))
                    self.assertEqual(marker(cells[2]), want_gl)
                    self.assertEqual(marker(cells[3]), want_vk)

    def test_apis_divididas_tem_simbolo_proprio_em_cada_header(self) -> None:
        for cap_id, (gl_token, vk_token) in SPLIT_SYMBOLS.items():
            with self.subTest(cap=cap_id):
                cells = self.capability_rows[cap_id]
                self.assertIn(gl_token, self.gl)
                self.assertNotIn(gl_token, self.vk)
                self.assertIn(vk_token, self.vk)
                self.assertNotIn(vk_token, self.gl)
                self.assertEqual(marker(cells[2]), "[GL only]")
                self.assertEqual(marker(cells[3]), "[VK only]")

    def test_instancing_real_existe_nos_dois_backends(self) -> None:
        self.assert_regex_all(self.gl, "gl.hpp", r"draw_instanced\s*\(")
        self.assert_regex_all(self.vk, "vk.hpp", r"draw_instanced\s*\(")
        # Assinatura comum preservada: std::vector<Mat4> em ambos.
        self.assert_all(self.gl, "std::vector<Mat4>", "glDrawElementsInstanced",
                        "glVertexAttribDivisor")
        self.assert_all(self.vk, "std::vector<Mat4>", "upload_instances",
                        "vkCmdDrawIndexed")
        cells = self.capability_rows["C09"]
        self.assertEqual(marker(cells[2]), "[ambos]")
        self.assertEqual(marker(cells[3]), "[ambos]")

    def test_tipos_e_decodificadores_comuns_existem(self) -> None:
        self.assert_all(
            TYPES.read_text(encoding="utf-8"),
            "struct Vertex {",
            "struct MeshData {",
            "struct Material {",
            "class Scene {",
            "static constexpr std::size_t max_vertices = 65536;",
        )
        self.assert_all(
            FONT.read_text(encoding="utf-8"),
            "class FontAtlas {",
            "static constexpr int kCharCount = 95;",
            "GlyphQuad quad(",
        )
        self.assert_all(
            CODECS.read_text(encoding="utf-8"),
            "enum class ImageFormat",
            "decode_asset_image(",
            "decode_svg_auto(",
            "decode_webp_full(",
        )
        self.assert_all(
            ANIM_IMAGE.read_text(encoding="utf-8"),
            "class AnimatedImage {",
            "bool update(float dt_ms);",
            "size_t frame_count() const",
            "const Frame& current_frame() const",
        )
        self.assert_all(
            ANIM_SVG.read_text(encoding="utf-8"),
            "class AnimatedSvg {",
            "bool update(float dt_ms);",
            "const DecodedImage& current() const",
            "size_t animation_count() const",
        )

    def test_exemplos_sustentam_os_gaps_de_animacao_e_texto(self) -> None:
        self.assert_all(
            IMAGE_DEMO.read_text(encoding="utf-8"),
            "draw_with_texture(",
            "AnimatedImage",
            "AnimatedSvg",
        )
        self.assert_all(
            VK_EXAMPLE.read_text(encoding="utf-8"),
            'load_font("font.ttf"',
            "draw_text(",
            "scene_.render()",
        )


if __name__ == "__main__":
    unittest.main()
