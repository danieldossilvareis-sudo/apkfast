"""Integração do lockfile no builder (sem SDK real)."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from apkfast import lockfile as lf
from apkfast.builder import BuildError, BuildOptions, build

MANIFEST = """<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="com.example.nativehello">
  <application android:label="Hello" android:hasCode="false">
    <activity android:name="android.app.NativeActivity" android:exported="true">
      <intent-filter>
        <action android:name="android.intent.action.MAIN" />
        <category android:name="android.intent.category.LAUNCHER" />
      </intent-filter>
    </activity>
  </application>
</manifest>
"""


class BuilderLockTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.project = Path(self.tmp.name)
        (self.project / "AndroidManifest.xml").write_text(MANIFEST)
        self.tc = SimpleNamespace(
            ndk_dir=Path("/sdk/android-ndk-r30"),
            build_tools_dir=Path("/sdk/build-tools/36.0.0"),
            platform_dir=Path("/sdk/platforms/android-36"),
            api=36,
        )

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_divergent_lock_fails_before_build(self) -> None:
        doc = lf.generate(self.project, self.tc, api=36)
        doc["buildTools"] = "35.0.0"
        lf.write(self.project, doc)
        other = SimpleNamespace(
            ndk_dir=self.tc.ndk_dir,
            build_tools_dir=Path("/sdk/build-tools/36.0.0"),
            platform_dir=self.tc.platform_dir,
            api=36,
        )
        with patch("apkfast.builder.detect", return_value=other):
            with self.assertRaisesRegex(BuildError, "divergiu"):
                build(BuildOptions(project=self.project, api=36, quiet=True))

    def test_missing_lock_does_not_block(self) -> None:
        # A divergência é o que falha; ausência de lockfile é opcional.
        with patch("apkfast.builder.detect", return_value=self.tc):
            with patch("apkfast.builder.cache_mod.resolve_context") as resolve:
                resolve.side_effect = RuntimeError("parou antes de tocar no cache")
                with self.assertRaisesRegex(RuntimeError, "parou antes"):
                    build(BuildOptions(project=self.project, api=36, quiet=True))
        resolve.assert_called_once()

    def test_config_error_from_builder(self) -> None:
        (self.project / "apkfast.json").write_text('{"cacheMod": "verify"}')
        with self.assertRaisesRegex(BuildError, "chave desconhecida"):
            build(BuildOptions(project=self.project, api=36, quiet=True))


if __name__ == "__main__":
    unittest.main()
