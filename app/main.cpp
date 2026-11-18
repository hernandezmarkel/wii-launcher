// Launcher unificado: biblioteca de juegos, ficha por juego y sala online.
// La UI solo habla con el contrato de adaptadores (wl/adapter.h); no sabe nada de cada juego.
// Bucle por eventos: sin entrada ni trabajo pendiente la ventana no redibuja (CPU ~0 en reposo).
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "prefs.h"
#include "wl/catalog.h"
#include "wl/process.h"

using namespace wl;
using Clock = std::chrono::steady_clock;

namespace {

// ---------------------------------------------------------------- estilo

struct Palette {
    ImU32 bg = IM_COL32(16, 18, 24, 255);
    ImU32 sidebar = IM_COL32(22, 25, 33, 255);
    ImU32 card = IM_COL32(28, 32, 42, 255);
    ImU32 card_hover = IM_COL32(36, 41, 54, 255);
    ImU32 line = IM_COL32(48, 54, 70, 255);
    ImU32 text = IM_COL32(236, 239, 245, 255);
    ImU32 dim = IM_COL32(160, 168, 186, 255);
    ImU32 faint = IM_COL32(110, 118, 136, 255);
    ImU32 accent = IM_COL32(88, 166, 255, 255);
    ImU32 ok = IM_COL32(74, 201, 130, 255);
    ImU32 warn = IM_COL32(240, 182, 72, 255);
    ImU32 bad = IM_COL32(242, 99, 99, 255);
} P;

ImVec4 v4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

// Escala de pantalla de Windows (125 %, 150 %...). Toda medida fija pasa por px().
float g_scale = 1.0f;
float px(float v) { return v * g_scale; }

ImFont* g_regular = nullptr;
ImFont* g_semibold = nullptr;

// Color de cada ficha: generado desde game_id, sin arte del juego (no se usan assets propietarios).
ImU32 tile_color(const std::string& id) {
    static const ImU32 colors[] = {IM_COL32(230, 80, 90, 255), IM_COL32(70, 150, 240, 255), IM_COL32(60, 190, 120, 255),
                                   IM_COL32(240, 160, 50, 255), IM_COL32(150, 110, 230, 255)};
    unsigned h = 2166136261u;
    for (char c : id) h = (h ^ (unsigned char)c) * 16777619u;
    return colors[h % (sizeof colors / sizeof colors[0])];
}

std::string initials(const std::string& name) {
    std::string out;
    bool start = true;
    for (char c : name) {
        if (c == ' ' || c == '.') { start = true; continue; }
        if (start && out.size() < 2 && ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) out += c;
        start = false;
    }
    return out.empty() ? "?" : out;
}

void text(ImFont* font, float size, ImU32 color, const char* s, float wrap = 0.0f) {
    ImGui::PushFont(font, size);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(color));
    if (wrap > 0) ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
    ImGui::TextUnformatted(s);
    if (wrap > 0) ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

enum class Btn { Primary, Secondary, Ghost };

bool button(const char* label, Btn kind, bool enabled, float width = 0.0f, const char* why_disabled = nullptr) {
    ImU32 base = kind == Btn::Primary ? P.accent : kind == Btn::Secondary ? P.card_hover : IM_COL32(0, 0, 0, 0);
    ImGui::PushStyleColor(ImGuiCol_Button, v4(base));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, v4(kind == Btn::Primary ? IM_COL32(118, 184, 255, 255) : P.line));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, v4(kind == Btn::Primary ? IM_COL32(64, 140, 230, 255) : P.card));
    ImGui::PushStyleColor(ImGuiCol_Text, v4(kind == Btn::Primary ? IM_COL32(10, 16, 28, 255) : P.text));
    if (kind == Btn::Ghost) {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Border, v4(P.line));
    }
    ImGui::PushFont(g_semibold, 15.0f);
    ImGui::BeginDisabled(!enabled);
    bool pressed = ImGui::Button(label, ImVec2(width, px(40.0f)));
    ImGui::EndDisabled();
    ImGui::PopFont();
    if (kind == Btn::Ghost) {
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }
    ImGui::PopStyleColor(4);
    if (!enabled && why_disabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", why_disabled);
    return pressed && enabled;
}

void chip(const char* label, ImU32 color) {
    ImGui::PushFont(g_semibold, 12.5f);
    ImVec2 size = ImGui::CalcTextSize(label);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 box(size.x + px(20), size.y + px(8));
    ImDrawList* d = ImGui::GetWindowDrawList();
    ImU32 bgc = (color & 0x00FFFFFF) | (40u << 24);
    d->AddRectFilled(pos, ImVec2(pos.x + box.x, pos.y + box.y), bgc, box.y * 0.5f);
    d->AddText(ImVec2(pos.x + px(10), pos.y + px(4)), color, label);
    ImGui::Dummy(box);
    ImGui::PopFont();
}

