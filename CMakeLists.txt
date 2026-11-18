cmake_minimum_required(VERSION 3.20)
project(wii_launcher CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

enable_testing()
add_executable(wl_tests tests/tests.cpp)
target_link_libraries(wl_tests PRIVATE wl_core)
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

    add_executable(wii_launcher WIN32 app/main.cpp app/prefs.cpp)
    target_link_libraries(wii_launcher PRIVATE wl_core wl_imgui)
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
