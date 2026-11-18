#!/usr/bin/env bash
# Falla si el repositorio rastrea algo que no se puede publicar: discos o volcados de Wii, binarios, claves,
# tokens o datos de usuario. Lo ejecuta el CI en cada push y PR, y el release antes de compilar.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
fail=0

# 1. Tipos de archivo prohibidos entre los rastreados.
bad=$(git ls-files | grep -Ei '\.(iso|wbfs|rvz|wia|gcz|ciso|wad|dol|elf|rel|szs|brres|nand|exe|dll|zip|7z|key|pem|pfx|p12|sav)$|(^|/)(keys|otp|seeprom)\.bin$|(^|/)prefs\.ini$|(^|/)\.env' || true)
if [ -n "$bad" ]; then
    echo "ERROR: archivos no publicables rastreados:"; echo "$bad"; fail=1
fi

# 2. Archivos grandes (> 2 MB) fuera de third_party.
while IFS= read -r f; do
    [ -f "$f" ] || continue
    size=$(wc -c < "$f")
    if [ "$size" -gt 2097152 ] && [[ "$f" != third_party/* ]]; then echo "ERROR: archivo grande rastreado: $f ($size bytes)"; fail=1; fi
done < <(git ls-files)

# 3. Secretos: claves privadas PEM, tokens de GitHub y claves privadas del updater (192 hex = X||Y||d).
secrets=$(git grep -nIE -e '-----BEGIN [A-Z ]*PRIVATE KEY-----' -e 'gh[pousr]_[A-Za-z0-9]{30,}' -e 'github_pat_[A-Za-z0-9_]{30,}' -e '(^|[^0-9a-fA-F])[0-9a-fA-F]{192}([^0-9a-fA-F]|$)' -- . ':!tools/comprobar-publicable.sh' || true)
if [ -n "$secrets" ]; then
    echo "ERROR: posible secreto en el repositorio:"; echo "$secrets" | cut -c1-120; fail=1
fi

[ "$fail" -eq 0 ] && echo "OK: nada no publicable en $(git ls-files | wc -l) archivos rastreados."
exit "$fail"