void apply_style(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowPadding = ImVec2(px(0), px(0));
    s.FramePadding = ImVec2(px(14), px(10));
    s.ItemSpacing = ImVec2(px(10), px(10));
    s.FrameRounding = 8;
    s.ChildRounding = 12;
    s.PopupRounding = 8;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 0;
    s.FrameBorderSize = 0;
    s.ScrollbarSize = 10;
    s.ScrollbarRounding = 8;
    s.Colors[ImGuiCol_WindowBg] = v4(P.bg);
    s.Colors[ImGuiCol_ChildBg] = v4(P.card);
    s.Colors[ImGuiCol_PopupBg] = v4(P.card);
    s.Colors[ImGuiCol_Text] = v4(P.text);
    s.Colors[ImGuiCol_TextDisabled] = v4(P.faint);
    s.Colors[ImGuiCol_FrameBg] = v4(P.card_hover);
    s.Colors[ImGuiCol_FrameBgHovered] = v4(IM_COL32(20, 23, 31, 255));
    s.Colors[ImGuiCol_FrameBgActive] = v4(IM_COL32(20, 23, 31, 255));
    s.Colors[ImGuiCol_Border] = v4(P.line);
    s.Colors[ImGuiCol_NavCursor] = v4(P.accent);
    s.Colors[ImGuiCol_Header] = v4(P.card_hover);
    s.Colors[ImGuiCol_HeaderHovered] = v4(P.card_hover);
    s.Colors[ImGuiCol_HeaderActive] = v4(P.line);
    s.Colors[ImGuiCol_ScrollbarBg] = v4(IM_COL32(0, 0, 0, 0));
    s.Colors[ImGuiCol_ScrollbarGrab] = v4(P.line);
    s.ScaleAllSizes(scale);
    s.FontScaleDpi = scale;
}

// ---------------------------------------------------------------- estado

struct Game {
    Manifest manifest;
    std::unique_ptr<GameAdapter> adapter;
    fs::path root;
    // Resultado de probe(), escrito por un hilo de fondo.
    std::mutex mutex;
    bool probing = false;
    bool probed = false;
    InstallStatus status;
    double probe_ms = 0;
    // Juego en marcha.
    std::atomic<long long> running_pid{0};
    std::string message;
    bool message_bad = false;
    // Online con servidor en la LAN (lan_wfc): IP del anfitrión que se escribe en la configuración del juego.
    char lan_ip[64] = {};
    bool lan_loaded = false;
};

enum class Page { Game, Room };

struct App {
    std::vector<std::unique_ptr<Game>> games;
    std::vector<std::string> catalog_errors;
    int selected = 0;
    Page page = Page::Game;
    Prefs prefs;
    std::unique_ptr<RoomSession> room;
    int room_game = -1;
    char name[32] = {};
    char address[64] = {};
    int port = 0;
    int start_option = 1;
    std::string room_error;
    std::vector<std::string> addresses;
    std::atomic<int> pending{0};  // trabajos de fondo que piden redibujar
};

App* g_app = nullptr;
Uint32 g_wake_event = 0;

void wake() {
    SDL_Event e{};
    e.type = g_wake_event;
    SDL_PushEvent(&e);
}

void probe(Game& g) {
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        if (g.probing || !g.adapter) return;
        g.probing = true;
    }
    std::thread([&g] {
        auto t0 = Clock::now();
        InstallStatus s = g.adapter->probe(g.root);
        double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        {
            std::lock_guard<std::mutex> lock(g.mutex);
            g.status = s;
            g.probe_ms = ms;
            g.probed = true;
            g.probing = false;
        }
        wake();
    }).detach();
}

void launch(Game& g, const LaunchSpec& spec) {
    if (g.running_pid.load() != 0) {
        g.message = "El juego ya está abierto.";
        g.message_bad = false;
        return;
    }
    std::error_code e;
    fs::create_directories(g.root / "logs", e);
    Spawned p = spawn(spec);
    if (!p.ok) {
        g.message = p.error;
        g.message_bad = true;
        return;
    }
    g.message = "Juego iniciado.";
    g.message_bad = false;
    g.running_pid = p.pid;
    // Un hilo espera a que el juego termine; la UI no se bloquea.
    std::thread([&g, pid = p.pid] {
        int code = wait_exit(pid, -1);
        {
            std::lock_guard<std::mutex> lock(g.mutex);
            g.running_pid = 0;
            if (code == 0) {
                g.message = "El juego se ha cerrado.";
                g.message_bad = false;
            } else {
                g.message = "El juego se cerró con el código " + std::to_string(code) + ". Mira logs\\game.log en su carpeta.";
                g.message_bad = true;
            }
        }
        wake();
    }).detach();
}

const char* capability_text(const Manifest& m) {
    if (m.status == GameStatus::Planned) return "Previsto";
    if (m.has("lan_host") && m.has("lan_join")) return "Offline · Online por LAN o VPN";
    if (m.has("lan_wfc")) return "Offline · Online por LAN (beta)";
    return "Offline";
}

// ---------------------------------------------------------------- vistas

