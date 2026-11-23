cmake_minimum_required(VERSION 3.20)
# La versión del launcher vive aquí (y en CHANGELOG.md). El workflow de release exige que coincida con la etiqueta.
project(wii_launcher VERSION 1.0.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Ejecutables más pequeños en Release: sin símbolos y sin código no usado (el exe pasa de ~9,8 a ~6 MB).
if(MINGW AND CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-ffunction-sections -fdata-sections)
    add_link_options(-s -Wl,--gc-sections)
endif()

# Núcleo sin UI: catálogo, manifests, adaptadores y procesos. No depende de ningún juego.
add_library(wl_core STATIC
    core/src/manifest.cpp
    core/src/catalog.cpp
    core/src/process.cpp
    core/src/install.cpp
    adapters/mp9_adapter.cpp
    adapters/mp9/room/online.cpp
    adapters/wiiparty/room/lobby.cpp
    adapters/wiiparty/room/sha256.cpp
    adapters/wiiparty_adapter.cpp
    adapters/mkwii_adapter.cpp)
target_include_directories(wl_core PUBLIC core/include PRIVATE adapters adapters/mp9/room)
if(WIN32)
    target_link_libraries(wl_core PUBLIC ws2_32)
endif()
target_compile_options(wl_core PRIVATE -Wall -Wextra)
target_compile_definitions(wl_core PUBLIC WL_MANIFEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/manifests")

add_executable(wlaunch tools/wlaunch.cpp)
target_link_libraries(wlaunch PRIVATE wl_core)

# ---- Autoactualización del launcher (docs/updater/02-adr-arquitectura.md). Solo Windows: WinHTTP + CNG.
set(WL_VERSION_SUFFIX "" CACHE STRING "Sufijo SemVer de pre-release, p. ej. beta.1 (vacío = estable)")
set(WL_UPDATE_REPO "hernandezmarkel/wii-launcher" CACHE STRING "Repositorio GitHub (dueño/repo) de las Releases del launcher")
# Clave pública de firma de releases (P-256, X||Y en hex). La privada NUNCA está en el repo.
# Generada el 18/11/2026 con `wl_release keygen`; rotación en docs/updater/seguridad-y-recuperacion.md.
set(WL_UPDATE_PUBKEY "21087616d80cdb02bc0d82cdf71e4bdda9317d2c9531bf4b9f2dfbe6dd3e3a55222cee1b2266dddeaf1368bc3357a5d818c02dbde4925657aed177bf50557186" CACHE STRING "Clave pública anclada para verificar actualizaciones")
option(WL_UPDATER_TESTING "Build de pruebas: feed por http://127.0.0.1 y fallos simulados. Nunca en releases" OFF)
set(WL_VERSION_FULL "${PROJECT_VERSION}")
if(WL_VERSION_SUFFIX)
    set(WL_VERSION_FULL "${PROJECT_VERSION}-${WL_VERSION_SUFFIX}")
endif()
# Solo para el feed de staging (simular v1.0.0 -> v1.0.1 sin tocar project()). El release lo rechaza.
set(WL_VERSION_OVERRIDE "" CACHE STRING "Versión completa forzada (solo pruebas)")
if(WL_VERSION_OVERRIDE)
    set(WL_VERSION_FULL "${WL_VERSION_OVERRIDE}")
endif()
configure_file(updater/update_config.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/wl_update_config.h)
# Para los scripts de empaquetado y release (comprueban versión, modo de pruebas y clave).
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/version.txt "${WL_VERSION_FULL}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/updater-build.txt "testing=${WL_UPDATER_TESTING}\noverride=${WL_VERSION_OVERRIDE}\nrepo=${WL_UPDATE_REPO}\npubkey=${WL_UPDATE_PUBKEY}\n")
add_library(wl_updater STATIC
    updater/src/common.cpp
    updater/src/update_manifest.cpp
    updater/src/http_win.cpp
    updater/src/feed.cpp
    updater/src/apply.cpp
    updater/src/installers.cpp)
target_include_directories(wl_updater PUBLIC updater/include PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(wl_updater PUBLIC wl_core winhttp bcrypt)
target_compile_options(wl_updater PRIVATE -Wall -Wextra)
target_compile_definitions(wl_updater PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(wl_release tools/wl_release.cpp)
target_link_libraries(wl_release PRIVATE wl_updater)
if(MINGW)
    target_link_options(wl_release PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# ---- Mods (docs/mods/01-arquitectura.md). Subsistema aparte del updater: solo usa su SHA-256 y su HttpClient.
# zlib: en MSYS2 viene con gcc; con el MinGW de nuwen, exportar ZLIB_ROOT=<MinGW> (tools/entorno-mods.ps1).
set(ZLIB_USE_STATIC_LIBS ON)
find_package(ZLIB REQUIRED)
add_library(wl_mods STATIC
    mods/src/json.cpp
    mods/src/safe_path.cpp
    mods/src/zip.cpp
    mods/src/fst.cpp
    mods/src/xml.cpp
    mods/src/package.cpp
    mods/src/store.cpp
    mods/src/launch.cpp
    mods/src/provider.cpp
    mods/src/gamebanana.cpp
    mods/src/session.cpp
    mods/src/lobby_mods.cpp)
target_include_directories(wl_mods PUBLIC mods/include)
target_link_libraries(wl_mods PUBLIC wl_core wl_updater ZLIB::ZLIB)
target_compile_options(wl_mods PRIVATE -Wall -Wextra)
target_compile_definitions(wl_mods PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
add_executable(wlmods tools/wlmods.cpp)
target_link_libraries(wlmods PRIVATE wl_mods)
if(MINGW)
    target_link_options(wlmods PRIVATE -static -static-libgcc -static-libstdc++)
endif()

enable_testing()
add_executable(wl_tests tests/tests.cpp tests/updater_tests.cpp tests/mods_tests.cpp)
target_link_libraries(wl_tests PRIVATE wl_core wl_updater wl_mods)
add_test(NAME wl_tests COMMAND wl_tests WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# ---- Interfaz (opcional: -DWL_BUILD_APP=OFF compila solo el núcleo y los tests)
option(WL_BUILD_APP "Compilar la interfaz SDL3 + Dear ImGui" ON)
if(WL_BUILD_APP)
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    foreach(WL_SDL_OFF SDL_AUDIO SDL_CAMERA SDL_HAPTIC SDL_SENSOR SDL_POWER SDL_TRAY SDL_GPU SDL_RENDER_GPU SDL_VULKAN SDL_RENDER_VULKAN SDL_RENDER_D3D12)
        set(${WL_SDL_OFF} OFF CACHE BOOL "" FORCE)
    endforeach()
    # Misma versión y hash que los launchers actuales. Para no descargar: -DFETCHCONTENT_SOURCE_DIR_SDL3=<carpeta>
    FetchContent_Declare(sdl3
        URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz
        URL_HASH SHA256=7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68
        DOWNLOAD_EXTRACT_TIMESTAMP ON)
    FetchContent_MakeAvailable(sdl3)

    set(IMGUI third_party/imgui)  # Dear ImGui 1.92.9b (MIT), copiado de la misma versión que usan los launchers
    add_library(wl_imgui STATIC ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
        ${IMGUI}/backends/imgui_impl_sdl3.cpp ${IMGUI}/backends/imgui_impl_sdlrenderer3.cpp)
    target_include_directories(wl_imgui PUBLIC ${IMGUI} ${IMGUI}/backends)
    target_compile_definitions(wl_imgui PUBLIC IMGUI_DEFINE_MATH_OPERATORS IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMGUI_DISABLE_DEMO_WINDOWS IMGUI_DISABLE_DEBUG_TOOLS)
    target_link_libraries(wl_imgui PUBLIC SDL3::SDL3-static)

    add_executable(wii_launcher WIN32 app/main.cpp app/prefs.cpp app/update_service.cpp app/mods_service.cpp)
    target_link_libraries(wii_launcher PRIVATE wl_core wl_updater wl_mods wl_imgui)
    target_compile_options(wii_launcher PRIVATE -Wall -Wextra)
    set_target_properties(wii_launcher PROPERTIES OUTPUT_NAME "wii-launcher")
    if(MINGW)
        target_link_options(wii_launcher PRIVATE -static -static-libgcc -static-libstdc++)
    endif()

    # Fuente Inter (OFL). -DWL_INTER_DIR=<carpeta con Inter-Regular.ttf e Inter-SemiBold.ttf>
    set(WL_INTER_DIR "" CACHE PATH "Carpeta con las TTF de Inter")
    if(WL_INTER_DIR)
        add_custom_command(TARGET wii_launcher POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:wii_launcher>/fonts
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${WL_INTER_DIR}/Inter-Regular.ttf ${WL_INTER_DIR}/Inter-SemiBold.ttf $<TARGET_FILE_DIR:wii_launcher>/fonts/)
    endif()
endif()
