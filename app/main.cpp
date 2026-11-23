// Launcher unificado: biblioteca de juegos, ficha por juego y sala online.
// La UI solo habla con el contrato de adaptadores (wl/adapter.h); no sabe nada de cada juego.
// Bucle por eventos: sin entrada ni trabajo pendiente la ventana no redibuja (CPU ~0 en reposo).
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "mods_service.h"
#include "prefs.h"
#include "update_service.h"
#include "wl/catalog.h"
#include "wl/mods/launch.h"
#include "wl/mods/lobby_mods.h"
#include "wl/mods/session.h"
#include "wl/process.h"
#include "wl/update.h"

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
    s.Colors[ImGuiCol_Tab] = v4(P.card);
    s.Colors[ImGuiCol_TabHovered] = v4(P.card_hover);
    s.Colors[ImGuiCol_TabSelected] = v4(P.line);
    s.Colors[ImGuiCol_TabSelectedOverline] = v4(P.accent);
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
    std::atomic<bool> preparing{false};   // preparando el disco virtual de los mods antes de lanzar
    std::atomic<bool> end_session_on_exit{false};   // partida online con perfil de sesión: volver al habitual al cerrar
    std::string message;
    bool message_bad = false;
    // Online con servidor en la LAN (lan_wfc): IP del anfitrión que se escribe en la configuración del juego.
    char lan_ip[64] = {};
    bool lan_loaded = false;
};

enum class Page { Game, Room, Launcher, Mods };

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
    // Autoactualización del launcher (solo el launcher; nunca los juegos).
    std::unique_ptr<UpdateService> updates;
    bool update_later = false;      // «Más tarde»: no se vuelve a mostrar el aviso en esta sesión
    std::string installers_seen;    // aviso de instaladores ya leído (Entendido)
    std::string update_notice;      // resultado de una actualización anterior (éxito, fallo o recuperación)
    bool update_notice_bad = false;
    std::string update_why_not;     // por qué no se puede aplicar ahora
    bool quit_for_update = false;
    // Mods (docs/mods/). Subsistema aparte del updater.
    std::unique_ptr<ModsService> mods;
    int mods_game = -1;
    char mod_name[64] = {};
    char mod_version[32] = {};
    bool mod_activate = true;
    char new_profile[40] = {};
    std::string confirm_uninstall;   // clave pendiente de confirmar
    char gb_search[64] = {};
    int64_t consent_file = 0;       // archivo de GameBanana esperando «Sí, descargar»
    int gb_requested = -1;          // juego cuyo catálogo ya se pidió (evita pedirlo en cada fotograma)
    std::unique_ptr<mods::ManifestServer> manifest_server;   // anfitrión: publica sus mods junto a la sala
    bool auto_join = false;         // «unirme automáticamente cuando esté listo» (casilla)
    bool auto_join_armed = false;   // el usuario pidió preparar los mods con esa casilla marcada
    bool sync_consent = false;      // lista de descargas de la sala a la vista, esperando «Sí»
    bool join_preflight = false;
    bool ready_pending = false;
    std::string room_fingerprint;    // conjunto de mods con el que se entró en la sala (se revalida al empezar)
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

void set_message(Game& g, const std::string& msg, bool bad) {
    std::lock_guard<std::mutex> lock(g.mutex);
    g.message = msg;
    g.message_bad = bad;
}

// Vuelve al perfil de mods habitual si el activo es el temporal de una sala (C20). Rápido y sin red.
void end_mod_session(const Game& g) {
    const std::string& id = g.manifest.game_id;
    std::error_code ec;
    if (!mods::game_support(id) || !fs::exists(mods::default_mods_root(id) / "profiles" / "session.json", ec)) return;
    mods::ModStore store(id, mods::default_mods_root(id), mods::extracted_dir(id, g.root));
    std::string e;
    if (store.open(e)) store.end_session(e);
}

std::atomic<bool> g_minimize_launcher{false};