void sidebar(App& app, float height) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(P.sidebar));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::BeginChild("##sidebar", ImVec2(px(280), height), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    ImGui::SetCursorPos(ImVec2(px(24), px(26)));
    text(g_semibold, 20, P.text, "Wii Ports");
    ImGui::SetCursorPosX(px(24));
    text(g_regular, 13, P.faint, "Biblioteca");
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(14));
    for (size_t i = 0; i < app.games.size(); ++i) {
        Game& g = *app.games[i];
        ImGui::PushID((int)i);
        ImGui::SetCursorPosX(px(12));
        ImVec2 pos = ImGui::GetCursorScreenPos();
        bool selected = (int)i == app.selected;
        ImGui::PushStyleColor(ImGuiCol_Header, v4(P.card));
        if (ImGui::Selectable("##game", selected, ImGuiSelectableFlags_None, ImVec2(px(256), px(60)))) {
            if (app.selected != (int)i) app.page = Page::Game;
            app.selected = (int)i;
            app.prefs.set("selected", g.manifest.game_id);
        }
        ImGui::PopStyleColor();
        ImDrawList* d = ImGui::GetWindowDrawList();
        ImU32 tile = tile_color(g.manifest.game_id);
        bool planned = g.manifest.status == GameStatus::Planned;
        if (planned) tile = (tile & 0x00FFFFFF) | (90u << 24);
        d->AddRectFilled(ImVec2(pos.x + px(10), pos.y + px(10)), ImVec2(pos.x + px(50), pos.y + px(50)), tile, 10);
        ImGui::PushFont(g_semibold, 15);
        std::string ini = initials(g.manifest.display_name);
        ImVec2 ts = ImGui::CalcTextSize(ini.c_str());
        d->AddText(ImVec2(pos.x + px(30) - ts.x / 2, pos.y + px(30) - ts.y / 2), IM_COL32(255, 255, 255, planned ? 160 : 255), ini.c_str());
        d->AddText(ImVec2(pos.x + px(62), pos.y + px(11)), planned ? P.dim : P.text, g.manifest.display_name.c_str());
        ImGui::PopFont();
        std::string sub;
        ImU32 dot = P.faint;
        {
            std::lock_guard<std::mutex> lock(g.mutex);
            if (planned) sub = "Previsto";
            else if (g.running_pid.load()) { sub = "En ejecución"; dot = P.accent; }
            else if (!g.probed) sub = "Comprobando…";
            else if (g.status.ready()) { sub = "Listo para jugar"; dot = P.ok; }
            else { sub = g.status.folder ? "Necesita atención" : "No instalado"; dot = P.warn; }
        }
        ImGui::PushFont(g_regular, 12.5f);
        d->AddCircleFilled(ImVec2(pos.x + px(66), pos.y + px(40)), px(3.5f), dot);
        d->AddText(ImVec2(pos.x + px(76), pos.y + px(32)), P.dim, sub.c_str());
        ImGui::PopFont();
        ImGui::PopID();
    }
    ImGui::SetCursorPos(ImVec2(px(24), height - px(58)));
    text(g_regular, 11.5f, P.faint, "Sin servidores: el online es directo,\npor LAN o por una VPN como Hamachi.");
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void info_row(const char* label, const std::string& value, ImU32 color) {
    ImGui::SetCursorPosX(px(22));
    text(g_regular, 13, P.faint, label);
    ImGui::SameLine(px(190));
    text(g_regular, 13, color, value.c_str());
}

void open_room(App& app, bool host);

// "Instalar o actualizar" abre el launcher original del juego, que es quien compila desde el disco.
// La ruta la elige el usuario una vez y se guarda en las preferencias del launcher unificado.
struct PickRequest {
    App* app;
    std::string game_id;
};

void SDLCALL picked_launcher(void* data, const char* const* files, int) {
    std::unique_ptr<PickRequest> req(static_cast<PickRequest*>(data));
    if (!files || !files[0]) return;
    std::string path = files[0];
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (lower.size() < 4 || lower.substr(lower.size() - 4) != ".exe") return;
    req->app->prefs.set("legacy_launcher." + req->game_id, path);
    wake();
}

fs::path g_base;  // carpeta del exe del launcher unificado

// Instalador incluido en el paquete (manifest `installer`), si está junto al launcher.
std::optional<fs::path> bundled_installer(const Manifest& m) {
    if (m.installer.empty() || g_base.empty()) return std::nullopt;
    auto p = resolve_inside(g_base, m.installer);
    std::error_code e;
    if (!p || !fs::is_regular_file(*p, e)) return std::nullopt;
    return p;
}

// Instalador .ps1 (Mario Kart Wii): recibe el disco con -Disco y se ve en su propia consola.
struct DiscRequest {
    Game* game;
    fs::path installer;
};

void SDLCALL picked_disc(void* data, const char* const* files, int) {
    std::unique_ptr<DiscRequest> req(static_cast<DiscRequest*>(data));
    if (!files || !files[0]) return;
    LaunchSpec spec;
    spec.executable = "C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
    spec.args = {"-NoProfile", "-ExecutionPolicy", "Bypass", "-NoExit", "-File", req->installer.u8string(), "-Disco", files[0]};
    spec.working_dir = req->installer.parent_path();
    Spawned p = spawn(spec);
    Game& g = *req->game;
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        g.message = p.ok ? "Instalando en una ventana aparte (unos 25 minutos la primera vez). Al terminar, pulsa Volver a comprobar." : p.error;
        g.message_bad = !p.ok;
    }
    wake();
}

void open_legacy_launcher(App& app, Game& g, SDL_Window* window) {
    if (auto inst = bundled_installer(g.manifest)) {
        std::string ext = inst->extension().u8string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (ext == ".ps1") {
            static const SDL_DialogFileFilter filter[] = {{"Disco de Wii (iso, wbfs, rvz, wia, gcz, ciso)", "iso;wbfs;rvz;wia;gcz;ciso"}};
            SDL_ShowOpenFileDialog(picked_disc, new DiscRequest{&g, *inst}, window, filter, 1, nullptr, false);
            g.message = "Elige tu copia de " + g.manifest.display_name + " (ISO o WBFS).";
            g.message_bad = false;
            return;
        }
        LaunchSpec spec;
        spec.executable = *inst;
        spec.working_dir = inst->parent_path();
        Spawned p = spawn(spec);
        g.message = p.ok ? "Abierto el instalador de " + g.manifest.display_name + ". Necesita tu disco o tu WBFS. Al terminar, pulsa Volver a comprobar." : p.error;
        g.message_bad = !p.ok;
        return;
    }
    std::string path = app.prefs.get("legacy_launcher." + g.manifest.game_id);
    std::error_code e;
    if (path.empty() || !fs::is_regular_file(fs::u8path(path), e)) {
        static const SDL_DialogFileFilter filter[] = {{"Launcher (.exe)", "exe"}};
        SDL_ShowOpenFileDialog(picked_launcher, new PickRequest{&app, g.manifest.game_id}, window, filter, 1, nullptr, false);
        g.message = "Elige el launcher actual de " + g.manifest.display_name + " (por ejemplo mp9online-launcher.exe). Se recordará.";
        g.message_bad = false;
        return;
    }
    LaunchSpec spec;
    spec.executable = fs::u8path(path);
    spec.working_dir = spec.executable.parent_path();
    Spawned p = spawn(spec);
    g.message = p.ok ? "Abierto el launcher de " + g.manifest.display_name + ". Al terminar, pulsa Volver a comprobar." : p.error;
    g.message_bad = !p.ok;
}

