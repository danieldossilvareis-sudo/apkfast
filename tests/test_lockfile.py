"""Testes do lockfile reprodutível (lockVersion 1)."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from apkfast import lockfile as lf


def fake_toolchain() -> SimpleNamespace:
    return SimpleNamespace(
        ndk_dir=Path("/sdk/android-ndk-r30"),
        build_tools_dir=Path("/sdk/build-tools/36.0.0"),
        platform_dir=Path("/sdk/platforms/android-36"),
        api=36,
    )


class LockfileTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.project = Path(self.tmp.name)
        self.tc = fake_toolchain()

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_generate_write_load_roundtrip(self) -> None:
        doc = lf.generate(self.project, self.tc, api=36)
        target = lf.write(self.project, doc)
        self.assertEqual(target, self.project / lf.LOCK_NAME)
        loaded = lf.load(self.project)
        self.assertIsNotNone(loaded)
        assert loaded is not None
        self.assertEqual(loaded["lockVersion"], 1)
        self.assertEqual(loaded["ndkVersion"], "r30")
        self.assertEqual(loaded["buildTools"], "36.0.0")
        self.assertEqual(loaded["platformApi"], 36)
        self.assertEqual(loaded["ndkApi"], 36)
        self.assertIn("libwebp.tarball", loaded["checksums"])
        self.assertIn("lunaSVG.tarball", loaded["checksums"])
        self.assertEqual(lf.compare(loaded, lf.facts(self.tc, api=36)), [])

    def test_ndk_revision_from_source_properties(self) -> None:
        props = self.project / "ndk"
        props.mkdir()
        (props / "source.properties").write_text("Pkg.Revision = 27.2.12479018\n")
        tc = SimpleNamespace(
            ndk_dir=props,
            build_tools_dir=self.tc.build_tools_dir,
            platform_dir=self.tc.platform_dir,
            api=36,
        )
        self.assertEqual(lf.facts(tc)["ndkVersion"], "27.2.12479018")

    def test_detect_divergence(self) -> None:
        doc = lf.generate(self.project, self.tc, api=36)
        other = SimpleNamespace(
            ndk_dir=Path("/sdk/android-ndk-r30"),
            build_tools_dir=Path("/sdk/build-tools/35.0.0"),
            platform_dir=Path("/sdk/platforms/android-35"),
            api=35,
        )
        mismatches = lf.compare(doc, lf.facts(other, api=35))
        self.assertTrue(any("build-tools" in item for item in mismatches))
        self.assertTrue(any("API da platform" in item for item in mismatches))
        self.assertTrue(any("API do NDK" in item for item in mismatches))
        with self.assertRaisesRegex(lf.LockfileError, "divergiu"):
            lf.verify(doc, lf.facts(other, api=35))
        self.assertEqual(
            lf.verify(doc, lf.facts(other, api=35), ignore=True), mismatches
        )

    def test_corrupted_and_future_lockfile(self) -> None:
        target = self.project / lf.LOCK_NAME
        target.write_text("{ quebrado")
        with self.assertRaises(lf.LockfileError):
            lf.load(self.project)
        doc = lf.generate(self.project, self.tc, api=36)
        doc["lockVersion"] = 99
        with self.assertRaisesRegex(lf.LockfileError, "mais novo"):
            lf.validate(doc)
        target.write_text(json.dumps({"lockVersion": 1, "ndkVersion": "r30"}))
        with self.assertRaisesRegex(lf.LockfileError, "buildTools"):
            lf.load(self.project)

    def test_custom_path_and_required(self) -> None:
        with self.assertRaises(lf.LockfileError):
            lf.load(self.project, required=True)
        custom = self.project / "lock-custom.json"
        doc = lf.generate(self.project, self.tc, api=36)
        lf.write(self.project, doc, output=custom)
        self.assertEqual(lf.load(custom)["lockVersion"], 1)


if __name__ == "__main__":
    unittest.main()
