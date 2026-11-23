# Checklist de release

A versão vive em `apkfast/__init__.py` (`__version__`) e é lida por
`pyproject.toml`, CLI e lockfile. Não incrementá-la em nenhum outro lugar.

## Antes da tag

- [ ] `python3 -m unittest discover -s tests -v` (todos os testes passam).
- [ ] `python3 -m compileall -q apkfast apkfast.py`.
- [ ] `python3 apkfast.py --version` → versão nova.
- [ ] `python3 apkfast.py config show examples/native-hello` → sem erros.
- [ ] `python3 apkfast.py --completions bash|zsh|fish` → scripts gerados.
- [ ] `CI` verde (`ci.yml`), incluindo validação de lockfile/config.
- [ ] `Device matrix` verde (`device.yml`): API 21/28/36 × arm64-v8a/
      armeabi-v7a, com logcat sem `FATAL`/`dlopen failed`.
- [ ] `apkfast build . --generate-lock` funciona e o lockfile está coerente.
- [ ] `pip install -e .` e `apkfast --version` funcionam em Python 3.11+.
- [ ] Wheel de teste contém `include/`, `src/`, `patches/` e `docs/`
      (ver `pyproject.toml` / `MANIFEST.in`).
- [ ] `CHANGELOG.md`, `README.md` e `docs/compatibility.md` atualizados,
      incluindo mudanças de API e prazos de depreciação.
- [ ] `THIRD_PARTY_NOTICES.md` revisado se alguma dependência mudou.
- [ ] Nenhum keystore, token ou `APKFAST_CACHE_HMAC` no repositório.

## Publicação

```sh
git tag -a vX.Y.Z -m "vX.Y.Z"
git push origin vX.Y.Z
gh release create vX.Y.Z --generate-notes
python3 -m pip install build
python3 -m build               # sdist + wheel
python3 -m pip install twine
twine check dist/*
twine upload dist/*
```

> A publicação no PyPI depende de credenciais de mantenedor e é feita
> manualmente por quem detém o acesso. Este checklist não executa `git push`,
> tag ou upload automaticamente.

## Depois da tag

- [ ] Verificar `pip install apkfast==X.Y.Z` em ambiente limpo.
- [ ] `apkfast --version` e `apkfast doctor --json` no ambiente limpo.
- [ ] Anexar o lockfile de referência e o relatório da matriz à release.
- [ ] Atualizar a tabela de suporte em `SECURITY.md`.