SDL_Window* g_window = nullptr;

// Mario Kart Wii (beta): el online es dentro del juego contra un servidor WFC que arranca el anfitrión.
void lan_wfc_card(App& app, Game& g, const InstallStatus& s, float width) {
    const Manifest& m = g.manifest;
    if (!g.lan_loaded && s.folder) {
        g.lan_loaded = true;
        std::snprintf(g.lan_ip, sizeof(g.lan_ip), "%s", g.adapter->lan_server(g.root).c_str());
        if (app.addresses.empty()) app.addresses = local_addresses();
    }
    ImGui::Dummy(ImVec2(px(0), px(14)));
    ImGui::BeginChild("##wfc", ImVec2(width, 0), ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar);
    float x = px(22);
    float inner = width - px(44);
    ImGui::SetCursorPos(ImVec2(x, px(18)));
    text(g_semibold, 15, P.text, "Online por LAN (beta)");
    ImGui::SetCursorPosX(x);
    text(g_regular, 13, P.dim, "Uno de vosotros es el anfitrión y arranca el servidor. Los demás escriben su IP aquí y, dentro del juego, entran en Nintendo WFC.", inner);
    ImGui::Dummy(ImVec2(0, px(6)));
    std::string mine;
    for (size_t i = 0; i < app.addresses.size(); ++i) mine += (i ? ", " : "") + app.addresses[i];
    ImGui::SetCursorPosX(x);
    text(g_regular, 13, P.faint, ("Tus direcciones: " + (mine.empty() ? std::string("—") : mine)).c_str(), inner);
    ImGui::Dummy(ImVec2(0, px(4)));

    // Anfitrión
    auto script = m.lan_server_script.empty() || g_base.empty() ? std::nullopt : resolve_inside(g_base, m.lan_server_script);
    std::error_code e;
    bool has_server = script && fs::is_regular_file(*script, e);
    ImGui::SetCursorPosX(x);
    if (button("Soy el anfitrión: arrancar servidor", Btn::Secondary, has_server && s.folder, px(330),
               has_server ? "Primero instala el juego." : "El servidor no está en esta copia del launcher (falta la carpeta servidor-mkw).")) {
        LaunchSpec spec;
        spec.executable = "C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
        spec.args = {"-NoProfile", "-ExecutionPolicy", "Bypass", "-NoExit", "-File", script->u8string()};
        spec.working_dir = script->parent_path();
        Spawned p = spawn(spec);
        std::string err;
        if (p.ok && !app.addresses.empty() && g.adapter->set_lan_server(g.root, app.addresses[0], err))
            std::snprintf(g.lan_ip, sizeof(g.lan_ip), "%s", app.addresses[0].c_str());
        g.message = p.ok ? "Servidor arrancando en una ventana aparte (no la cierres). Tus amigos deben escribir " +
                               (app.addresses.empty() ? std::string("tu IP") : app.addresses[0]) + "."
                         : p.error;
        g.message_bad = !p.ok;
    }
    ImGui::Dummy(ImVec2(0, px(6)));

    // IP del anfitrión
    ImGui::SetCursorPosX(x);
    ImGui::AlignTextToFramePadding();
    text(g_regular, 13, P.dim, "IP del anfitrión");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(px(200));
    ImGui::InputTextWithHint("##lanip", "192.168.1.20", g.lan_ip, sizeof(g.lan_ip), ImGuiInputTextFlags_CharsNoBlank);
    ImGui::SameLine();
    if (button("Guardar", Btn::Primary, s.folder && g.lan_ip[0] != 0, px(120), "Escribe la IP que te diga el anfitrión.")) {
        std::string err;
        bool ok = g.adapter->set_lan_server(g.root, g.lan_ip, err);
        g.message = ok ? std::string("Guardado. Abre el juego y entra en Nintendo WFC.") : err;
        g.message_bad = !ok;
    }
    ImGui::SameLine();
    if (button("Jugar sin online", Btn::Ghost, s.folder, px(170))) {
        std::string err;
        bool ok = g.adapter->set_lan_server(g.root, "", err);
        if (ok) g.lan_ip[0] = 0;
        g.message = ok ? std::string("Quitado el servidor: el juego vuelve a funcionar como el original.") : err;
        g.message_bad = !ok;
    }
    ImGui::Dummy(ImVec2(0, px(6)));
    ImGui::SetCursorPosX(x);
    text(g_regular, 12, P.faint, "En el juego: F10 > Controller settings > Player 1 para elegir teclado o mando. Luego Nintendo WFC > 1 jugador > Permit.", inner);
    ImGui::Dummy(ImVec2(0, px(10)));
    ImGui::EndChild();
}

