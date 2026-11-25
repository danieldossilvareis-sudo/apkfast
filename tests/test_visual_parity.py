"""Testes estáticos/estruturais da verificação visual GL × Vulkan (sem GPU)."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "visual-parity.sh"
DEVICE_CHECK = ROOT / "scripts" / "device-check.sh"
DOC = ROOT / "docs" / "visual-parity.md"
GL_MANIFEST = ROOT / "examples" / "nativeapp-gl" / "AndroidManifest.xml"
VK_MANIFEST = ROOT / "examples" / "nativeapp-vulkan" / "AndroidManifest.xml"
VK_EXAMPLE = ROOT / "examples" / "nativeapp-vulkan" / "jni" / "main.cpp"
VK_ASSETS = ROOT / "examples" / "nativeapp-vulkan" / "assets"
MEDIA_ASSETS = ROOT / "examples" / "media-demo" / "assets"

# Elementos que precisam aparecer TANTO na cena GL gerada pelo harness quanto
# no exemplo Vulkan versionado. Se um deles desaparecer, gl.png/vk.png deixam
# de ser comparáveis.
SCENE_ELEMENTS = (
    "set_clear_color(0.10f, 0.20f, 0.35f, 1.0f)",
    "MeshData::triangle()",
    "MeshData::quad(180.0f, 180.0f)",
    "load_font(\"font.ttf\", 28)",
    "draw_text(\"VULKAN OK\", 24.0f, 48.0f, 1.4f",
    "sprite.png",
)


class VisualParityStructureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.script = SCRIPT.read_text(encoding="utf-8")
        cls.doc = DOC.read_text(encoding="utf-8")
        cls.vk_example = VK_EXAMPLE.read_text(encoding="utf-8")
        cls.gl_manifest = GL_MANIFEST.read_text(encoding="utf-8")
        cls.vk_manifest = VK_MANIFEST.read_text(encoding="utf-8")
        gl_match = re.search(r"<<'CPP'\n(.*?)\nCPP\n", cls.script, re.DOTALL)
        cls.gl_scene = gl_match.group(1) if gl_match else ""

    def assert_all(self, source: str, *tokens: str) -> None:
        for token in tokens:
            with self.subTest(token=token):
                self.assertIn(token, source)

    def test_script_e_device_check_existem(self) -> None:
        self.assertTrue(SCRIPT.is_file(), f"faltou {SCRIPT}")
        self.assertTrue(DEVICE_CHECK.is_file(), f"faltou {DEVICE_CHECK}")
        self.assertIn("#!/usr/bin/env sh", self.script.splitlines()[0])

    def test_opcoes_e_saidas_esperadas(self) -> None:
        self.assert_all(
            self.script,
            "--build-dir",
            "--out",
            "--abi",
            "--adb",
            "--serial",
            "--extras",
            "gl.png",
            "vk.png",
            "gl.apk",
            "vk.apk",
            "src/gl_parity_main.cpp",
        )

    def test_build_dos_exemplos_e_packages(self) -> None:
        self.assert_all(
            self.script,
            "examples/nativeapp-gl",
            "examples/nativeapp-vulkan",
            "examples/image-demo",
            "examples/media-demo",
            "com.apkfast.nativeappgl",
            "com.apkfast.nativeappvk",
            "com.apkfast.imagedemo",
            "com.apkfast.mediademo",
        )
        self.assertIn('package="com.apkfast.nativeappgl"', self.gl_manifest)
        self.assertIn('package="com.apkfast.nativeappvk"', self.vk_manifest)

    def test_instalacao_captura_e_fatal(self) -> None:
        self.assert_all(
            self.script,
            "scripts/device-check.sh",
            "install -r",
            "adb exec-out screencap -p",
            "dumpsys window",
            "89504e470d0a1a0a",
            "FATAL|dlopen failed|AndroidRuntime.*Exception",
            "abi(device)",
            "abi(build)",
            "platform-tools/adb",
            "emulator-5554",
        )

    def test_harness_gera_as_duas_capturas(self) -> None:
        # Não basta citar os nomes: o script precisa chamar a captura para os
        # dois rótulos e gravar em "$OUT/gl.png" / "$OUT/vk.png".
        self.assert_all(
            self.script,
            'capture_one gl com.apkfast.nativeappgl "$OUT/gl.png"',
            'capture_one vk com.apkfast.nativeappvk "$OUT/vk.png"',
            'adb_run exec-out screencap -p >"$vp_png"',
            '[ "$vp_sig" = "89504e470d0a1a0a" ]',
            'note "captura $vp_label: $vp_png"',
        )
        self.assert_all(
            self.script,
            "build_one gl examples/nativeapp-gl com.apkfast.nativeappgl",
            "build_one vk examples/nativeapp-vulkan com.apkfast.nativeappvk",
        )
        self.assert_all(
            self.doc,
            "gl.png",
            "vk.png",
            "adb exec-out screencap -p",
            "89504e470d0a1a0a",
        )

    def test_cena_gl_gerada_existe_e_tem_elementos_combinados(self) -> None:
        self.assertTrue(self.gl_scene, "heredoc CPP da cena GL não encontrado")
        self.assert_all(self.gl_scene, "class ParityApp : public App",
                        "GLES2Scene scene_", "void on_draw() override")
        for token in SCENE_ELEMENTS:
            with self.subTest(origem="gl", token=token):
                self.assertIn(token, self.gl_scene)

    def test_exemplo_vk_tem_elementos_combinados(self) -> None:
        for token in SCENE_ELEMENTS:
            with self.subTest(origem="vk", token=token):
                self.assertIn(token, self.vk_example)

    def test_cena_gl_gerada_usa_mesmos_parametros_do_vulkan(self) -> None:
        # A cena GL é gerada dentro do script (heredoc) e injetada via --src.
        self.assert_all(
            self.script,
            'set_scene(&scene_)',
            "w * 0.5f",
            "h * 0.35f",
            "w * 0.72f",
            "h * 0.55f",
            "--src",
            "--assets",
            "--build-dir",
        )
        self.assert_all(
            self.gl_scene,
            'set_scene(&scene_)',
            'load_font("font.ttf", 28)',
            'textured.texture = "sprite.png"',
            'draw_text("VULKAN OK", 24.0f, 48.0f, 1.4f',
        )
        self.assert_all(
            self.vk_example,
            'load_texture("sprite.png")',
            'load_font("font.ttf", 28)',
            'draw_text("VULKAN OK", 24.0f, 48.0f, 1.4f',
            "scene_.render()",
        )

    def test_assets_sao_os_mesmos_nos_dois_apks(self) -> None:
        for name in ("sprite.png", "font.ttf"):
            vk_asset = VK_ASSETS / name
            media_asset = MEDIA_ASSETS / name
            with self.subTest(asset=name):
                self.assertTrue(vk_asset.is_file(), f"faltou {vk_asset}")
                self.assertTrue(media_asset.is_file(), f"faltou {media_asset}")
                self.assertEqual(vk_asset.read_bytes(), media_asset.read_bytes())

    def test_documentacao_explica_uso_e_limitacoes(self) -> None:
        self.assert_all(
            self.doc,
            "scripts/visual-parity.sh",
            "--out",
            "--build-dir",
            "adb exec-out screencap -p",
            "gl.png",
            "vk.png",
            "com.apkfast.nativeappgl",
            "com.apkfast.nativeappvk",
            "Limitações",
            "Screenshot",
            "pixel-diff",
        )
        self.assertIn("Diferenças observadas", self.doc)
        self.assertIn("Estado final", self.doc)
        self.assertIn("1/255", self.doc)


if __name__ == "__main__":
    unittest.main()
