# Fase 1: repositorio y política de publicación

## Repositorio propio del launcher (no mono-repo)

Los juegos ya viven en sus propios repos (`mp9-recomp`, `wiiparty-recomp`, `mkwii-recomp`/WiiCompiled) con builds de
~40 min, herramientas pesadas y material que no se puede publicar. El launcher es pequeño (48 archivos, ~4 MB con ImGui) y
compila en ~2 min sin tocar ningún juego. Un repo aparte `wii-launcher`:

- permite que un cambio de interfaz dispare **solo** el CI del launcher;
- deja las Releases del launcher limpias (sin instaladores ni servidores);
- no colisiona con los repos de juegos ni con el trabajo de otros hilos.

El contrato con cada juego sigue siendo el de `core/include/wl/adapter.h` + `manifests/*.toml`.

## Qué se versiona y qué no

Se versiona: `core/`, `adapters/`, `app/`, `updater/`, `tools/`, `tests/`, `manifests/`, `third_party/imgui/` (MIT),
`packaging/`, `docs/`, `.github/`, `CMakeLists.txt`, `CHANGELOG.md`, `README.md`, `LICENSE`.

No se versiona (ver `.gitignore`): `build*/`, `dist/`, ejecutables, ZIPs, discos y volcados de Wii (`*.iso`, `*.wbfs`,
`*.rvz`, `*.dol`, ...), claves (`*.key`, `*.pem`, `*.pfx`, `updater-signing*`), registros, `prefs.ini`, `parches/` y
`build-tools/` (pertenecen a los repos de los juegos).

Antes de cada push: `git ls-files` y `git diff --stat origin/main..` revisados a mano. El historial nació limpio el
18/11/2026 (no hay secretos previos que sanear).

## Ramas, commits y versiones

- `main`: siempre compila y pasa tests. Solo se fusiona con aprobación del propietario.
- `feature/...`, `fix/...`: trabajo diario. Commits pequeños en español, cuerpo con el porqué.
- Etiquetas `vMAJOR.MINOR.PATCH` (estable) y `vMAJOR.MINOR.PATCH-beta.N` (beta). **Una etiqueta = una release.**
  Un commit o un push nunca publica nada.
- La versión se cambia en `CMakeLists.txt` (`project(wii_launcher VERSION ...)`) y en `CHANGELOG.md` en el mismo commit.
- `v0.1.0-clase`: estado repartido en clase, sin updater. `v1.0.0`: primera versión con updater (versión puente).

## Visibilidad

Recomendación: **público**. Las descargas de Releases públicas no necesitan credenciales y `releases/latest/download/`
no consume la cuota de la API (importante en clase, donde todos comparten IP). Un repo privado obligaría a meter un token
en el cliente (prohibido) o a montar otra distribución. El código no contiene material de Nintendo (auditoría §5).
**Decidido por el propietario el 18/11/2026: público, `hernandezmarkel/wii-launcher`.**

## Pasos para crear el remoto (solo con autorización)

```bash
gh repo create hernandezmarkel/wii-launcher --public --source . --remote origin --description "Launcher unificado para ports nativos de Wii en PC"
git push -u origin main
git push -u origin feature/launcher-autoupdate
```

Después, en GitHub: Settings → Environments → `release` con revisor obligatorio (el propietario); Settings → Secrets →
`WL_UPDATE_SIGNING_KEY` (en el entorno `release`, no en el repo); reglas de etiquetas `v*` protegidas.