void game_page(App& app, Game& g, float width) {
    const Manifest& m = g.manifest;
    bool planned = m.status == GameStatus::Planned;
    InstallStatus s;
    bool probed, probing;
    std::string message;
    bool message_bad;
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        s = g.status;
        probed = g.probed;
        probing = g.probing;
        message = g.message;
        message_bad = g.message_bad;
    }
    bool running = g.running_pid.load() != 0;
    bool ready = probed && s.ready();

    // Cabecera
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX(), px(34)));
    ImVec2 head = ImGui::GetCursorScreenPos();
    ImU32 tile = tile_color(m.game_id);
    ImGui::GetWindowDrawList()->AddRectFilled(head, ImVec2(head.x + px(84), head.y + px(84)), planned ? ((tile & 0x00FFFFFF) | (90u << 24)) : tile, 18);
    ImGui::PushFont(g_semibold, 30);
    std::string ini = initials(m.display_name);
    ImVec2 ts = ImGui::CalcTextSize(ini.c_str());
    ImGui::GetWindowDrawList()->AddText(ImVec2(head.x + px(42) - ts.x / 2, head.y + px(42) - ts.y / 2), IM_COL32(255, 255, 255, 255), ini.c_str());
    ImGui::PopFont();
    float x0 = ImGui::GetCursorPosX();
    ImGui::SetCursorPos(ImVec2(x0 + px(104), px(36)));
    text(g_semibold, 32, P.text, m.display_name.c_str());
    ImGui::SetCursorPosX(x0 + px(104));
    std::string st = status_name(m.status);
    st[0] = (char)std::toupper((unsigned char)st[0]);
    chip(st.c_str(), m.status == GameStatus::Playable ? P.ok : (m.status == GameStatus::InDevelopment || m.status == GameStatus::Beta) ? P.warn : P.faint);
    if (!planned) {
        ImGui::SameLine();
        chip(capability_text(m), P.accent);
    }
    if (running) {
        ImGui::SameLine();
        chip("En ejecución", P.accent);
    }
    ImGui::SetCursorPos(ImVec2(x0, px(146)));

    if (planned) {
        text(g_regular, 15, P.dim, "Este port todavía no existe. La ficha está preparada para cuando llegue:", width);
        text(g_regular, 15, P.dim, "bastará con su manifest y su adaptador.", width);
        if (!m.notes.empty()) {
            ImGui::Dummy(ImVec2(px(0), px(6)));
            text(g_regular, 13, P.faint, m.notes.c_str(), width);
        }
        return;
    }

    // Acciones: Jugar, Crear sala, Unirse, Ajustes.
    bool online = m.has("lan_host") && g.adapter && g.adapter->make_room() != nullptr;
    const char* why = !probed ? "Comprobando la instalación…" : !s.ready() ? s.problem.c_str() : running ? "El juego ya está abierto." : nullptr;
    bool not_installed = probed && !s.folder;
    if (not_installed) {
        // Sin instalar: la acción principal es instalar (con el instalador incluido en el paquete).
        if (button("Instalar", Btn::Primary, true, px(150))) open_legacy_launcher(app, g, g_window);
    } else if (button("  Jugar  ", Btn::Primary, ready && !running, px(150), why)) {
        launch(g, g.adapter->offline(g.root));
    }
    bool wfc = m.has("lan_wfc");
    if (!wfc) {
        ImGui::SameLine();
        const char* why_online = !online ? "El online de este juego aún no está en este launcher. Usa su launcher actual." : why;
        if (button("Crear sala", Btn::Secondary, online && ready && !running, px(140), why_online)) open_room(app, true);
        ImGui::SameLine();
        if (button("Unirse", Btn::Secondary, online && ready && !running, px(120), why_online)) open_room(app, false);
    }
    ImGui::SameLine();
    if (button("Ajustes", Btn::Ghost, probed && s.folder, px(110), "No está instalado.")) {
        fs::path ini_path = wfc ? resolve_inside(g.root, m.working_dir).value_or(g.root) / "UserData" / "Config.toml" : g.root / "games" / m.game_id / "settings.ini";
        std::string url = "file:///" + ini_path.generic_u8string();
        SDL_OpenURL(url.c_str());
    }
    if (!message.empty()) {
        ImGui::Dummy(ImVec2(px(0), px(2)));
        text(g_regular, 13.5f, message_bad ? P.bad : P.dim, message.c_str(), width);
    }
    if (probed && !s.ready()) {
        ImGui::Dummy(ImVec2(px(0), px(2)));
        text(g_regular, 13.5f, P.warn, s.problem.c_str(), width);
    }

    if (wfc && s.folder) lan_wfc_card(app, g, s, width);

    // Tarjeta: instalación y compatibilidad.
    ImGui::Dummy(ImVec2(px(0), px(14)));
    ImGui::BeginChild("##info", ImVec2(width, 0), ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar);
    ImGui::SetCursorPos(ImVec2(px(22), px(18)));
    text(g_semibold, 15, P.text, "Instalación y versión");
    ImGui::Dummy(ImVec2(px(0), px(2)));
    info_row("Estado", !probed ? "Comprobando…" : s.ready() ? "Listo" : (s.folder ? "Incompleto" : "No instalado"),
             !probed ? P.dim : s.ready() ? P.ok : P.warn);
    info_row("Carpeta", g.root.u8string(), P.dim);
    info_row("Build instalada", s.revision.empty() ? "—" : s.revision, P.text);
    info_row("Disco", s.disc_id.empty() ? "—" : s.disc_id + (s.disc_ok ? "  (admitido)" : "  (no admitido)"), s.disc_ok ? P.text : P.warn);
    if (!m.has("lan_wfc")) info_row("Versión de sala", probed && g.adapter ? g.adapter->compat_token(s) : "—", P.text);
    info_row("Jugadores", "Hasta " + std::to_string(m.max_players), P.text);
    ImGui::Dummy(ImVec2(0, px(6)));
    ImGui::SetCursorPosX(px(22));
    text(g_regular, 12, P.faint, ("Comprobado en " + std::to_string((int)(g.probe_ms + 0.5)) + " ms · " + m.notes).c_str());
    ImGui::Dummy(ImVec2(0, px(10)));
    ImGui::EndChild();

    ImGui::Dummy(ImVec2(px(0), px(4)));
    if (button("Volver a comprobar", Btn::Ghost, !probing, px(190))) probe(g);
    ImGui::SameLine();
    if (button("Abrir carpeta", Btn::Ghost, probed && s.folder, px(150))) SDL_OpenURL(("file:///" + g.root.generic_u8string()).c_str());
    ImGui::SameLine();
    if (button("Instalar o actualizar", Btn::Ghost, true, px(210))) open_legacy_launcher(app, g, g_window);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Abre el launcher actual del juego, que compila el juego desde tu disco.");
}

