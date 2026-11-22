"""Gerenciamento de dispositivos adb e instalação de APKs."""

from __future__ import annotations

import os
import shutil
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path

HOME = Path(os.environ.get("HOME", str(Path.home())))


class DeviceError(RuntimeError):
    pass


@dataclass
class Device:
    serial: str
    state: str
    model: str = ""
    product: str = ""

    @property
    def online(self) -> bool:
        return self.state == "device"

    def label(self) -> str:
        parts = [self.serial]
        if self.model:
            parts.append(self.model)
        return " ".join(parts)


def find_adb(explicit: str | None = None) -> Path:
    if explicit:
        path = Path(explicit).expanduser()
        if path.is_file() and os.access(path, os.X_OK):
            return path
        raise DeviceError(f"adb não executável: {path}")
    candidates = [
        HOME / "Android" / "android-sdk" / "platform-tools" / "adb",
        HOME / "Android" / "Sdk" / "platform-tools" / "adb",
    ]
    for path in candidates:
        if path.is_file():
            return path
    found = shutil.which("adb")
    if found:
        return Path(found)
    raise DeviceError(
        "adb não encontrado. Instale android-platform-tools ou use --adb /caminho/adb"
    )


def _run(adb: Path, args: list[str], *, timeout: int = 30) -> subprocess.CompletedProcess:
    try:
        return subprocess.run(
            [str(adb), *args],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as exc:
        raise DeviceError(f"adb {args[0]} expirou após {timeout}s") from exc
    except OSError as exc:
        raise DeviceError(f"Falha ao executar adb: {exc}") from exc


def list_devices(adb: Path) -> list[Device]:
    result = _run(adb, ["devices", "-l"])
    if result.returncode != 0:
        raise DeviceError(f"adb devices falhou:\n{result.stdout.strip()}")
    devices: list[Device] = []
    lines = result.stdout.splitlines()
    if lines and "List of devices" in lines[0]:
        lines = lines[1:]
    for line in lines:
        line = line.strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        serial, state = parts[0], parts[1]
        model = ""
        product = ""
        for token in parts[2:]:
            if token.startswith("model:"):
                model = token.split(":", 1)[1]
            elif token.startswith("product:"):
                product = token.split(":", 1)[1]
        devices.append(Device(serial, state, model, product))
    return devices


def restart_server(adb: Path) -> None:
    """Reinicia o servidor adb quando há estados travados/offline."""
    _run(adb, ["kill-server"])
    _run(adb, ["start-server"])
    time.sleep(1.5)


def resolve_device(
    adb: Path,
    serial: str | None = None,
    *,
    try_restart: bool = True,
) -> Device:
    devices = list_devices(adb)

    if serial:
        for device in devices:
            if device.serial == serial:
                if device.online:
                    return device
                if try_restart:
                    restart_server(adb)
                    for refreshed in list_devices(adb):
                        if refreshed.serial == serial and refreshed.online:
                            return refreshed
                raise DeviceError(
                    f"Dispositivo {serial} está '{device.state}'. "
                    "Verifique a depuração USB / autorização na tela do aparelho."
                )
        raise DeviceError(
            f"Dispositivo {serial} não aparece em 'adb devices'. "
            f"Disponíveis: {', '.join(d.label() for d in devices) or 'nenhum'}"
        )

    online = [d for d in devices if d.online]
    if online:
        # Prefere dispositivo USB real a emulador.
        for device in online:
            if not device.serial.startswith("emulator-"):
                return device
        return online[0]

    if not devices:
        if try_restart:
            restart_server(adb)
            devices = list_devices(adb)
            online = [d for d in devices if d.online]
            if online:
                return online[0]
        raise DeviceError(
            "Nenhum dispositivo conectado. Conecte o aparelho via USB com "
            "depuração USB ativada, ou use 'adb connect <ip>:5555'."
        )

    states = ", ".join(f"{d.serial}={d.state}" for d in devices)
    if try_restart:
        restart_server(adb)
        devices = list_devices(adb)
        online = [d for d in devices if d.online]
        if online:
            return online[0]
        states = ", ".join(f"{d.serial}={d.state}" for d in devices)

    if any(d.state == "unauthorized" for d in devices):
        raise DeviceError(
            f"Dispositivo não autorizado ({states}). "
            "Aceite a solicitação 'Permitir depuração USB?' no aparelho."
        )
    raise DeviceError(
        f"Dispositivo offline ({states}). Reconecte o USB ou autorize a depuração."
    )


def install(
    apk: Path,
    *,
    adb_path: str | None = None,
    serial: str | None = None,
    reinstall: bool = True,
    allow_downgrade: bool = False,
    grant_permissions: bool = False,
) -> str:
    if not apk.is_file():
        raise DeviceError(f"APK não encontrado: {apk}")
    adb = find_adb(adb_path)
    device = resolve_device(adb, serial)

    cmd = [str(adb), "-s", device.serial, "install"]
    if reinstall:
        cmd.append("-r")
    if allow_downgrade:
        cmd.append("-d")
    if grant_permissions:
        cmd.append("-g")
    cmd.append(str(apk))

    result = _run(adb, [c for c in cmd[1:]], timeout=180)
    output = result.stdout.strip()
    if result.returncode != 0 or "Failure" in output:
        raise DeviceError(f"Falha ao instalar em {device.label()}:\n{output}")
    return output or "Success"
