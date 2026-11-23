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
    not_executable: Path | None = None
    for path in candidates:
        if not path.is_file():
            continue
        if os.access(path, os.X_OK):
            return path
        not_executable = not_executable or path
    if not_executable is not None:
        raise DeviceError(f"adb não executável: {not_executable}")
    found = shutil.which("adb")
    if found:
        return Path(found)
    raise DeviceError(
        "adb não encontrado. Instale android-platform-tools ou use --adb /caminho/adb"
    )


def _run(
    adb: Path,
    args: list[str],
    *,
    timeout: int = 30,
    label: str | None = None,
) -> subprocess.CompletedProcess[str]:
    """Executa um subcomando adb; erros de IO/timeout viram DeviceError.

    ``label`` evita mensagens como "adb -s expirou" quando o comando começa
    com opções globais (ex.: ``-s <serial> install``).
    """
    try:
        return subprocess.run(
            [str(adb), *args],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as exc:
        command = label or (args[0] if args else "comando")
        raise DeviceError(f"adb {command} expirou após {timeout}s") from exc
    except OSError as exc:
        raise DeviceError(f"Falha ao executar adb ({adb}): {exc}") from exc


def list_devices(adb: Path) -> list[Device]:
    result = _run(adb, ["devices", "-l"])
    if result.returncode != 0:
        raise DeviceError(f"adb devices falhou:\n{result.stdout.strip()}")
    lines = result.stdout.splitlines()
    header = next(
        (index for index, line in enumerate(lines) if "List of devices" in line),
        None,
    )
    if header is None:
        raise DeviceError(
            "Saída inesperada de 'adb devices' (sem cabeçalho):\n"
            f"{result.stdout.strip()}"
        )

    devices: list[Device] = []
    # Ignora mensagens do daemon ("starting now", "server didn't ACK", ...)
    # que podem vir ANTES do cabeçalho em versões recentes do adb.
    for line in lines[header + 1 :]:
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
    for command in ("kill-server", "start-server"):
        result = _run(adb, [command], label=command)
        if result.returncode != 0:
            raise DeviceError(f"adb {command} falhou:\n{result.stdout.strip()}")
    time.sleep(1.5)


def _prefer_usb(devices: list[Device]) -> Device:
    """Prefere aparelho USB/real a emulador; ``devices`` não pode estar vazio."""
    for device in devices:
        if not device.serial.startswith("emulator-"):
            return device
    return devices[0]


def _state_hint(state: str) -> str:
    if state == "unauthorized":
        return "Aceite a solicitação 'Permitir depuração USB?' no aparelho."
    return "Verifique a depuração USB / autorização na tela do aparelho."


def resolve_device(
    adb: Path,
    serial: str | None = None,
    *,
    try_restart: bool = True,
) -> Device:
    devices = list_devices(adb)

    if serial:
        for device in devices:
            if device.serial != serial:
                continue
            if device.online:
                return device
            state = device.state
            if try_restart:
                restart_server(adb)
                for refreshed in list_devices(adb):
                    if refreshed.serial == serial:
                        if refreshed.online:
                            return refreshed
                        state = refreshed.state
                        break
            raise DeviceError(f"Dispositivo {serial} está '{state}'. {_state_hint(state)}")
        raise DeviceError(
            f"Dispositivo {serial} não aparece em 'adb devices'. "
            f"Disponíveis: {', '.join(d.label() for d in devices) or 'nenhum'}"
        )

    online = [d for d in devices if d.online]
    if online:
        return _prefer_usb(online)

    if try_restart:
        restart_server(adb)
        devices = list_devices(adb)
        online = [d for d in devices if d.online]
        if online:
            return _prefer_usb(online)

    if not devices:
        raise DeviceError(
            "Nenhum dispositivo conectado. Conecte o aparelho via USB com "
            "depuração USB ativada, ou use 'adb connect <ip>:5555'."
        )

    states = ", ".join(f"{d.serial}={d.state}" for d in devices)
    if any(d.state == "unauthorized" for d in devices):
        raise DeviceError(
            f"Dispositivo não autorizado ({states}). {_state_hint('unauthorized')}"
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

    # Opções globais (-s) vêm antes do subcomando; nada de reconstruir a linha
    # de comando com cmd[1:] (isso também quebrava a mensagem de timeout).
    args = ["-s", device.serial, "install"]
    if reinstall:
        args.append("-r")
    if allow_downgrade:
        args.append("-d")
    if grant_permissions:
        args.append("-g")
    args.append(str(apk))

    result = _run(adb, args, timeout=180, label="install")
    output = result.stdout.strip()
    failed = result.returncode != 0 or any(
        line.lstrip().startswith("Failure") for line in output.splitlines()
    )
    if failed:
        raise DeviceError(f"Falha ao instalar em {device.label()}:\n{output}")
    return output or "Success"