// ---------------------------------------------------------------- sala

std::string seat_name(int seat) { return seat < 0 ? "Sin jugador" : "Jugador " + std::to_string(seat + 1); }

void open_room(App& app, bool host) {
    Game& g = *app.games[app.selected];
    app.room_error.clear();
    app.room = g.adapter->make_room();
    app.room_game = app.selected;
    app.page = Page::Room;
    if (!app.port) app.port = app.room->default_port();
    if (host) {
        InstallStatus s;
        {
            std::lock_guard<std::mutex> lock(g.mutex);
            s = g.status;
        }
        if (!app.room->host((uint16_t)app.port, app.name, g.adapter->compat_token(s), app.room_error)) app.room.reset();
        app.addresses = local_addresses();
    }
}

void room_page(App& app, float width) {
    Game& g = *app.games[app.room_game];
    InstallStatus s;
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        s = g.status;
    }
    float x0 = ImGui::GetCursorPosX();
    ImGui::SetCursorPos(ImVec2(x0, px(34)));
    if (button("‹ Volver", Btn::Ghost, true, px(110))) {
        if (app.room) app.room->leave();
        app.room.reset();
        app.page = Page::Game;
        return;
    }
    ImGui::SetCursorPosX(x0);
    text(g_semibold, 28, P.text, ("Sala · " + g.manifest.display_name).c_str());
    ImGui::SetCursorPosX(x0);
    text(g_regular, 13.5f, P.dim,
         "Un jugador crea la sala y los demás se unen con su dirección (LAN o Hamachi). Todos necesitáis la misma build del juego.", width);
    ImGui::Dummy(ImVec2(px(0), px(8)));

    RoomMode mode = app.room ? app.room->mode() : RoomMode::Idle;
    if (mode == RoomMode::Idle) {
        ImGui::SetCursorPosX(x0);
        ImGui::BeginChild("##join", ImVec2(width, 0), ImGuiChildFlags_AutoResizeY);
        ImGui::SetCursorPos(ImVec2(px(22), px(18)));
        text(g_regular, 13, P.faint, "Tu nombre");
        ImGui::SetCursorPosX(px(22));
        ImGui::SetNextItemWidth(px(260));
        if (ImGui::InputText("##name", app.name, sizeof app.name)) app.prefs.set("online_name", app.name);
        ImGui::SetCursorPosX(px(22));
        text(g_regular, 13, P.faint, "Dirección del anfitrión y puerto");
        ImGui::SetCursorPosX(px(22));
        ImGui::SetNextItemWidth(px(260));
        if (ImGui::InputTextWithHint("##addr", "p. ej. 25.12.34.56", app.address, sizeof app.address)) app.prefs.set("online_address", app.address);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(px(110));
        if (ImGui::InputInt("##port", &app.port, 0)) {
            app.port = std::clamp(app.port, 1024, 65534);
            app.prefs.set("online_port", std::to_string(app.port));
        }
        ImGui::SetCursorPosX(px(22));
        ImGui::Dummy(ImVec2(px(0), px(4)));
        ImGui::SetCursorPosX(px(22));
        if (button("Unirse", Btn::Primary, app.address[0] != 0, px(140), "Escribe la dirección del anfitrión.")) {
            app.room = g.adapter->make_room();
            app.room_error.clear();
            if (!app.room->join(app.address, (uint16_t)app.port, app.name, g.adapter->compat_token(s), app.room_error)) app.room.reset();
        }
        ImGui::SameLine();
        if (button("Crear sala", Btn::Secondary, true, px(140))) open_room(app, true);
        ImGui::Dummy(ImVec2(0, px(12)));
        ImGui::EndChild();
        std::string msg = !app.room_error.empty() ? app.room_error : app.room ? app.room->status() : "";
        if (!msg.empty()) {
            ImGui::SetCursorPosX(x0);
            text(g_regular, 14, P.bad, msg.c_str(), width);
        }
        return;
    }
    if (mode == RoomMode::Joining) {
        ImGui::SetCursorPosX(x0);
        text(g_regular, 15, P.dim, "Conectando con el anfitrión…");
        ImGui::SetCursorPosX(x0);
        if (button("Cancelar", Btn::Secondary, true, px(140))) {
            app.room->leave();
            app.room.reset();
        }
        return;
    }

    RoomSession& room = *app.room;
    bool hosting = mode == RoomMode::Hosting;
    ImGui::SetCursorPosX(x0);
    text(g_regular, 14, P.dim, room.status().c_str(), width);
    if (hosting) {
        std::string line = "Tus direcciones: ";
        for (size_t i = 0; i < app.addresses.size(); ++i) line += (i ? ", " : "") + app.addresses[i] + (app.addresses[i].rfind("25.", 0) == 0 ? " (Hamachi)" : "");
        line += "  ·  puerto " + std::to_string(app.port);
        if (g.manifest.game_id == "mp9") line += " (y el siguiente para el juego)";  // MP9 juega en puerto + 1
        ImGui::SetCursorPosX(x0);
        text(g_regular, 13, P.faint, line.c_str(), width);
    }
    ImGui::Dummy(ImVec2(px(0), px(6)));
    std::vector<RoomMember> members = room.members();
    const RoomMember* me = nullptr;
    ImGui::SetCursorPosX(x0);
    ImGui::BeginChild("##members", ImVec2(width, 0), ImGuiChildFlags_AutoResizeY);
    ImGui::SetCursorPos(ImVec2(px(22), px(16)));
    for (const RoomMember& mm : members) {
        if (mm.id == room.self()) me = &mm;
        ImGui::SetCursorPosX(px(22));
        std::string who = mm.name + (mm.id == 0 ? "  (anfitrión)" : "") + (mm.id == room.self() ? "  · tú" : "");
        text(g_semibold, 14, P.text, who.c_str());
        ImGui::SameLine(px(320));
        text(g_regular, 14, P.dim, seat_name(mm.seat).c_str());
        ImGui::SameLine(px(460));
        bool ok = mm.id == 0 || mm.ready;
        text(g_regular, 14, ok ? P.ok : P.faint, ok ? "Listo" : "No listo");
    }
    if (members.empty()) {
        ImGui::SetCursorPosX(px(22));
        text(g_regular, 14, P.dim, "Esperando a la sala…");
    }
    ImGui::Dummy(ImVec2(0, px(12)));
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(px(0), px(4)));
    if (me) {
        ImGui::SetCursorPosX(x0);
        for (int seat = 0; seat < room.seats(); ++seat) {
            bool taken = std::any_of(members.begin(), members.end(), [&](const RoomMember& o) { return o.id != me->id && o.seat == seat; });
            if (seat) ImGui::SameLine();
            ImGui::PushID(seat);
            if (button(seat_name(seat).c_str(), me->seat == seat ? Btn::Primary : Btn::Secondary, !taken, px(120), "Ese jugador ya está elegido.")) room.choose_seat(seat);
            ImGui::PopID();
        }
        ImGui::SetCursorPosX(x0);
        if (hosting) {
            auto options = room.start_options();
            if (!options.empty()) {
                ImGui::AlignTextToFramePadding();
                text(g_regular, 13, P.faint, "Red");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(px(200));
                if (ImGui::BeginCombo("##red", options[std::clamp(app.start_option, 0, (int)options.size() - 1)].c_str())) {
                    for (int i = 0; i < (int)options.size(); ++i)
                        if (ImGui::Selectable(options[i].c_str(), i == app.start_option)) {
                            app.start_option = i;
                            app.prefs.set("online_stability", std::to_string(i));
                        }
                    ImGui::EndCombo();
                }
                ImGui::SetCursorPosX(x0);
            }
            if (button("Empezar", Btn::Primary, room.can_start(), px(150), "Cada jugador necesita su propio puesto y estar listo.")) room.start(app.start_option);
        } else if (button(me->ready ? "No estoy listo" : "Listo", me->ready ? Btn::Secondary : Btn::Primary, me->seat >= 0, px(160), "Elige antes un jugador.")) {
            room.set_ready(!me->ready);
        }
        ImGui::SameLine();
    } else {
        ImGui::SetCursorPosX(x0);
    }
    if (button("Salir de la sala", Btn::Ghost, true, px(170))) {
        room.leave();
        app.room.reset();
    }
}

}  // namespace

