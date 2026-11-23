#!/usr/bin/env python3
"""Entry point executável do apkfast.

A lógica fica em ``apkfast.cli.main``; aqui só garantimos que o pacote ao lado
deste arquivo seja importado mesmo quando o script é chamado por symlink ou de
outro diretório.
"""

import sys
from pathlib import Path

_ROOT = str(Path(__file__).resolve().parent)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

try:
    from apkfast.cli import main  # noqa: E402 - sys.path ajustado acima
except ImportError as exc:
    raise SystemExit(f"[✗] Não foi possível importar apkfast ({_ROOT}): {exc}") from exc

if __name__ == "__main__":
    raise SystemExit(main())
