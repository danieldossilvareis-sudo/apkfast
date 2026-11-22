"""Minimal AndroidManifest.xml parsing for build-time decisions."""

from __future__ import annotations

import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class ActivityInfo:
    name: str
    exported: bool = True
    attributes: dict[str, str] = field(default_factory=dict)


@dataclass
class ManifestInfo:
    package: str
    version_code: str | None
    version_name: str | None
    min_sdk: str | None
    target_sdk: str | None
    application_attrs: dict[str, str]
    activities: list[ActivityInfo]
    uses_features: list[str]
    uses_permissions: list[str]


def _strip_ns(tag: str) -> str:
    return tag.split("}", 1)[-1] if "}" in tag else tag


def _local_attrs(elem: ET.Element) -> dict[str, str]:
    return {_strip_ns(k): v for k, v in elem.attrib.items()}


def parse_manifest(path: Path) -> ManifestInfo:
    try:
        root = ET.parse(path).getroot()
    except ET.ParseError as exc:
        raise RuntimeError(f"Manifesto inválido ({path}): {exc}") from exc

    if _strip_ns(root.tag) != "manifest":
        raise RuntimeError(f"Raiz do manifesto deve ser <manifest>: {path}")

    attrs = _local_attrs(root)
    package = attrs.get("package")
    if not package:
        raise RuntimeError(f"AndroidManifest.xml sem atributo package: {path}")

    uses_sdk = None
    application = None
    for child in root:
        tag = _strip_ns(child.tag)
        if tag == "uses-sdk" and uses_sdk is None:
            uses_sdk = _local_attrs(child)
        elif tag == "application" and application is None:
            application = child

    app_attrs = _local_attrs(application) if application is not None else {}
    activities: list[ActivityInfo] = []
    if application is not None:
        for child in application:
            if _strip_ns(child.tag) == "activity":
                a = _local_attrs(child)
                name = a.get("android:name") or a.get("name")
                if not name:
                    continue
                exported_raw = a.get("android:exported", a.get("exported", "true"))
                activities.append(
                    ActivityInfo(
                        name=name,
                        exported=exported_raw.lower() in ("true", "1"),
                        attributes=a,
                    )
                )

    sdk_attrs = uses_sdk or {}
    return ManifestInfo(
        package=package,
        version_code=attrs.get("android:versionCode") or attrs.get("versionCode"),
        version_name=attrs.get("android:versionName") or attrs.get("versionName"),
        min_sdk=sdk_attrs.get("android:minSdkVersion") or sdk_attrs.get("minSdkVersion"),
        target_sdk=sdk_attrs.get("android:targetSdkVersion")
        or sdk_attrs.get("targetSdkVersion"),
        application_attrs=app_attrs,
        activities=activities,
        uses_features=[
            _local_attrs(c).get("android:name", _local_attrs(c).get("name", ""))
            for c in root
            if _strip_ns(c.tag) == "uses-feature"
        ],
        uses_permissions=[
            _local_attrs(c).get("android:name", _local_attrs(c).get("name", ""))
            for c in root
            if _strip_ns(c.tag) == "uses-permission"
        ],
    )


def is_native_activity(manifest: ManifestInfo) -> bool:
    """Heuristic: native-activity is declared via android.app.NativeActivity."""
    for activity in manifest.activities:
        if "NativeActivity" in activity.name:
            return True
    return "NativeActivity" in str(manifest.application_attrs)