// ---------------------------------------------------------------- arranque

int main(int argc, char** argv) {
    auto t_start = Clock::now();
    std::string manifests = WL_MANIFEST_DIR;
    std::string screenshot;
    int select = -1;
    bool host_now = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--manifests") && i + 1 < argc) manifests = argv[++i];
        else if (!std::strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshot = argv[++i];
        else if (!std::strcmp(argv[i], "--select") && i + 1 < argc) select = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--host")) host_now = true;  // abre la sala del juego elegido (pruebas)
    }
    // Junto al exe hay una carpeta manifests/ en la versión instalable; en desarrollo se usa la del repo.
    if (const char* base = SDL_GetBasePath()) {
        g_base = fs::u8path(base);
        fs::path local = fs::u8path(base) / "manifests";
        std::error_code e;
        if (fs::is_directory(local, e)) manifests = local.u8string();
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (scale <= 0) scale = 1;
    if (const char* forced = SDL_getenv("WL_SCALE")) scale = (float)std::atof(forced);  // pruebas de escalado
    SDL_Window* window = SDL_CreateWindow("Wii Ports", (int)(1120 * scale), (int)(700 * scale), SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!window || !renderer) {
        std::fprintf(stderr, "ventana: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);
    g_window = window;
    SDL_SetWindowMinimumSize(window, (int)(900 * scale), (int)(580 * scale));
    g_wake_event = SDL_RegisterEvents(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    {
        fs::path fonts = fs::u8path(SDL_GetBasePath() ? SDL_GetBasePath() : "") / "fonts";
        std::error_code e;
        if (fs::exists(fonts / "Inter-Regular.ttf", e)) {
            g_regular = io.Fonts->AddFontFromFileTTF((fonts / "Inter-Regular.ttf").u8string().c_str(), 15.0f);
            g_semibold = io.Fonts->AddFontFromFileTTF((fonts / "Inter-SemiBold.ttf").u8string().c_str(), 15.0f);
        }
        if (!g_regular) g_regular = io.Fonts->AddFontDefault();
        if (!g_semibold) g_semibold = g_regular;
    }
    g_scale = scale;
    apply_style(scale);
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    App app;
    g_app = &app;
    app.prefs.load();
    Catalog catalog = load_catalog(manifests);
    app.catalog_errors = catalog.errors;
    for (auto& e : catalog.games) {
        auto g = std::make_unique<Game>();
        g->manifest = e.manifest;
        g->adapter = make_adapter(e.manifest);
        g->root = install_root(e.manifest);
        app.games.push_back(std::move(g));
    }
    // Orden: jugables, en desarrollo, previstos.
    std::stable_sort(app.games.begin(), app.games.end(), [](const auto& a, const auto& b) { return (int)a->manifest.status < (int)b->manifest.status; });
    std::string last = app.prefs.get("selected");
    for (size_t i = 0; i < app.games.size(); ++i)
        if (app.games[i]->manifest.game_id == last) app.selected = (int)i;
    if (select >= 0 && select < (int)app.games.size()) app.selected = select;
    std::snprintf(app.name, sizeof app.name, "%s", app.prefs.get("online_name", SDL_getenv("USERNAME") ? SDL_getenv("USERNAME") : "Jugador").c_str());
    std::snprintf(app.address, sizeof app.address, "%s", app.prefs.get("online_address").c_str());
    app.port = std::atoi(app.prefs.get("online_port", "47630").c_str());
    app.start_option = std::atoi(app.prefs.get("online_stability", "1").c_str());
    for (auto& g : app.games) probe(*g);  // en segundo plano: la ventana aparece sin esperar al disco

    if (host_now && app.games[app.selected]->adapter) {
        for (int i = 0; i < 200 && !app.games[app.selected]->probed; ++i) SDL_Delay(5);
        if (app.games[app.selected]->adapter->make_room()) open_room(app, true);
    }
    SDL_ShowWindow(window);
    bool first_frame = true;
    int frames = 0;
    bool quit = false;
    int redraw = 2;
    while (!quit) {
        // Reposo: espera eventos. Con sala abierta o un juego en marcha, despierta a ~30 Hz para la red.
        bool busy = app.room && app.room->mode() != RoomMode::Idle;
        SDL_Event ev;
        bool got = redraw > 0 || !screenshot.empty() ? SDL_PollEvent(&ev) : SDL_WaitEventTimeout(&ev, busy ? 33 : 1000);
        while (got) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            if (ev.type == SDL_EVENT_QUIT || (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && ev.window.windowID == SDL_GetWindowID(window))) quit = true;
            redraw = 2;
            got = SDL_PollEvent(&ev);
        }
        if (busy) {
            SessionStart start;
            if (app.room->poll(start)) {
                Game& g = *app.games[app.room_game];
                auto spec = g.adapter->online(g.root, start);
                if (spec) launch(g, *spec);
                else app.room_error = "La sala empezó pero este PC no tiene puesto: no se abre el juego.";
                app.page = Page::Game;
            }
            redraw = std::max(redraw, 1);
        }
        if (redraw <= 0 && screenshot.empty()) continue;
        redraw--;

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(px(0), px(0)));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
        sidebar(app, io.DisplaySize.y);
        ImGui::SameLine(0, 0);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(P.bg));
        ImGui::BeginChild("##main", ImVec2(0, io.DisplaySize.y));
        float width = std::min(ImGui::GetContentRegionAvail().x - px(96.0f), px(860.0f));
        ImGui::Indent(px(48));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(P.card));
        if (app.games.empty()) {
            ImGui::SetCursorPosY(px(40));
            text(g_semibold, 20, P.text, "No hay juegos en el catálogo.");
        } else if (app.page == Page::Room && app.room_game >= 0) {
            room_page(app, width);
        } else {
            game_page(app, *app.games[app.selected], width);
        }
        for (const auto& e : app.catalog_errors) text(g_regular, 12, P.bad, ("Manifest rechazado: " + e).c_str(), width);
        ImGui::PopStyleColor();
        ImGui::Unindent(px(48));
        ImGui::EndChild();
        ImGui::PopStyleColor();
        // Atajos: flechas arriba/abajo cambian de juego, Intro = Jugar (también con mando vía navegación).
        if (!io.WantTextInput && app.page == Page::Game) {
            if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) app.selected = std::min(app.selected + 1, (int)app.games.size() - 1);
            if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) app.selected = std::max(app.selected - 1, 0);
        }
        ImGui::End();
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 16, 18, 24, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        if (!screenshot.empty() && ++frames == 4) {
            if (SDL_Surface* shot = SDL_RenderReadPixels(renderer, nullptr)) {
                SDL_SaveBMP(shot, screenshot.c_str());
                SDL_DestroySurface(shot);
            }
            quit = true;
        }
        SDL_RenderPresent(renderer);
        if (first_frame) {
            first_frame = false;
            double ms = std::chrono::duration<double, std::milli>(Clock::now() - t_start).count();
            std::error_code e;
            fs::create_directories(local_app_data() / "WiiLauncher", e);
            std::ofstream(local_app_data() / "WiiLauncher" / "metricas.txt", std::ios::app) << "primer_fotograma_ms=" << ms << "\n";
        }
        // Mientras alguna comprobación sigue en marcha, otro fotograma cuando termine (wake()).
    }
    if (app.room) app.room->leave();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