// El online arranca el juego por un evento de red, no por un clic: Windows no le da el primer plano y su
// ventana puede quedar detrás del launcher (el juego corre «invisible»). Se busca su ventana y se trae al
// frente; si Windows no lo permite, el launcher se minimiza para que se vea.
void bring_game_to_front(long long pid) {
#ifdef _WIN32
    AllowSetForegroundWindow(static_cast<DWORD>(pid));
    std::thread([pid] {
        struct Find { DWORD pid; HWND hwnd; };
        for (int i = 0; i < 600; ++i) {   // hasta 60 s: en red el juego espera a los demás antes de abrir su ventana
            Find f{static_cast<DWORD>(pid), nullptr};
            EnumWindows([](HWND h, LPARAM lp) -> BOOL {
                auto* f = reinterpret_cast<Find*>(lp);
                DWORD owner = 0;
                GetWindowThreadProcessId(h, &owner);
                if (owner == f->pid && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) { f->hwnd = h; return FALSE; }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&f));
            if (f.hwnd) {
                if (IsIconic(f.hwnd)) ShowWindow(f.hwnd, SW_RESTORE);
                BringWindowToTop(f.hwnd);
                SetForegroundWindow(f.hwnd);
                if (GetForegroundWindow() != f.hwnd) {
                    g_minimize_launcher = true;
                    wake();
                }
                return;
            }
            DWORD code = 0;
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
            bool alive = h && GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
            if (h) CloseHandle(h);
            if (!alive) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }).detach();
#else
    (void)pid;
#endif
}

void launch(Game& g, const LaunchSpec& spec) {
    if (g.running_pid.load() != 0) {
        set_message(g, "El juego ya está abierto.", false);
        return;
    }
    std::error_code e;
    fs::create_directories(g.root / "logs", e);
    Spawned p = spawn(spec);
    if (!p.ok) {
        set_message(g, p.error, true);
        return;
    }
    set_message(g, "Juego iniciado.", false);
    g.running_pid = p.pid;
    bring_game_to_front(p.pid);
    // Un hilo espera a que el juego termine; la UI no se bloquea.
    std::thread([&g, pid = p.pid] {
        int code = wait_exit(pid, -1);
        if (g.end_session_on_exit.exchange(false)) end_mod_session(g);
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

// Fingerprint del perfil de mods activo («vanilla» si no hay mods o el juego no los admite). Rápido: no
// lee los archivos de los mods, solo sus mod.json y el perfil.
std::string active_fingerprint(const Game& g, std::string* error = nullptr) {
    const std::string& id = g.manifest.game_id;
    if (!mods::game_support(id)) return mods::kVanilla;
    std::error_code ec;
    if (!fs::exists(mods::default_mods_root(id) / "profiles", ec)) return mods::kVanilla;
    mods::ModStore store(id, mods::default_mods_root(id), mods::extracted_dir(id, g.root));
    std::string e;
    mods::Profile p;
    if (!store.open(e) || !store.load_profile(store.active_profile(), p, e)) {
        if (error) *error = e;
        return mods::kVanilla;
    }
    mods::Effective eff = store.compute(p);
    if (!eff.problems.empty() && error) *error = eff.problems.front();
    return eff.fingerprint;
}

// Lanza con el perfil de mods activo. El disco virtual se prepara en un hilo (enlaces y hashes) y solo
// entonces se abre el juego. `expected`: fingerprint acordado en la sala; si el perfil cambió desde
// entonces, no se lanza (nunca se juega online con otro conjunto de mods).
void launch_modded(Game& g, LaunchSpec spec, std::string expected = {}) {
    if (g.running_pid.load() != 0 || g.preparing.exchange(true)) {
        set_message(g, "El juego ya está abierto o preparándose.", false);
        return;
    }
    set_message(g, "Preparando los mods…", false);
    wake();
    std::thread([&g, spec, expected]() mutable {
        mods::LaunchMods info;
        std::string error;
        bool ok = mods::prepare_launch(g.manifest.game_id, g.root, spec, info, error);
        if (ok && !expected.empty() && info.fingerprint != expected) {
            ok = false;
            error = "Tus mods han cambiado desde que entraste en la sala: no se abre el juego para no desincronizar la partida.";
        }
        g.preparing = false;
        if (!ok) {
            set_message(g, "No se puede lanzar con mods: " + error, true);
            if (!expected.empty()) end_mod_session(g);
        } else {
            if (!expected.empty()) g.end_session_on_exit = true;
            launch(g, spec);
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
            if (app.selected != (int)i || app.page == Page::Launcher) app.page = Page::Game;
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
    // Pie: versión del launcher y acceso a sus ajustes. Si hay versión nueva, un aviso discreto.
    UpdateSnapshot u = app.updates ? app.updates->snapshot() : UpdateSnapshot{};
    bool news = u.state == UpdState::Available || u.state == UpdState::Downloading || u.state == UpdState::Verifying || u.state == UpdState::Ready;
    ImGui::SetCursorPos(ImVec2(px(12), height - px(news ? 112 : 84)));
    ImGui::PushStyleColor(ImGuiCol_Header, v4(P.card));
    ImGui::BeginDisabled(app.page == Page::Room);  // la sala se cierra con sus propios botones
    if (news) {
        ImGui::PushStyleColor(ImGuiCol_Text, v4(P.accent));
        ImGui::PushFont(g_semibold, 13.0f);
        std::string label = (u.state == UpdState::Ready ? "Lista para instalar: v" : "Actualización disponible: v") + u.available + "##upd";
        if (ImGui::Selectable(label.c_str(), app.page == Page::Launcher, ImGuiSelectableFlags_None, ImVec2(px(256), px(24)))) app.page = Page::Launcher;
        ImGui::PopFont();
        ImGui::PopStyleColor();
        ImGui::SetCursorPosX(px(12));
    }
    ImGui::PushFont(g_regular, 13.0f);
    if (ImGui::Selectable("Ajustes del launcher##launcher", app.page == Page::Launcher, ImGuiSelectableFlags_None, ImVec2(px(256), px(24))))
        app.page = Page::Launcher;
    ImGui::PopFont();
    ImGui::EndDisabled();
    ImGui::PopStyleColor();
    ImGui::SetCursorPosX(px(24));
    text(g_regular, 11.5f, P.faint, ("Launcher v" + upd::current_version() + " · online directo, sin servidores").c_str());
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
void leave_room(App& app);
void open_mods(App& app);

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
    // El manifest dice que el paquete trae instalador, pero no está junto al launcher (p. ej. solo se bajó el ZIP del
    // launcher): se explica qué falta en vez de pedir un .exe, que confunde (se espera elegir el disco).
    if (!g.manifest.installer.empty()) {
        g.message = "No encuentro el instalador de " + g.manifest.display_name + " (" + g.manifest.installer +
                    ") junto al launcher. Descomprime el paquete completo del launcher, con su carpeta instaladores, y vuelve a pulsar.";
        g.message_bad = true;
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

// ---------------------------------------------------------------- actualizaciones del launcher

bool any_game_running(const App& app) {
    for (const auto& g : app.games)
        if (g->running_pid.load() != 0) return true;
    return false;
}

std::string size_text(uint64_t bytes) {
    char buf[32];
    if (bytes < 1024 * 1024) std::snprintf(buf, sizeof buf, "%.0f KB", bytes / 1024.0);
    else std::snprintf(buf, sizeof buf, "%.1f MB", bytes / (1024.0 * 1024.0));
    return buf;
}

void try_apply(App& app) {
    app.update_why_not.clear();
    if (app.updates->apply(any_game_running(app), app.update_why_not)) app.quit_for_update = true;
}

// Aviso discreto abajo a la derecha cuando hay versión nueva. No bloquea nada; «Más tarde» lo oculta en esta sesión.
// Aviso de instaladores de juegos actualizados (p. ej. parche del online de MP9): qué hacer para aplicarlo.
void installers_toast(App& app, const ImVec2& display, const UpdateSnapshot& u) {
    if (u.installers_note.empty() || app.installers_seen == u.installers_note) return;
    ImGui::SetNextWindowPos(ImVec2(display.x - px(24), display.y - px(24)), ImGuiCond_Always, ImVec2(1, 1));
    ImGui::SetNextWindowSize(ImVec2(px(400), 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, v4(P.card));
    ImGui::PushStyleColor(ImGuiCol_Border, v4(P.ok));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(18), px(16)));
    ImGui::Begin("##installers_toast", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
    text(g_semibold, 15, P.text, "Instalador actualizado");
    text(g_regular, 13, P.dim, u.installers_note.c_str(), px(360));
    ImGui::Dummy(ImVec2(0, px(2)));
    if (button("Entendido", Btn::Primary, true, px(130))) app.installers_seen = u.installers_note;
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

void update_toast(App& app, const ImVec2& display) {
    if (!app.updates || app.page == Page::Launcher) return;
    UpdateSnapshot u = app.updates->snapshot();
    if (u.state != UpdState::Available && u.state != UpdState::Ready) {
        installers_toast(app, display, u);
        return;
    }
    if (app.update_later) return;
    ImGui::SetNextWindowPos(ImVec2(display.x - px(24), display.y - px(24)), ImGuiCond_Always, ImVec2(1, 1));
    ImGui::SetNextWindowSize(ImVec2(px(400), 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, v4(P.card));
    ImGui::PushStyleColor(ImGuiCol_Border, v4(P.accent));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(18), px(16)));
    ImGui::Begin("##update_toast", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
    bool ready = u.state == UpdState::Ready;
    text(g_semibold, 15, P.text, ((ready ? "Actualización lista — v" : "Nueva versión disponible — v") + u.available).c_str());
    std::string line = u.notes.empty() ? std::string("Mejoras del launcher.") : u.notes[0];
    if (u.notes.size() > 1) line += " (+" + std::to_string(u.notes.size() - 1) + ")";
    text(g_regular, 13, P.dim, line.c_str(), px(360));
    text(g_regular, 12, P.faint, ready ? "Se instala al reiniciar el launcher. Tus juegos y ajustes no se tocan."
                                       : ("Descarga: " + size_text(u.download_size) + ". Solo el launcher; tus juegos y ajustes no se tocan.").c_str(),
         px(360));
    ImGui::Dummy(ImVec2(0, px(2)));
    if (ready) {
        if (button("Reiniciar y actualizar", Btn::Primary, true, px(200))) try_apply(app);
    } else if (button("Actualizar", Btn::Primary, true, px(130))) {
        app.updates->download();
    }
    ImGui::SameLine();
    if (button("Más tarde", Btn::Secondary, true, px(110))) app.update_later = true;
    ImGui::SameLine();
    if (button("Novedades", Btn::Ghost, true, px(110))) app.page = Page::Launcher;
    if (!app.update_why_not.empty()) text(g_regular, 12.5f, P.warn, app.update_why_not.c_str(), px(360));
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

// Ajustes del launcher: versión, canal, comprobar, novedades, progreso y errores.
void launcher_page(App& app, float width) {
    float x0 = ImGui::GetCursorPosX();
    ImGui::SetCursorPos(ImVec2(x0, px(34)));
    text(g_semibold, 30, P.text, "Ajustes del launcher");
    ImGui::SetCursorPosX(x0);
    text(g_regular, 13.5f, P.dim, "Las actualizaciones solo cambian el launcher. Tus juegos, partidas, salas y ajustes no se tocan, y nada se instala sin tu permiso.", width);
    ImGui::Dummy(ImVec2(0, px(10)));
    if (!app.update_notice.empty()) {
        ImGui::SetCursorPosX(x0);
        text(g_regular, 14, app.update_notice_bad ? P.warn : P.ok, app.update_notice.c_str(), width);
        ImGui::Dummy(ImVec2(0, px(6)));
    }
    if (!app.updates) return;
    UpdateSnapshot u = app.updates->snapshot();

    ImGui::SetCursorPosX(x0);
    ImGui::BeginChild("##updates", ImVec2(width, 0), ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar);
    float inner = width - px(44);
    ImGui::SetCursorPos(ImVec2(px(22), px(18)));
    text(g_semibold, 15, P.text, "Actualizaciones");
    ImGui::Dummy(ImVec2(0, px(2)));
    info_row("Versión instalada", "v" + u.installed, P.text);
    info_row("Canal", u.channel == "beta" ? "Beta (pruebas)" : "Estable", P.text);
    info_row("Última comprobación", u.last_check.empty() ? "—" : u.last_check, P.dim);

    // Estado
    std::string status;
    ImU32 color = P.dim;
    switch (u.state) {
        case UpdState::Idle: status = u.enabled ? "Sin comprobar todavía." : u.info; break;
        case UpdState::Checking: status = "Buscando actualizaciones…"; break;
        case UpdState::UpToDate: status = u.info.empty() ? "Estás al día." : u.info; color = P.ok; break;
        case UpdState::Available: status = "Hay una versión nueva: v" + u.available + (u.published.empty() ? "" : " (" + u.published + ")"); color = P.accent; break;
        case UpdState::Downloading: status = "Descargando… " + size_text(u.done) + " de " + size_text(u.total); color = P.accent; break;
        case UpdState::Verifying: status = "Comprobando la firma y los archivos…"; color = P.accent; break;
        case UpdState::Ready: status = "v" + u.available + " descargada y verificada. Se instala al reiniciar el launcher."; color = P.ok; break;
        case UpdState::Installing: status = "Cerrando el launcher para instalar…"; color = P.accent; break;
        case UpdState::Error: status = "No se pudo completar: " + u.error + " Puedes seguir jugando."; color = P.warn; break;
    }
    ImGui::Dummy(ImVec2(0, px(4)));
    ImGui::SetCursorPosX(px(22));
    text(g_regular, 14, color, status.c_str(), inner);
    if (!u.installers_note.empty()) {
        ImGui::SetCursorPosX(px(22));
        text(g_regular, 13, P.ok, u.installers_note.c_str(), inner);
    }
    if (u.state == UpdState::Available && !u.info.empty()) {
        ImGui::SetCursorPosX(px(22));
        text(g_regular, 12.5f, P.faint, u.info.c_str(), inner);
    }
    if (u.state == UpdState::Downloading) {
        ImGui::SetCursorPosX(px(22));
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, v4(P.accent));
        ImGui::ProgressBar(u.total ? (float)((double)u.done / (double)u.total) : 0.0f, ImVec2(inner, px(8)), "");
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, px(4)));
    ImGui::SetCursorPosX(px(22));
    bool busy = u.state == UpdState::Checking || u.state == UpdState::Downloading || u.state == UpdState::Verifying || u.state == UpdState::Installing;
    if (u.state == UpdState::Available) {
        if (button("Actualizar", Btn::Primary, true, px(140))) app.updates->download();
        ImGui::SameLine();
        text(g_regular, 12.5f, P.faint, ("Descarga " + size_text(u.download_size) + " (versión completa " + size_text(u.update_size) + ")").c_str());
    } else if (u.state == UpdState::Downloading) {
        if (button("Cancelar", Btn::Secondary, true, px(140))) app.updates->cancel();
    } else if (u.state == UpdState::Ready) {
        bool running = any_game_running(app);
        if (button("Reiniciar y actualizar", Btn::Primary, !running, px(220), "Cierra el juego antes: no se reinicia el launcher a mitad de partida.")) try_apply(app);
    } else {
        const char* label = u.state == UpdState::Error ? "Reintentar" : "Buscar actualizaciones";
        if (button(label, Btn::Secondary, u.enabled && !busy, px(210), u.enabled ? "Espera a que termine." : "Esta copia no tiene actualizaciones configuradas.")) app.updates->check_now();
    }
    if (!app.update_why_not.empty()) {
        ImGui::SetCursorPosX(px(22));
        text(g_regular, 13, P.warn, app.update_why_not.c_str(), inner);
    }
    ImGui::Dummy(ImVec2(0, px(6)));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(px(4), px(4)));  // casillas a escala del texto
    ImGui::SetCursorPosX(px(22));
    bool auto_check = u.auto_check;
    if (ImGui::Checkbox("Buscar actualizaciones al abrir (como mucho cada 6 horas)", &auto_check)) app.updates->set_auto(auto_check);
    ImGui::SetCursorPosX(px(22));
    bool beta = u.channel == "beta";
    ImGui::BeginDisabled(busy);
    if (ImGui::Checkbox("Recibir versiones beta (para probar novedades; pueden fallar)", &beta)) app.updates->set_channel(beta);
    ImGui::EndDisabled();
    ImGui::PopStyleVar();
    ImGui::Dummy(ImVec2(0, px(12)));
    ImGui::EndChild();

    if (!u.notes.empty() && !u.available.empty() && u.state != UpdState::UpToDate) {
        ImGui::Dummy(ImVec2(0, px(10)));
        ImGui::SetCursorPosX(x0);
        ImGui::BeginChild("##notes", ImVec2(width, 0), ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar);
        ImGui::SetCursorPos(ImVec2(px(22), px(18)));
        text(g_semibold, 15, P.text, ("Novedades de la v" + u.available).c_str());
        for (const auto& n : u.notes) {
            ImGui::SetCursorPosX(px(22));
            text(g_regular, 13.5f, P.dim, ("•  " + n).c_str(), inner);
        }
        ImGui::Dummy(ImVec2(0, px(12)));
        ImGui::EndChild();
    }
    ImGui::Dummy(ImVec2(0, px(10)));
    ImGui::SetCursorPosX(x0);
    text(g_regular, 12, P.faint, "Si algo falla, el launcher vuelve solo a la versión anterior. Registro: %LOCALAPPDATA%\\WiiLauncher\\updater.log", width);
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
    } else if (button("  Jugar  ", Btn::Primary, ready && !running && !g.preparing, px(150), why)) {
        launch_modded(g, g.adapter->offline(g.root));
    }
    bool wfc = m.has("lan_wfc");
    if (!wfc) {
        ImGui::SameLine();
        const char* why_online = !online ? "El online de este juego aún no está en este launcher. Usa su launcher actual." : why;
        if (button("Crear sala", Btn::Secondary, online && ready && !running, px(140), why_online)) open_room(app, true);
        ImGui::SameLine();
        if (button("Unirse", Btn::Secondary, online && ready && !running, px(120), why_online)) open_room(app, false);
    }
    const mods::GameModSupport* mod_support = mods::game_support(m.game_id);
    if (mod_support && mod_support->proven) {
        ImGui::SameLine();
        if (button("Mods", Btn::Secondary, ready && !running, px(100), why)) open_mods(app);
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
    if (mod_support && mod_support->proven && probed && s.ready()) {
        // Caché corta: la ficha se redibuja con cada evento y abrir el almacén lee la FST del disco.
        static std::map<std::string, std::pair<Clock::time_point, std::string>> cache;
        auto& c = cache[m.game_id];
        if (c.second.empty() || Clock::now() - c.first > std::chrono::seconds(3)) c = {Clock::now(), active_fingerprint(g)};
        const std::string& fp = c.second;
        info_row("Mods", fp == mods::kVanilla ? "Sin mods (perfil original)" : "Perfil con mods · " + fp.substr(0, 12), fp == mods::kVanilla ? P.dim : P.accent);
    } else if (!mod_support) {
        info_row("Mods", m.game_id == "mkwii" ? "Requiere un perfil compilado de WiiCompiled (no disponible)" : "No admitidos", P.faint);
    } else {
        info_row("Mods", "Pendientes de verificar en este port", P.faint);
    }
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

// ---------------------------------------------------------------- mods

void open_mods(App& app) {
    Game& g = *app.games[app.selected];
    if (!app.mods) app.mods = std::make_unique<ModsService>([] { wake(); });
    if (app.mods_game != app.selected) {
        app.mods->open(g.manifest.game_id, g.manifest.display_name, g.root);
        app.mods_game = app.selected;
    }
    app.page = Page::Mods;
}

struct ModPick {
    App* app;
};

void SDLCALL picked_mod(void* data, const char* const* files, int) {
    auto* p = static_cast<ModPick*>(data);
    if (files && files[0] && p->app->mods) p->app->mods->preview(fs::u8path(files[0]));
    delete p;
    wake();
}

std::string human_size(uint64_t b) {
    char buf[32];
    if (b >= (1ull << 20)) std::snprintf(buf, sizeof buf, "%.1f MB", b / 1048576.0);
    else std::snprintf(buf, sizeof buf, "%.0f KB", b / 1024.0);
    return buf;
}

void mods_preview_card(App& app, const ModsSnapshot& m, float width) {
    const mods::PackagePlan& plan = m.preview;
    ImGui::BeginChild("##preview", ImVec2(width, 0), ImGuiChildFlags_AutoResizeY);
    ImGui::SetCursorPos(ImVec2(px(22), px(18)));
    text(g_semibold, 16, P.text, "Revisar antes de instalar");
    ImGui::SetCursorPosX(px(22));
    text(g_regular, 12.5f, P.faint, m.preview_source.c_str(), width - px(44));
    ImGui::SetCursorPosX(px(22));
    std::string fmt = plan.format == mods::ModFormat::Riivolution ? "Riivolution (solo archivos)" : plan.format == mods::ModFormat::Files ? "Archivos del disco" : "—";
    text(g_regular, 14, P.dim, ("Formato: " + fmt + " · " + human_size(plan.payload_bytes) + " · mod de datos (no ejecuta código)").c_str(), width - px(44));
    for (const auto& b : plan.blocking) {
        ImGui::SetCursorPosX(px(22));
        text(g_regular, 14, P.bad, ("No se puede instalar: " + b).c_str(), width - px(44));
    }
    for (const auto& w : plan.warnings) {
        ImGui::SetCursorPosX(px(22));
        text(g_regular, 13, P.warn, w.c_str(), width - px(44));
    }
    for (const auto& dependency : plan.dependencies) {
        ImGui::SetCursorPosX(px(22));
        text(g_regular, 13, P.dim, ("Requiere " + dependency.id + " " + dependency.version).c_str(), width - px(44));
    }
    if (plan.ok()) {
        size_t n = 0;
        for (const auto& [id, ops] : plan.patches) n += ops.size();
        ImGui::SetCursorPosX(px(22));
        text(g_semibold, 13.5f, P.ok, ("Cambia " + std::to_string(n) + " archivo(s) de tu disco (sin tocarlo: se aplica al lanzar):").c_str());
        int shown = 0;
        for (const auto& [id, ops] : plan.patches)
            for (const auto& op : ops)
                if (shown++ < 8) {
                    ImGui::SetCursorPosX(px(34));
                    text(g_regular, 12.5f, P.dim, op.disc.c_str());
                }
        if (shown > 8) {
            ImGui::SetCursorPosX(px(34));
            text(g_regular, 12.5f, P.faint, ("… y " + std::to_string(shown - 8) + " más").c_str());
        }
    }
    if (!plan.ignored.empty()) {
        ImGui::SetCursorPosX(px(22));
        text(g_regular, 12.5f, P.faint, (std::to_string(plan.ignored.size()) + " archivo(s) del paquete no se usan (documentación u otra región).").c_str());
        if (ImGui::IsItemHovered()) {
            std::string all;
            for (size_t i = 0; i < plan.ignored.size() && i < 20; ++i) all += plan.ignored[i] + "\n";
            ImGui::SetTooltip("%s", all.c_str());
        }
    }
    if (plan.ok() && !m.preview_update.empty()) {
        ImGui::SetCursorPosX(px(22));
        const std::string update = m.preview_profile.empty()
            ? "La nueva versión se añadirá a Instalados."
            : "Se actualizará el perfil «" + m.preview_profile + "» conservando el orden, la activación y las opciones compatibles.";
        text(g_regular, 13, P.dim, (update + " La versión anterior seguirá disponible.").c_str(), width - px(44));
    } else if (plan.ok()) {
        ImGui::SetCursorPosX(px(22));
        text(g_regular, 13, P.faint, "Nombre y versión (como los llama el autor)");
        ImGui::SetCursorPosX(px(22));
        ImGui::SetNextItemWidth(px(300));
        ImGui::InputTextWithHint("##modname", "Nombre del mod", app.mod_name, sizeof app.mod_name);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(px(120));
        ImGui::InputTextWithHint("##modver", "1.0", app.mod_version, sizeof app.mod_version);
        ImGui::SetCursorPosX(px(22));
        ImGui::Checkbox("Activarlo en el perfil actual", &app.mod_activate);
    }
    ImGui::SetCursorPosX(px(22));
    if (button(m.preview_update.empty() ? "Instalar" : "Actualizar", Btn::Primary, plan.ok() && !m.busy, px(140), "Este paquete no se puede instalar.")) {
        if (m.preview_update.empty()) app.mods->install_preview(app.mod_name, app.mod_version, app.mod_activate);
        else app.mods->install_preview("", "", false);
        app.mod_name[0] = app.mod_version[0] = 0;
    }
    ImGui::SameLine();
    if (button("Cancelar", Btn::Ghost, true, px(130))) app.mods->discard_preview();
    ImGui::Dummy(ImVec2(0, px(12)));
    ImGui::EndChild();
}

const char* verdict_text(const mods::QuickCheck& q, ImU32& color) {
    switch (q.verdict) {
        case mods::QuickVerdict::Compatible: color = P.ok; return "Compatible con tu disco";
        case mods::QuickVerdict::Incompatible: color = P.bad; return "No compatible";
        default: color = P.warn; return "Se comprueba al descargar";
    }
}

void mods_explore_tab(App& app, const ModsSnapshot& m, float x0, float width) {
    if (!m.gb_game) {
        ImGui::SetCursorPosX(x0);
        text(g_regular, 14, P.dim, "Este juego no tiene catálogo en GameBanana.");
        return;
    }
    if (!m.catalog_loaded && app.gb_requested != app.mods_game) { app.mods->browse(1, ""); app.gb_requested = app.mods_game; }
    auto installed = [&](int64_t id) {
        const std::string key = "gamebanana." + std::to_string(id);
        return std::any_of(m.installed.begin(), m.installed.end(), [&](const mods::InstalledMod& mod) { return mod.id() == key; });
    };
    if (m.has_detail) {
        const mods::CatalogDetail& d = m.detail;
        ImGui::SetCursorPosX(x0);
        ImGui::BeginChild("##detail", ImVec2(width, 0), ImGuiChildFlags_AutoResizeY);
        ImGui::SetCursorPos(ImVec2(px(22), px(18)));
        text(g_semibold, 17, P.text, d.item.name.c_str(), width - px(44));
        ImGui::SetCursorPosX(px(22));
        text(g_regular, 13, P.dim, ("de " + d.item.author + " · " + d.item.category +
            (d.item.version.empty() ? "" : " · versión " + d.item.version) +
            (installed(d.item.id) ? " · Instalado" : "")).c_str(), width - px(44));
        ImGui::SetCursorPosX(px(22));
        text(g_regular, 12.5f, P.faint, ("Licencia: " + (d.license.empty() ? std::string("no indicada") : d.license)).c_str(), width - px(44));
        if (!d.description.empty()) {
            ImGui::SetCursorPosX(px(22));
            text(g_regular, 13, P.dim, d.description.substr(0, 500).c_str(), width - px(44));
        }
        ImGui::SetCursorPosX(px(22));
        text(g_regular, 12, P.faint, "Los datos de la ficha vienen del autor y de GameBanana: no prueban que el mod sea seguro ni que funcione. "
                                     "El launcher lo comprueba él mismo antes y después de descargar.", width - px(44));
        for (size_t i = 0; i < d.files.size(); ++i) {
            const mods::CatalogFile& f = d.files[i];
            ImGui::PushID((int)i);
            ImU32 vc = P.warn;
            const char* vt = i < m.checks.size() ? verdict_text(m.checks[i], vc) : "—";
            ImGui::SetCursorPosX(px(22));
            const std::string version = d.version_for(f);
            text(g_semibold, 13.5f, P.text, (f.name + (version.empty() ? "" : " · " + version) + " · " +
                human_size(f.size) + " · antivirus de GameBanana: " + (f.av_result.empty() ? "?" : f.av_result)).c_str(), width - px(44));
            ImGui::SetCursorPosX(px(34));
            std::string why = i < m.checks.size() ? m.checks[i].reason : "";
            if (i < m.checks.size() && m.checks[i].files) why = "cambia " + std::to_string(m.checks[i].files) + " archivo(s) de tu disco";
            text(g_regular, 13, vc, (std::string(vt) + (why.empty() ? "" : ": " + why)).c_str(), width - px(56));
            bool installable = i < m.checks.size() && m.checks[i].verdict != mods::QuickVerdict::Incompatible && m.checks[i].reason.find(".7z") == std::string::npos;
            ImGui::SetCursorPosX(px(34));
            if (app.consent_file == f.id) {
                text(g_regular, 13, P.warn, ("¿Descargar «" + f.name + "» (" + human_size(f.size) + ") desde gamebanana.com? Se comprobará y te pediré confirmación antes de instalar.").c_str(), width - px(56));
                ImGui::SetCursorPosX(px(34));
                if (button("Sí, descargar", Btn::Primary, !m.busy, px(160))) { app.mods->download_file(d.item.id, f.id); app.consent_file = 0; }
                ImGui::SameLine();
                if (button("No", Btn::Ghost, true, px(70))) app.consent_file = 0;
            } else if (button("Descargar y revisar", Btn::Secondary, installable && !m.busy, px(200), "Este archivo no se puede instalar en este port.")) {
                app.consent_file = f.id;
            }
            ImGui::PopID();
        }
        ImGui::SetCursorPosX(px(22));
        if (button("Abrir página", Btn::Ghost, true, px(150))) SDL_OpenURL(d.item.profile_url.c_str());
        ImGui::SameLine();
        if (button("‹ Volver a la lista", Btn::Ghost, true, px(190))) { app.mods->close_detail(); app.consent_file = 0; }
        ImGui::Dummy(ImVec2(0, px(12)));
        ImGui::EndChild();
        return;
    }
    ImGui::SetCursorPosX(x0);
    ImGui::SetNextItemWidth(px(320));
    bool go = ImGui::InputTextWithHint("##gbsearch", "Buscar en GameBanana", app.gb_search, sizeof app.gb_search, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (button("Buscar", Btn::Secondary, !m.busy, px(110)) || go) app.mods->browse(1, app.gb_search);
    if (!m.catalog_error.empty()) {
        ImGui::SetCursorPosX(x0);
        text(g_regular, 13.5f, P.bad, m.catalog_error.c_str(), width);
        ImGui::SetCursorPosX(x0);
        if (button("Reintentar", Btn::Ghost, !m.busy, px(130))) app.mods->browse(m.page, m.search);
    }
    if (m.catalog.from_cache) {
        ImGui::SetCursorPosX(x0);
        text(g_regular, 12, P.faint, "Mostrando datos guardados (sin conexión o consultados hace poco).");
    }
    ImGui::SetCursorPosX(x0);
    ImGui::BeginChild("##catalog", ImVec2(width, 0), ImGuiChildFlags_AutoResizeY);
    ImGui::Dummy(ImVec2(0, px(8)));
    for (const auto& it : m.catalog.items) {
        ImGui::PushID((int)it.id);
        ImGui::SetCursorPosX(px(22));
        if (ImGui::Selectable(("##it" + std::to_string(it.id)).c_str(), false, ImGuiSelectableFlags_None, ImVec2(width - px(44), px(40)))) app.mods->open_detail(it.id);
        ImGui::SameLine(px(32));
        ImGui::BeginGroup();
        text(g_semibold, 14, P.text, it.name.c_str());
        text(g_regular, 12, P.faint, (it.category + " · " + it.author + (installed(it.id) ? " · Instalado" : "")).c_str());
        ImGui::EndGroup();
        ImGui::PopID();
    }
    if (m.catalog.items.empty() && m.catalog_loaded && m.catalog_error.empty()) {
        ImGui::SetCursorPosX(px(22));
        text(g_regular, 13.5f, P.dim, "Sin resultados.");
    }
    ImGui::SetCursorPosX(px(22));
    if (button("‹ Anterior", Btn::Ghost, m.page > 1 && !m.busy, px(130))) app.mods->browse(m.page - 1, m.search);
    ImGui::SameLine();
    text(g_regular, 13, P.faint, ("Página " + std::to_string(m.page) + " · " + std::to_string(m.catalog.total) + " mods").c_str());
    ImGui::SameLine();
    if (button("Siguiente ›", Btn::Ghost, !m.catalog.complete && (int64_t)m.page * 20 < m.catalog.total && !m.busy, px(130))) app.mods->browse(m.page + 1, m.search);
    ImGui::SetCursorPosX(px(22));
    text(g_regular, 12, P.faint, "Abre un mod para ver si es compatible con tu disco antes de descargar nada.");
    ImGui::Dummy(ImVec2(0, px(12)));
    ImGui::EndChild();
}

void mods_updates_tab(App& app, const ModsSnapshot& m, float x0, float width) {
    if (m.has_detail) {   // «Ver y actualizar» abre la ficha aquí mismo
        mods_explore_tab(app, m, x0, width);
        return;
    }
    ImGui::SetCursorPosX(x0);
    text(g_regular, 13.5f, P.dim, "Solo se buscan al pulsar el botón (nunca al arrancar). Una versión nueva se instala aparte: "
                                  "la anterior sigue instalada y puedes volver a ella marcándola en Instalados.", width);
    ImGui::SetCursorPosX(x0);
    if (button("Buscar actualizaciones", Btn::Secondary, !m.busy, px(230))) app.mods->check_updates();
    if (!m.updates_checked) return;
    ImGui::SetCursorPosX(x0);
    ImGui::BeginChild("##updates", ImVec2(width, 0), ImGuiChildFlags_AutoResizeY);
    ImGui::Dummy(ImVec2(0, px(8)));
    if (m.updates.empty()) {
        ImGui::SetCursorPosX(px(22));
        text(g_regular, 13.5f, P.dim, "No hay mods de GameBanana instalados (los importados a mano no se actualizan solos).");
    }
    for (const auto& u : m.updates) {
        ImGui::PushID(u.key.c_str());
        ImGui::SetCursorPosX(px(22));
        text(g_semibold, 14, P.text, (u.name + "  " + u.installed_version).c_str());
        ImGui::SetCursorPosX(px(34));
        text(g_regular, 13, u.available ? P.accent : P.faint, u.note.c_str(), width - px(56));
        if (u.available) {
            ImGui::SetCursorPosX(px(34));
            if (button("Ver y actualizar", Btn::Secondary, !m.busy, px(180))) app.mods->open_detail(u.mod_id, u.key);
        }
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, px(12)));
    ImGui::EndChild();
}

void mods_page(App& app, float width) {
    ModsSnapshot m = app.mods ? app.mods->snapshot() : ModsSnapshot{};
    Game& g = *app.games[app.mods_game >= 0 ? app.mods_game : app.selected];
    bool running = g.running_pid.load() != 0 || g.preparing;
    float x0 = ImGui::GetCursorPosX();
    ImGui::SetCursorPos(ImVec2(x0, px(34)));
    if (button("‹ Volver", Btn::Ghost, true, px(110))) {
        app.page = Page::Game;
        return;
    }
    ImGui::SetCursorPosX(x0);
    text(g_semibold, 28, P.text, ("Mods · " + g.manifest.display_name).c_str());
    ImGui::SetCursorPosX(x0);
    text(g_regular, 13.5f, P.dim,
         "Solo mods de datos que este port carga de verdad (archivos del disco y Riivolution sin código). Tu disco y tus partidas "
         "guardadas no se modifican: los mods se aplican al lanzar, sobre un disco virtual.", width);
    if (!m.base.disc_id.empty()) {
        ImGui::SetCursorPosX(x0);
        text(g_regular, 12, P.faint, ("Disco " + m.base.base_content_id).c_str());
    }
    ImGui::Dummy(ImVec2(0, px(4)));
    if (m.busy) {
        ImGui::SetCursorPosX(x0);
        std::string t = m.busy_text;
        if (m.total) t += "  (" + std::to_string((int)(100.0 * m.done / m.total)) + " %)";
        text(g_regular, 14, P.accent, t.c_str(), width);
        ImGui::SetCursorPosX(x0);
        if (button("Cancelar operación", Btn::Ghost, true, px(190))) app.mods->cancel();
    }
    if (!m.error.empty() || !m.notice.empty()) {
        ImGui::SetCursorPosX(x0);
        text(g_regular, 14, m.error.empty() ? P.ok : P.bad, (m.error.empty() ? m.notice : m.error).c_str(), width);
        ImGui::SetCursorPosX(x0);
        if (button("Entendido", Btn::Ghost, true, px(130))) app.mods->clear_messages();
    }
    if (!m.ready) return;
    if (running) {
        ImGui::SetCursorPosX(x0);
        text(g_regular, 14, P.warn, "El juego está abierto: los cambios de mods se aplican la próxima vez que lo lances (no hay recarga en caliente).", width);
    }

    if (m.has_preview) {
        ImGui::SetCursorPosX(x0);
        mods_preview_card(app, m, width);
        return;
    }

    // Perfil
    ImGui::SetCursorPosX(x0);
    ImGui::BeginChild("##profile", ImVec2(width, 0), ImGuiChildFlags_AutoResizeY);
    ImGui::SetCursorPos(ImVec2(px(22), px(18)));
    text(g_semibold, 15, P.text, "Perfil");
    ImGui::SetCursorPosX(px(22));
    ImGui::SetNextItemWidth(px(220));
    if (ImGui::BeginCombo("##profiles", m.active.c_str())) {
        for (const auto& name : m.profiles)
            if (ImGui::Selectable(name.c_str(), name == m.active) && name != m.active) app.mods->activate_profile(name);
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(px(170));
    ImGui::InputTextWithHint("##newprofile", "nuevo-perfil", app.new_profile, sizeof app.new_profile);
    ImGui::SameLine();
    if (button("Crear (copia)", Btn::Ghost, app.new_profile[0] && !m.busy, px(150))) {
        app.mods->create_profile(app.new_profile, true);
        app.new_profile[0] = 0;
    }
    ImGui::SameLine();
    if (button("Borrar", Btn::Ghost, m.active != "default" && !m.busy, px(100), "El perfil «default» no se borra.")) app.mods->delete_profile(m.active);
    ImGui::SetCursorPosX(px(22));
    const mods::Effective& eff = m.effective;
    std::string summary = eff.vanilla() ? "Sin mods activos: el juego arranca con tu disco original."
                                        : std::to_string(eff.manifest_mods.size()) + " mod(s) activos · " + std::to_string(eff.files.size()) +
                                              " archivo(s) cambiados · huella " + eff.fingerprint.substr(0, 12);
    text(g_regular, 13.5f, eff.vanilla() ? P.dim : P.accent, summary.c_str(), width - px(44));
    for (const auto& p : eff.problems) {
        ImGui::SetCursorPosX(px(22));
        text(g_regular, 13.5f, P.bad, ("Problema: " + p).c_str(), width - px(44));
    }
    for (const auto& c : eff.conflicts) {
        ImGui::SetCursorPosX(px(22));
        text(g_regular, 13, P.warn, ("Conflicto: " + c).c_str(), width - px(44));
    }
    ImGui::Dummy(ImVec2(0, px(10)));
    ImGui::EndChild();

    // Acciones
    ImGui::Dummy(ImVec2(0, px(4)));
    ImGui::SetCursorPosX(x0);
    if (button("Importar ZIP…", Btn::Primary, !m.busy, px(160))) {
        static const SDL_DialogFileFilter filters[] = {{"Mods (ZIP)", "zip"}};
        SDL_ShowOpenFileDialog(picked_mod, new ModPick{&app}, g_window, filters, 1, nullptr, false);
    }
    ImGui::SameLine();
    if (button("Importar carpeta…", Btn::Secondary, !m.busy, px(190))) SDL_ShowOpenFolderDialog(picked_mod, new ModPick{&app}, g_window, nullptr, false);
    ImGui::SameLine();
    if (button("Abrir GameBanana", Btn::Ghost, true, px(190))) {
        static const std::map<std::string, std::string> pages = {{"wiiparty", "https://gamebanana.com/games/6370"}, {"mp9", "https://gamebanana.com/games/6248"}};
        auto it = pages.find(m.game_id);
        if (it != pages.end()) SDL_OpenURL(it->second.c_str());
    }
    ImGui::SetCursorPosX(x0);
    text(g_regular, 12, P.faint, "También puedes arrastrar un ZIP o una carpeta a esta ventana. Los .7z y .rar: descomprímelos y elige la carpeta.", width);

    // Pestañas: Instalados / Explorar / Actualizaciones
    ImGui::Dummy(ImVec2(0, px(6)));
    ImGui::SetCursorPosX(x0);
    int tab = 0;
    ImGui::PushFont(g_semibold, 14.5f);
    if (ImGui::BeginTabBar("##modtabs")) {
        if (ImGui::BeginTabItem("Instalados y perfiles")) { tab = 0; ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Explorar")) { tab = 1; ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Actualizaciones")) { tab = 2; ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::PopFont();
    if (tab == 1) { mods_explore_tab(app, m, x0, width); return; }
    if (tab == 2) { mods_updates_tab(app, m, x0, width); return; }
    ImGui::SetCursorPosX(x0);
    ImGui::BeginChild("##installed", ImVec2(width, 0), ImGuiChildFlags_AutoResizeY);
    ImGui::SetCursorPos(ImVec2(px(22), px(18)));
    text(g_semibold, 15, P.text, "Instalados");
    if (m.installed.empty()) {
        ImGui::SetCursorPosX(px(22));
        text(g_regular, 13.5f, P.dim, "Todavía no hay mods instalados para este juego.");
    }
    mods::Profile profile = m.profile;
    bool changed = false;
    // Primero, en orden de carga, los que están en el perfil; luego el resto.
    std::vector<const mods::InstalledMod*> order;
    for (const auto& e : profile.mods)
        for (const auto& im : m.installed)
            if (im.key == e.package) order.push_back(&im);
    for (const auto& im : m.installed)
        if (std::find(order.begin(), order.end(), &im) == order.end()) order.push_back(&im);
    for (size_t i = 0; i < order.size(); ++i) {
        const mods::InstalledMod& im = *order[i];
        ImGui::PushID(im.key.c_str());
        auto entry = std::find_if(profile.mods.begin(), profile.mods.end(), [&](const mods::ProfileEntry& e) { return e.package == im.key; });
        bool in_profile = entry != profile.mods.end();
        // Otra versión del mismo mod en el perfil: esta queda como versión anterior (para volver atrás).
        auto same_id = std::find_if(profile.mods.begin(), profile.mods.end(), [&](const mods::ProfileEntry& e) { return e.id == im.id(); });
        bool enabled = in_profile && entry->enabled;
        ImGui::SetCursorPosX(px(22));
        if (ImGui::Checkbox("##on", &enabled) && !m.busy) {
            if (in_profile) entry->enabled = enabled;
            else if (same_id != profile.mods.end()) { same_id->package = im.key; same_id->enabled = true; }
            else profile.mods.push_back({im.id(), im.key, true, {}});
            changed = true;
        }
        ImGui::SameLine();
        text(g_semibold, 14, in_profile ? P.text : P.dim, (im.name() + "  " + im.version()).c_str());
        ImGui::SameLine();
        text(g_regular, 12.5f, P.faint, (im.json["format"].str() + " · " + im.json["provider"].str() + (in_profile ? "" : same_id != profile.mods.end() ? " · otra versión activa" : "")).c_str());
        for (const auto& dep : im.json["dependencies"].elements()) {
            ImGui::SetCursorPosX(px(56));
            text(g_regular, 13, P.dim, ("Requiere " + dep["id"].str() + " " + dep["version"].str()).c_str());
        }
        ImGui::SetCursorPosX(px(56));
        if (in_profile) {
            size_t pos = (size_t)(entry - profile.mods.begin());
            if (button("Subir", Btn::Ghost, pos > 0 && !m.busy, px(80))) { std::swap(profile.mods[pos], profile.mods[pos - 1]); changed = true; }
            ImGui::SameLine();
            if (button("Bajar", Btn::Ghost, pos + 1 < profile.mods.size() && !m.busy, px(80))) { std::swap(profile.mods[pos], profile.mods[pos + 1]); changed = true; }
            ImGui::SameLine();
            if (button("Quitar del perfil", Btn::Ghost, !m.busy, px(170))) { profile.mods.erase(entry); changed = true; }
        } else if (app.confirm_uninstall == im.key) {
            text(g_regular, 13, P.warn, "¿Desinstalar? Se borra el paquete (tu disco y partidas no se tocan).");
            ImGui::SameLine();
            if (button("Sí, desinstalar", Btn::Secondary, !m.busy, px(160))) { app.mods->uninstall(im.key); app.confirm_uninstall.clear(); }
            ImGui::SameLine();
            if (button("No", Btn::Ghost, true, px(70))) app.confirm_uninstall.clear();
        } else if (button("Desinstalar", Btn::Ghost, !m.busy, px(140))) {
            app.confirm_uninstall = im.key;
        }
        // Opciones de Riivolution: cuentan para la compatibilidad online.
        if (in_profile && !changed) {
            for (const auto& opt : im.json["options"].elements()) {
                std::string id = opt["id"].str();
                int pick = (int)opt["default"].num(0);
                auto it = entry->choices.find(id);
                if (it != entry->choices.end()) pick = it->second;
                std::string current = pick > 0 && pick <= (int)opt["choices"].size() ? opt["choices"][(size_t)pick - 1]["name"].str() : "Desactivada";
                ImGui::SetCursorPosX(px(56));
                ImGui::SetNextItemWidth(px(260));
                if (ImGui::BeginCombo((opt["name"].str() + "##" + id).c_str(), current.c_str())) {
                    if (ImGui::Selectable("Desactivada", pick == 0)) { entry->choices[id] = 0; changed = true; }
                    for (size_t c = 0; c < opt["choices"].size(); ++c)
                        if (ImGui::Selectable(opt["choices"][c]["name"].str().c_str(), pick == (int)c + 1)) { entry->choices[id] = (int)c + 1; changed = true; }
                    ImGui::EndCombo();
                }
            }
        }
        ImGui::PopID();
        if (changed) break;   // el perfil ya cambió: se guarda y se redibuja
    }
    ImGui::Dummy(ImVec2(0, px(12)));
    ImGui::EndChild();
    if (changed) app.mods->save_profile(profile);
}

// ---------------------------------------------------------------- sala

std::string seat_name(int seat) { return seat < 0 ? "Sin jugador" : "Jugador " + std::to_string(seat + 1); }

// Token de sala = el del adaptador + fingerprint de los mods activos (si hay). El anfitrión, con el protocolo
// de siempre, rechaza en el HELLO a quien no tenga exactamente el mismo conjunto: falla cerrado sin tocar el
// netcode. Con perfil original el token no cambia (compatible con launchers sin mods).
std::string room_token(App& app, const Game& g, const InstallStatus& s) {
    std::string error;
    app.room_fingerprint = active_fingerprint(g, &error);
    if (!error.empty()) {
        app.room_error = "Tu perfil de mods tiene un problema (" + error + "). Revísalo en Mods.";
        return {};
    }
    return mods::mod_compat_token(g.adapter->compat_token(s), app.room_fingerprint);
}

// Anfitrión: publica el manifiesto de su perfil de mods activo en el puerto de la sala + 10 (solo lectura).
void start_manifest_server(App& app, const Game& g, const InstallStatus& status) {
    app.manifest_server.reset();
    if (!app.room || !mods::game_support(g.manifest.game_id)) return;
    if (app.port > 65535 - mods::kManifestPortOffset) {
        app.room_error = "El puerto de sala no deja espacio para publicar el manifiesto de mods.";
        return;
    }
    const auto& id = g.manifest.game_id;
    std::string lobby = std::to_string(std::chrono::system_clock::now().time_since_epoch().count() % 1000000007);
    mods::ModStore store(id, mods::default_mods_root(id), mods::extracted_dir(id, g.root));
    std::string error;
    mods::Profile profile;
    if (!store.open(error) || !store.load_profile(store.active_profile(), profile, error)) {
        app.room_error = "No se pudo publicar el perfil de mods: " + error + ".";
        return;
    }
    const auto manifest = mods::build_session_manifest(store, profile, lobby, profile.epoch, status.revision);
    if (manifest["effective_mod_fingerprint"].str() != app.room_fingerprint) {
        app.room_error = "El perfil de mods cambió al abrir la sala.";
        return;
    }
    auto server = std::make_unique<mods::ManifestServer>();
    bool ok = server->start((uint16_t)(app.port + mods::kManifestPortOffset), [body = manifest.dump()] { return body; }, error);
    if (ok) app.manifest_server = std::move(server);
    else app.room_error = "La sala funciona, pero los demás no podrán comparar mods contigo: " + error + ".";
}

void leave_room(App& app) {
    if (app.room) app.room->leave();
    app.room.reset();
    app.manifest_server.reset();
    app.auto_join_armed = false;
    app.join_preflight = false;
    app.ready_pending = false;
    app.sync_consent = false;
    if (app.room_game >= 0) {
        Game& g = *app.games[app.room_game];
        if (g.running_pid.load() == 0 && !g.preparing) {
            if (app.mods && app.mods_game == app.room_game) {
                if (app.mods->snapshot().busy) app.mods->cancel();
                else end_mod_session(g);
                app.mods->session_end();
            } else end_mod_session(g);
        }
    }
    if (app.mods) app.mods->clear_messages();
}

void open_room(App& app, bool host) {
    if (app.room || app.join_preflight) leave_room(app);
    Game& g = *app.games[app.selected];
    app.room_error.clear();
    app.join_preflight = false;
    app.ready_pending = false;
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
        const auto token = room_token(app, g, s);
        if (token.empty() || !app.room->host((uint16_t)app.port, app.name, token, app.room_error)) app.room.reset();
        app.addresses = local_addresses();
        start_manifest_server(app, g, s);
    }
}

void join_room(App& app, Game& g, const InstallStatus& s) {
    app.room = g.adapter->make_room();
    app.room_error.clear();
    const auto token = room_token(app, g, s);
    if (token.empty() || !app.room->join(app.address, (uint16_t)app.port, app.name, token, app.room_error)) app.room.reset();
}

void open_room_mods(App& app, const Game& game) {
    if (!app.mods) app.mods = std::make_unique<ModsService>([] { wake(); });
    if (app.mods_game != app.room_game) {
        app.mods->open(game.manifest.game_id, game.manifest.display_name, game.root);
        app.mods_game = app.room_game;
    }
}

const char* diff_text(mods::ModDiff d) {
    switch (d) {
        case mods::ModDiff::ExactMatch: return "Igual";
        case mods::ModDiff::Missing: return "Te falta";
        case mods::ModDiff::WrongVersion: return "Otra versión";
        case mods::ModDiff::WrongHash: return "Mismo número de versión, contenido distinto";
        case mods::ModDiff::Disabled: return "Lo tienes desactivado";
        case mods::ModDiff::ExtraActive: return "Lo tienes activo y el anfitrión no";
        case mods::ModDiff::LoadOrder: return "Otro orden de carga";
        case mods::ModDiff::ConfigMismatch: return "Otras opciones";
        case mods::ModDiff::Unsupported: return "No soportado";
        case mods::ModDiff::NoAuthorizedSource: return "Te falta y no hay descarga autorizada";
        case mods::ModDiff::IncompatibleGame: return "Juego o disco distinto";
    }
    return "?";
}

// Tarjeta «Mods de la sala» antes de unirse: compara con el anfitrión, explica cada diferencia y propone
// arreglarla (perfil de sesión, descargas con consentimiento). El anfitrión nunca decide qué se descarga.
void room_mods_card(App& app, Game& g, float x0, float width) {
    if (!mods::game_support(g.manifest.game_id) || !mods::game_support(g.manifest.game_id)->proven) return;
    open_room_mods(app, g);
    ModsSnapshot m = app.mods->snapshot();
    const bool same_room = m.session_address == app.address && m.session_port == app.port;
    ImGui::Dummy(ImVec2(0, px(6)));
    ImGui::SetCursorPosX(x0);
    ImGui::BeginChild("##roommods", ImVec2(width, 0), ImGuiChildFlags_AutoResizeY);
    ImGui::SetCursorPos(ImVec2(px(22), px(18)));
    text(g_semibold, 15, P.text, "Mods de la sala");
    ImGui::SetCursorPosX(px(22));
    text(g_regular, 13, P.dim, "Compara tus mods con los del anfitrión antes de entrar. Solo se comparan listas y huellas: "
                               "el anfitrión no te envía archivos ni puede hacer que descargues nada.", width - px(44));
    ImGui::SetCursorPosX(px(22));
    if (button("Comparar mods", Btn::Secondary, app.address[0] && !m.busy, px(170), "Escribe la dirección del anfitrión.")) {
        app.sync_consent = false;
        app.mods->session_compare(app.address, (uint16_t)app.port);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Unirme automáticamente cuando todo esté listo", &app.auto_join);
    if (m.busy) {
        ImGui::SetCursorPosX(px(22));
        std::string t = m.busy_text;
        if (m.total) t += "  (" + std::to_string((int)(100.0 * m.done / m.total)) + " %)";
        text(g_regular, 13.5f, P.accent, t.c_str(), width - px(44));
        ImGui::SetCursorPosX(px(22));
        if (button("Cancelar", Btn::Ghost, true, px(120))) {
            app.mods->cancel();
            app.auto_join_armed = false;
            app.join_preflight = false;
        }
    }
    if (!m.error.empty() || !m.notice.empty()) {
        ImGui::SetCursorPosX(px(22));
        text(g_regular, 13.5f, m.error.empty() ? P.ok : P.bad, (m.error.empty() ? m.notice : m.error).c_str(), width - px(44));
    }
    if (same_room && m.session_checked && !m.session_error.empty()) {
        ImGui::SetCursorPosX(px(22));
        text(g_regular, 13.5f, P.warn, ("No se pudo comparar: " + m.session_error + ". Puedes intentar unirte igualmente: si los mods no coinciden, el anfitrión te rechazará.").c_str(), width - px(44));
    } else if (same_room && m.session_checked) {
        const mods::SessionDiff& d = m.session_diff;
        int extra = d.extra, missing = d.missing, wrong = d.wrong, disabled = d.disabled;
        std::string summary;
        if (!d.game_ok) summary = d.game_problem;
        else if (d.compatible) summary = "Tus mods coinciden con los del anfitrión (" + std::to_string(m.session_manifest.mods.size()) + "). Puedes unirte y ponerte Listo.";
        else summary = "El anfitrión usa " + std::to_string(m.session_manifest.mods.size()) + " mods. Te faltan " + std::to_string(missing) + ", " +
                       std::to_string(wrong) + " son distintos, " + std::to_string(disabled) + " los tienes desactivados y tienes " + std::to_string(extra) +
                       " activos de más.";
        ImGui::SetCursorPosX(px(22));
        text(g_semibold, 14, d.compatible ? P.ok : P.warn, summary.c_str(), width - px(44));
        for (const auto& it : d.items) {
            if (it.state == mods::ModDiff::ExactMatch || it.state == mods::ModDiff::IncompatibleGame) continue;
            ImGui::SetCursorPosX(px(34));
            std::string line = (it.name.empty() ? it.id : it.name) + (it.wanted_version.empty() ? "" : " " + it.wanted_version) + " — " + diff_text(it.state);
            if (!it.local_version.empty() && it.state != mods::ModDiff::ExtraActive) line += " (tienes " + it.local_version + ")";
            if (!it.source.empty() && it.source != "local") line += " · fuente: " + it.source + (it.source_id.empty() ? "" : " " + it.source_id);
            text(g_regular, 13, P.dim, line.c_str(), width - px(56));
            if (!it.detail.empty()) {
                ImGui::SetCursorPosX(px(46));
                text(g_regular, 12, P.faint, it.detail.c_str(), width - px(68));
            }
            const auto& refs = m.session_manifest.mods;
            auto wanted = std::find_if(refs.begin(), refs.end(), [&](const mods::SessionMod& mod) { return mod.id == it.id; });
            if (wanted != refs.end())
                for (const auto& dependency : wanted->dependencies) {
                    ImGui::SetCursorPosX(px(46));
                    text(g_regular, 12, P.faint, ("Requiere " + dependency.id + " " + dependency.version).c_str(), width - px(68));
                }
        }
        bool any_download = std::any_of(d.items.begin(), d.items.end(), [](const mods::DiffItem& it) { return it.downloadable; });
        ImGui::SetCursorPosX(px(22));
        if (d.game_ok && !d.compatible && d.session_ready) {
            if (button("Usar los mods de la sala", Btn::Primary, !m.busy, px(250))) {
                app.mods->session_use();
                app.auto_join_armed = app.auto_join;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Activa un perfil temporal igual al del anfitrión. Tu perfil habitual vuelve al salir.");
        } else if (d.game_ok && !d.compatible && any_download && !m.sync_planned) {
            if (button("Buscar los que faltan", Btn::Primary, !m.busy, px(230))) app.mods->sync_prepare();
        }
        if (m.sync_planned && !m.sync.empty()) {
            uint64_t total = 0;
            int ok = 0;
            for (const auto& s : m.sync) {
                ImGui::SetCursorPosX(px(34));
                std::string line = s.name + ": " + (s.done ? "instalado" : s.ok ? s.file_name + " · " + human_size(s.size) + " · gamebanana.com/mods/" + std::to_string(s.mod_id) : s.reason);
                text(g_regular, 13, s.done ? P.ok : s.ok ? P.text : P.warn, line.c_str(), width - px(56));
                if (s.ok && !s.done) { total += s.size; ok++; }
            }
            if (ok > 0) {
                ImGui::SetCursorPosX(px(22));
                if (!app.sync_consent) {
                    if (button(("Descargar " + std::to_string(ok) + " (" + human_size(total) + ")").c_str(), Btn::Primary, !m.busy, px(260))) app.sync_consent = true;
                } else {
                    text(g_regular, 13, P.warn, ("¿Descargar " + std::to_string(ok) + " mod(s), " + human_size(total) +
                                                 ", desde gamebanana.com? Se comprueban antes de instalar; tu perfil habitual no cambia.").c_str(), width - px(44));
                    ImGui::SetCursorPosX(px(22));
                    if (button("Sí, descargar", Btn::Primary, !m.busy, px(170))) {
                        app.mods->sync_run();
                        app.sync_consent = false;
                        app.auto_join_armed = app.auto_join;
                    }
                    ImGui::SameLine();
                    if (button("No", Btn::Ghost, true, px(70))) app.sync_consent = false;
                }
            }
        }
    }
    if (m.session_active) {
        ImGui::SetCursorPosX(px(22));
        text(g_regular, 12.5f, P.accent, "Estás usando el perfil temporal de una sala.");
        ImGui::SameLine();
        if (button("Volver a mi perfil", Btn::Ghost, !m.busy, px(190))) app.mods->session_end();
    }
    ImGui::Dummy(ImVec2(0, px(12)));
    ImGui::EndChild();
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
        leave_room(app);
        app.page = Page::Game;
        return;
    }
    ImGui::SetCursorPosX(x0);
    text(g_semibold, 28, P.text, ("Sala · " + g.manifest.display_name).c_str());
    ImGui::SetCursorPosX(x0);
    text(g_regular, 13.5f, P.dim,
         "Un jugador crea la sala y los demás se unen con su dirección (LAN o Hamachi). Todos necesitáis la misma build del juego.", width);
    if (mods::game_support(g.manifest.game_id)) {
        ImGui::SetCursorPosX(x0);
        bool vanilla = app.room_fingerprint.empty() || app.room_fingerprint == mods::kVanilla;
        text(g_regular, 13, vanilla ? P.faint : P.accent,
             vanilla ? "Mods: ninguno. Solo podréis jugar juntos quienes tampoco tengáis mods activos."
                     : ("Mods activos (" + app.room_fingerprint.substr(0, 12) + "). Solo entran quienes tengan exactamente los mismos mods, versiones y orden; "
                        "si no, el anfitrión rechaza la conexión.").c_str(),
             width);
    }
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
        const auto* support = mods::game_support(g.manifest.game_id);
        const bool can_compare = support && support->proven;
        const bool mods_busy = can_compare && app.mods && app.mods_game == app.room_game && app.mods->snapshot().busy;
        if (button("Unirse", Btn::Primary, app.address[0] != 0 && !mods_busy, px(140), "Escribe la dirección del anfitrión.")) {
            if (!can_compare) {
                join_room(app, g, s);
            } else {
                open_room_mods(app, g);
                app.room_error.clear();
                app.join_preflight = true;
                app.mods->session_compare(app.address, static_cast<uint16_t>(app.port));
            }
        }
        ImGui::SameLine();
        if (button("Crear sala", Btn::Secondary, !mods_busy, px(140))) open_room(app, true);
        ImGui::Dummy(ImVec2(0, px(12)));
        ImGui::EndChild();
        room_mods_card(app, g, x0, width);
        std::string msg = !app.room_error.empty() ? app.room_error : app.room ? app.room->status() : "";
        // El rechazo por «otra versión» también llega cuando los mods no coinciden (el token incluye su huella).
        if (!msg.empty() && mods::game_support(g.manifest.game_id) && msg.find("versi") != std::string::npos)
            msg += " Si los dos tenéis el mismo juego, puede que vuestros mods sean distintos: pulsa «Comparar mods».";
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
        if (button("Cancelar", Btn::Secondary, true, px(140))) leave_room(app);
        return;
    }

    RoomSession& room = *app.room;
    bool hosting = mode == RoomMode::Hosting;
    ImGui::SetCursorPosX(x0);
    text(g_regular, 14, P.dim, room.status().c_str(), width);
    if (app.ready_pending || !app.room_error.empty()) {
        ImGui::SetCursorPosX(x0);
        text(g_regular, 13.5f, app.ready_pending ? P.accent : P.warn,
             app.ready_pending ? "Verificando los mods de la partida…" : app.room_error.c_str(), width);
    }
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
            if (button("Empezar", Btn::Primary, room.can_start() && !app.ready_pending, px(150), "Cada jugador necesita su propio puesto y estar listo.")) {
                if (mods::game_support(g.manifest.game_id)) {
                    open_room_mods(app, g);
                    app.ready_pending = true;
                    app.mods->verify_ready(app.room_fingerprint);
                } else room.start(app.start_option);
            }
        } else if (button(me->ready ? "No estoy listo" : "Listo", me->ready ? Btn::Secondary : Btn::Primary,
                          me->seat >= 0 && !app.ready_pending, px(160), "Elige antes un jugador.")) {
            if (!me->ready && mods::game_support(g.manifest.game_id)) {
                open_room_mods(app, g);
                app.ready_pending = true;
                app.mods->verify_ready(app.room_fingerprint);
            } else room.set_ready(!me->ready);
        }
        ImGui::SameLine();
    } else {
        ImGui::SetCursorPosX(x0);
    }
    if (button("Salir de la sala", Btn::Ghost, true, px(170))) leave_room(app);
}

}  // namespace

// ---------------------------------------------------------------- arranque

int main(int argc, char** argv) {
    auto t_start = Clock::now();
    std::string manifests = WL_MANIFEST_DIR;
    std::string screenshot;
    int select = -1;
    bool host_now = false, mods_now = false;
    std::string post_update_journal, update_failed;
    bool auto_update = false, open_settings = false;
    // Modo aplicador: esta copia del exe sustituye los archivos del launcher y no abre ventana.
    if (argc > 1 && !std::strcmp(argv[1], "--apply-update")) {
        upd::ApplyArgs a;
        for (int i = 1; i + 1 < argc; i += 2) {
            std::string k = argv[i], v = argv[i + 1];
            if (k == "--apply-update") a.staging = fs::u8path(v);
            else if (k == "--target") a.target = fs::u8path(v);
            else if (k == "--journal") a.journal = fs::u8path(v);
            else if (k == "--wait-pid") a.wait_pid = std::strtoul(v.c_str(), nullptr, 10);
        }
        if (upd::testing_build())
            if (const char* t = SDL_getenv("WL_UPDATER_HEALTH_MS")) a.health_timeout_ms = std::atoi(t);
        if (a.staging.empty() || a.target.empty() || a.journal.empty()) return 2;
        return upd::run_apply(a);
    }
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--post-update") && i + 1 < argc) post_update_journal = argv[++i];
        else if (!std::strcmp(argv[i], "--update-failed") && i + 1 < argc) update_failed = argv[++i];
        else if (!std::strcmp(argv[i], "--auto-update") && upd::testing_build()) auto_update = true;  // pruebas automáticas
        else if (!std::strcmp(argv[i], "--manifests") && i + 1 < argc) manifests = argv[++i];
        else if (!std::strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshot = argv[++i];
        else if (!std::strcmp(argv[i], "--select") && i + 1 < argc) select = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--host")) host_now = true;
        else if (!std::strcmp(argv[i], "--mods")) mods_now = true;  // abre la página de Mods del juego elegido (pruebas y capturas)
        else if (!std::strcmp(argv[i], "--launcher-settings")) open_settings = true;  // abre Ajustes del launcher (pruebas y capturas)  // abre la sala del juego elegido (pruebas)
    }
    // Junto al exe hay una carpeta manifests/ en la versión instalable; en desarrollo se usa la del repo.
    if (const char* base = SDL_GetBasePath()) {
        g_base = fs::u8path(base);
        fs::path local = fs::u8path(base) / "manifests";
        std::error_code e;
        if (fs::is_directory(local, e)) manifests = local.u8string();
    }
    // Una actualización que se cortó (apagón, cierre forzado): se deshace antes de abrir nada y se reabre la versión restaurada.
    if (post_update_journal.empty() && update_failed.empty()) {
        std::string recovered = upd::recover_interrupted_update();
        if (!recovered.empty() && !g_base.empty()) {
            LaunchSpec again;
            again.executable = g_base / "wii-launcher.exe";
            again.working_dir = g_base;
            again.args = {"--update-failed", "recuperado"};
            if (spawn(again).ok) return 0;
        }
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
    for (auto& g : app.games) end_mod_session(*g);  // perfil temporal de una sala que quedó a medias (cierre inesperado)
    for (auto& g : app.games) probe(*g);  // en segundo plano: la ventana aparece sin esperar al disco

    // Autoactualización: estado, mensajes de una actualización anterior y comprobación en segundo plano.
    app.updates = std::make_unique<UpdateService>(app.prefs, g_base, [] { wake(); });
    {
        std::string from = app.prefs.get("update.from_version");
        if (!update_failed.empty()) {
            static const std::pair<const char*, const char*> reasons[] = {
                {"otra-ventana", "había otra ventana del launcher abierta"}, {"verificacion", "los archivos descargados no pasaron la verificación"},
                {"permisos", "la carpeta del launcher no permite escribir"}, {"espacio", "no hay espacio libre suficiente"},
                {"incompleta", "faltaban archivos de la actualización"}, {"instalacion", "Windows o el antivirus bloquearon un archivo"},
                {"arranque", "la versión nueva no arrancó bien"}, {"recuperado", "se interrumpió a mitad (cierre o apagón)"}};
            std::string why = update_failed;
            for (const auto& [k, t] : reasons)
                if (update_failed == k) why = t;
            app.update_notice = "La actualización no se instaló: " + why + ". Sigues en la versión v" + upd::current_version() + ", que funciona como antes.";
            app.update_notice_bad = true;
            app.page = Page::Launcher;
        } else if (!from.empty() && from != upd::current_version()) {
            app.update_notice = "Actualizado de v" + from + " a v" + upd::current_version() + ".";
            app.prefs.set("update.from_version", "");
            app.page = Page::Launcher;
        }
        if (!update_failed.empty()) app.prefs.set("update.from_version", "");
        if (open_settings) app.page = Page::Launcher;
    }

    if (host_now && app.games[app.selected]->adapter) {
        for (int i = 0; i < 200 && !app.games[app.selected]->probed; ++i) SDL_Delay(5);
        if (app.games[app.selected]->adapter->make_room()) open_room(app, true);
    }
    if (mods_now && mods::game_support(app.games[app.selected]->manifest.game_id)) {
        open_mods(app);
        for (int i = 0; i < 400 && (app.mods->snapshot().busy || !app.mods->snapshot().ready); ++i) SDL_Delay(5);
    }
    SDL_ShowWindow(window);
    if (screenshot.empty() && post_update_journal.empty() && !auto_update) app.updates->start_auto();
    // Recién actualizado: una comprobación ya, para traer los instaladores de esta misma versión (p. ej. el parche de MP9)
    // sin esperar a la siguiente comprobación automática (cada 6 h).
    if (!post_update_journal.empty()) app.updates->check_now();  // hilo aparte: no retrasa la ventana
    if (auto_update) app.updates->check_now();
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
            // Arrastrar un ZIP o una carpeta a la ventana en la página de Mods = importar (con revisión previa).
            if (ev.type == SDL_EVENT_DROP_FILE && app.page == Page::Mods && app.mods && ev.drop.data) app.mods->preview(fs::u8path(ev.drop.data));
            if (ev.type == SDL_EVENT_QUIT || (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && ev.window.windowID == SDL_GetWindowID(window))) quit = true;
            redraw = 2;
            got = SDL_PollEvent(&ev);
        }
        if (busy) {
            SessionStart start;
            if (app.room->poll(start)) {
                Game& g = *app.games[app.room_game];
                auto spec = g.adapter->online(g.root, start);
                if (spec) launch_modded(g, *spec, app.room_fingerprint);
                else app.room_error = "La sala empezó pero este PC no tiene puesto: no se abre el juego.";
                app.page = Page::Game;
            }
            redraw = std::max(redraw, 1);
        }
        app.updates->pump();
        if (g_minimize_launcher.exchange(false)) SDL_MinimizeWindow(window);   // el juego no pudo pasar al frente
        if (app.ready_pending && app.room && app.mods) {
            const auto ms = app.mods->snapshot();
            if (!ms.busy) {
                app.ready_pending = false;
                if (ms.ready_verified) {
                    app.room_error.clear();
                    if (app.room->mode() == RoomMode::Hosting && app.room->can_start()) app.room->start(app.start_option);
                    else if (app.room->mode() == RoomMode::Joined) app.room->set_ready(true);
                } else app.room_error = "Revisa tus mods antes de continuar. " + ms.ready_error;
            }
        }
        if (app.join_preflight && app.mods && app.page == Page::Room && app.room_game >= 0) {
            const auto ms = app.mods->snapshot();
            if (!ms.busy) {
                app.join_preflight = false;
                if (ms.session_address == app.address && ms.session_port == app.port && ms.session_checked &&
                    (ms.session_diff.compatible || !ms.session_error.empty())) {
                    Game& rg = *app.games[app.room_game];
                    InstallStatus rs;
                    {
                        std::lock_guard<std::mutex> lock(rg.mutex);
                        rs = rg.status;
                    }
                    join_room(app, rg, rs);
                } else if (!ms.ready) app.room_error = ms.error;
            }
        }
        // Mods de la sala: unirse solo, si se pidió, cuando la comparación diga que todo coincide (C17/C18).
        if (app.auto_join_armed && app.mods && app.page == Page::Room && app.room_game >= 0 && (!app.room || app.room->mode() == RoomMode::Idle)) {
            ModsSnapshot ms = app.mods->snapshot();
            if (!ms.busy) {
                Game& rg = *app.games[app.room_game];
                if (ms.session_address != app.address || ms.session_port != app.port) {
                    app.auto_join_armed = false;
                } else if (ms.session_checked && ms.session_diff.compatible) {
                    InstallStatus rs;
                    {
                        std::lock_guard<std::mutex> lock(rg.mutex);
                        rs = rg.status;
                    }
                    app.auto_join_armed = false;
                    join_room(app, rg, rs);
                } else {
                    app.auto_join_armed = false;   // no se pudo dejar todo listo: el usuario decide
                }
            }
        }
        // El anfitrión deja de publicar su manifiesto cuando no hay sala.
        if (app.manifest_server && (!app.room || app.room->mode() == RoomMode::Idle)) app.manifest_server.reset();
        {
            UpdateSnapshot us = app.updates->snapshot();
            if (us.state == UpdState::Downloading || us.state == UpdState::Verifying) redraw = std::max(redraw, 1);  // progreso
            if (auto_update && us.state == UpdState::Available) app.updates->download();
            if (auto_update && us.state == UpdState::Ready) {
                try_apply(app);
                if (!app.quit_for_update) auto_update = false;  // pospuesta: no se insiste en cada fotograma
            }
            if (auto_update && (us.state == UpdState::Error || us.state == UpdState::UpToDate)) auto_update = false;
        }
        if (app.quit_for_update) break;  // el aplicador espera a que este proceso termine
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
        } else if (app.page == Page::Launcher) {
            launcher_page(app, width);
        } else if (app.page == Page::Mods && app.mods) {
            mods_page(app, width);
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
        update_toast(app, io.DisplaySize);
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
            // Versión recién instalada: avisa al aplicador de que arranca bien (si no, deshace la actualización).
            if (!post_update_journal.empty()) upd::report_health(fs::u8path(post_update_journal));
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
