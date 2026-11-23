"""Gerador de scripts de conclusão (bash/zsh/fish) para o CLI do apkfast."""

from __future__ import annotations

from typing import Any

SHELLS = ("bash", "zsh", "fish")


def _option_strings(parser: Any) -> list[str]:
    result: list[str] = []
    for action in parser._actions:  # argparse não expõe API pública para isso
        opts = [opt for opt in action.option_strings if not opt.startswith("--help")]
        result.extend(opts)
    return result


def _positional_names(parser: Any) -> list[str]:
    result: list[str] = []
    for action in parser._actions:
        if action.option_strings or action.dest in ("help", "==SUPPRESS=="):
            continue
        name = action.metavar or action.dest
        if action.nargs == "?":
            name = f"[{name}]"
        elif action.nargs in ("*", "+"):
            name = f"{name}..."
        result.append(str(name))
    return result


def commands() -> tuple[list[str], dict[str, list[str]]]:
    """Descobre subcomandos e opções diretamente do parser atual."""
    from .cli import create_parser

    parser = create_parser()
    global_options = _option_strings(parser)
    subcommands: dict[str, list[str]] = {}
    for action in parser._actions:
        choices = getattr(action, "choices", None)
        if choices is None or not hasattr(choices, "items"):
            continue
        for name, subparser in choices.items():
            options = global_options + _option_strings(subparser)
            options += [p for p in _positional_names(subparser) if p.startswith("[")]
            subcommands[name] = list(dict.fromkeys(options))
    return list(subcommands), subcommands


def _q(value: str) -> str:
    return "'" + value.replace("'", "'\\''") + "'"


def _bash(names: list[str], subs: dict[str, list[str]]) -> str:
    lines = [
        "# apkfast — conclusão para bash",
        "# Instale: eval \"$(apkfast --completions bash)\"",
        "_apkfast() {",
        "  local cur prev cmd",
        "  cur=\"${COMP_WORDS[COMP_CWORD]}\"",
        "  cmd=\"${COMP_WORDS[1]}\"",
        "  local opts=\"\"",
        f"  local commands=\"{' '.join(names)}\"",
        "  if [[ $COMP_CWORD -eq 1 ]]; then",
        "    opts=\"$commands\"",
        "  else",
        "    case \"$cmd\" in",
    ]
    for name, options in subs.items():
        lines.append(f"      {name}) opts=\"{' '.join(options)}\" ;;")
    lines += [
        "      *) opts=\"--help\" ;;",
        "    esac",
        "  fi",
        "  COMPREPLY=( $(compgen -W \"$opts\" -- \"$cur\") )",
        "}",
        "complete -F _apkfast apkfast",
        "",
    ]
    return "\n".join(lines)


def _zsh(names: list[str], subs: dict[str, list[str]]) -> str:
    lines = [
        "#compdef apkfast",
        "# apkfast — conclusão para zsh",
        "# Instale: eval \"$(apkfast --completions zsh)\"",
        "_apkfast() {",
        f"  local -a commands; commands=({' '.join(_q(n) for n in names)})",
        "  if (( CURRENT == 2 )); then",
        "    _describe 'comando' commands",
        "    return",
        "  fi",
        "  local -a options",
        "  case \"${words[2]}\" in",
    ]
    for name, options in subs.items():
        rendered = " ".join(_q(opt) for opt in options)
        lines.append(f"    {name}) options=({rendered}) ;;")
    lines += [
        "    *) options=(--help) ;;",
        "  esac",
        "  _describe 'opção' options",
        "}",
        "compdef _apkfast apkfast",
        "",
    ]
    return "\n".join(lines)


def _fish(names: list[str], subs: dict[str, list[str]]) -> str:
    lines = [
        "# apkfast — conclusão para fish",
        "# Instale: apkfast --completions fish | source",
        "complete -c apkfast -f",
    ]
    for name in names:
        lines.append(f"complete -c apkfast -n '__fish_use_subcommand' -a {_q(name)}")
    for name, options in subs.items():
        for option in options:
            if option.startswith("--"):
                lines.append(
                    f"complete -c apkfast -n '__fish_seen_subcommand_from {name}' "
                    f"-l {option[2:]}"
                )
            elif option.startswith("-"):
                lines.append(
                    f"complete -c apkfast -n '__fish_seen_subcommand_from {name}' "
                    f"-s {option[1:]}"
                )
    lines.append("")
    return "\n".join(lines)


def generate(shell: str, prog: str = "apkfast") -> str:
    """Devolve o script de conclusão para ``bash``, ``zsh`` ou ``fish``."""
    shell = shell.lower()
    if shell not in SHELLS:
        raise ValueError(f"Shell não suportado: {shell}. Use: {', '.join(SHELLS)}")
    names, subs = commands()
    # `prog` é mantido como parâ público para integrações; o CLI instalado
    # chama o executável de mesmo nome.
    script = {"bash": _bash, "zsh": _zsh, "fish": _fish}[shell](names, subs)
    return script if prog == "apkfast" else script.replace("apkfast", prog)
