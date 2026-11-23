"""Minimal AndroidManifest.xml parsing for build-time decisions."""

from __future__ import annotations

import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path

_ANDROID_NS = "http://schemas.android.com/apk/res/android"
_TRUE_VALUES = frozenset({"true", "1"})


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
    """Atributos por nome local, priorizando o namespace android.

    ElementTree expande ``android:name`` para ``{uri}name``. Manifestos com
    prefixo alternativo, com namespace padrão ou sem prefixo são aceitos;
    ``android:name`` literal (quando declarado como atributo sem namespace)
    continua acessível pela chave original.
    """
    result: dict[str, str] = {}
    other: dict[str, str] = {}
    android_prefix = "{" + _ANDROID_NS + "}"
    for key, value in elem.attrib.items():
        local = _strip_ns(key)
        if key.startswith(android_prefix) or key == local:
            result[local] = value
        else:
            other.setdefault(local, value)
    for key, value in other.items():
        result.setdefault(key, value)
    return result


def _as_bool(value: str | None, default: bool = True) -> bool:
    if value is None:
        return default
    return value.strip().lower() in _TRUE_VALUES


def _is_native_activity_name(name: str) -> bool:
    """android.app.NativeActivity e subclasses, sem casar com rótulos/helpers."""
    return name.strip().endswith("NativeActivity")


def parse_manifest(path: Path) -> ManifestInfo:
    try:
        root = ET.parse(path).getroot()
    except ET.ParseError as exc:
        hint = ""
        if "unbound prefix" in str(exc).lower():
            hint = ' (declare xmlns:android="http://schemas.android.com/apk/res/android")'
        raise RuntimeError(f"Manifesto inválido ({path}): {exc}{hint}") from exc
    except OSError as exc:
        raise RuntimeError(f"Manifesto ilegível ({path}): {exc}") from exc

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
            if _strip_ns(child.tag) != "activity":
                continue
            a = _local_attrs(child)
            name = a.get("name") or a.get("android:name")
            if not name:
                continue
            activities.append(
                ActivityInfo(
                    name=name,
                    exported=_as_bool(a.get("android:exported", a.get("exported"))),
                    attributes=a,
                )
            )

    def names_of(tag: str) -> list[str]:
        names: list[str] = []
        for child in root:
            if _strip_ns(child.tag) != tag:
                continue
            child_attrs = _local_attrs(child)
            name = child_attrs.get("name") or child_attrs.get("android:name")
            if name:
                names.append(name)
        return names

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
        uses_features=names_of("uses-feature"),
        uses_permissions=names_of("uses-permission"),
    )


def is_native_activity(manifest: ManifestInfo) -> bool:
    """Heurística: NativeActivity é declarada via android.app.NativeActivity."""
    for activity in manifest.activities:
        if _is_native_activity_name(activity.name):
            return True
    app_name = (
        manifest.application_attrs.get("name")
        or manifest.application_attrs.get("android:name")
        or ""
    )
    return _is_native_activity_name(app_name)
