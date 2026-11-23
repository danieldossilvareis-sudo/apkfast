# Publicação no PyPI

> **Estado desta entrega:** a automação está pronta, mas **nada foi publicado**.
> `publish.yml` não foi executado; nenhum `twine upload`, tag, commit ou release
> foi feito. A versão continua `2.4.1`.

## Versionamento

A única fonte de verdade é `apkfast/__init__.py` (`__version__`). Ela é lida por
`pyproject.toml`, pela CLI (`apkfast --version`) e pelo lockfile. Use SemVer:

- patch: correção compatível (2.4.1 → 2.4.2);
- minor: feature compatível (2.4.x → 2.5.0);
- major: quebra de API/ABI (2.x → 3.0.0).

A tag segue `vX.Y.Z`. Não incrementar a versão em nenhum outro arquivo.

## Ferramentas locais

```sh
python3 -m pip install -e '.[publish]'   # instala build + twine como extra
```

O extra `publish` está declarado em `pyproject.toml`. Alternativamente:
`python3 -m pip install build twine`.

> Para usar os comandos pela CLI, registre o módulo em `create_parser()` —
> veja as linhas de integração no relatório da Equipe B. Enquanto isso, as
> funções `apkfast.publish.build_dist/check_dist/upload` já são importáveis.

## Comandos locais (seguros)

```sh
# Gera sdist + wheel e valida metadados; NÃO envia nada
apkfast dist . -o dist

# Dry-run (padrão): gera, valida e mostra o plano de upload
apkfast publish-pypi . -o dist --repository testpypi

# Upload real (somente com token no ambiente e confirmação explícita)
PYPI_TOKEN='pypi-...' apkfast publish-pypi . --repository testpypi --yes
PYPI_TOKEN='pypi-...' apkfast publish-pypi . --yes
```

Regras de segurança do `publish-pypi`:

| Situação | Comportamento |
|---|---|
| sem `--yes` | **dry-run**: nenhum subprocess de upload é chamado |
| `--dry-run` | explícito, equivalente ao padrão |
| `--yes` sem token | falha com mensagem clara, antes de chamar o twine |
| `--yes` com token | token vai em `TWINE_USERNAME`/`TWINE_PASSWORD` do subprocess, nunca na linha de comando |
| erro do twine | saída é redigida (`***`) antes de aparecer |

Variáveis aceitas, em ordem de preferência:

1. `TWINE_USERNAME` + `TWINE_PASSWORD`;
2. `TWINE_API_TOKEN`;
3. `PYPI_TOKEN` (vira `__token__` + senha).

`--repository testpypi` usa `https://test.pypi.org/legacy/`. Outros valores são
interpretados como nomes do `~/.pypirc` (`--repository <nome>`).

## Fluxo GitHub Release → PyPI

1. Atualizar `CHANGELOG.md`, `README.md` e `docs/compatibility.md`.
2. Rodar os testes e a matriz (ver `RELEASE.md`).
3. Conferir o dry-run local: `apkfast dist` + `apkfast publish-pypi`.
4. Criar a tag e publicar a release:

   ```sh
   git tag -a vX.Y.Z -m "vX.Y.Z"
   git push origin vX.Y.Z
   gh release create vX.Y.Z --generate-notes
   ```

5. O evento `release: [published]` dispara `.github/workflows/publish.yml`:

   - `build`: checkout → setup-python 3.11 → `pip install build twine` →
     `python -m build` → `twine check dist/*` → artefato `python-dist`;
   - `publish`: baixa o artefato e publica com
     `pypa/gh-action-pypi-publish@release/v1` (OIDC) ou com
     `secrets.PYPI_API_TOKEN`.

6. TestPyPI sem release: `workflow_dispatch` em `publish.yml`, escolhendo
   `repository=testpypi` e `dry_run=true` primeiro; depois `dry_run=false`.

## Trusted Publishing (OIDC)

No PyPI (e novamente no TestPyPI, se aplicável):

1. abra **Settings → Publishing → Add a trusted publisher**;
2. preencha Owner, Repository `apkfast`, Workflow `publish.yml`;
3. use o environment `pypi` (o mesmo usado no job `publish`);
4. mantenha `permissions: id-token: write` no workflow.

Com OIDC configurado, **não** defina `PYPI_API_TOKEN`: o job de token fica
skipado e o de OIDC publica. Se o token existir, ele tem precedência (útil para
migração). Não imprima nem comite o token.

## Checklist de release

- [ ] `__version__` atualizado somente em `apkfast/__init__.py`.
- [ ] `python3 -m unittest discover -s tests -v` verde.
- [ ] `python3 -m compileall -q apkfast apkfast.py` verde.
- [ ] `CI` e `Build matrix` verdes.
- [ ] `docs/device-ci.md`: validação em dispositivo executada.
- [ ] `apkfast dist` gera 2 artefatos e `twine check` passa.
- [ ] `apkfast publish-pypi` em dry-run revisado (sem secrets no log).
- [ ] Release publicada no GitHub com notas geradas.
- [ ] `publish.yml` verde; `pip install apkfast==X.Y.Z` testado em ambiente limpo.
- [ ] `apkfast --version` e `apkfast doctor --json` no ambiente limpo.
