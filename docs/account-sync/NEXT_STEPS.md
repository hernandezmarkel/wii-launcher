# Reanudación de cuentas y sincronización

1. Abrir el worktree `wii-launcher-account-sync` y comprobar `git status --short --branch` y `git log -5 --oneline`.
2. Cargar el toolchain con `. .\tools\entorno-mods.ps1`.
3. Configurar núcleo con `cmake -S . -B build-account -G Ninja "-DWL_BUILD_APP=OFF" "-DCMAKE_BUILD_TYPE=Release"`, compilar con `cmake --build build-account -j 4` y ejecutar `ctest --test-dir build-account --output-on-failure`.
4. Implementar autenticación sobre transporte inyectable, proteger la sesión con DPAPI y conectar la UI sin red en el hilo principal.
5. Confirmar disponibilidad de Docker y conectar solo Supabase local o el proyecto de staging identificado. Probar con cuentas ficticias y dos raíces aisladas.
