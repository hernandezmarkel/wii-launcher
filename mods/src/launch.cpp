#include "wl/mods/launch.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include "wl/mods/safe_path.h"

namespace wl::mods {

fs::path extracted_dir(const std::string& game_id, const fs::path& install_root) {
    return install_root / "games" / game_id / "extracted";
}

namespace {

// El motor abre el disco con fopen() (página de códigos ANSI). Si la ruta tiene caracteres fuera de ASCII
// (usuario de Windows con tilde), se pasa su nombre corto 8.3, que siempre es ASCII.
fs::path ascii_path(const fs::path& p) {
    std::string u = to_utf8(p);
    bool ascii = true;
    for (unsigned char c : u) ascii &= c < 0x80;
    if (ascii) return p;
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetShortPathNameW(p.wstring().c_str(), buf, MAX_PATH * 2);
    if (n > 0 && n < MAX_PATH * 2) return fs::path(std::wstring(buf, n));
#endif
    return p;
}

}  // namespace

bool prepare_launch(const std::string& game_id, const fs::path& install_root, LaunchSpec& spec, LaunchMods& info,
                    std::string& error, const std::string& profile_name, bool allow_unproven) {
    info = LaunchMods{};
    const GameModSupport* support = game_support(game_id);
    if (!support) return true;   // juego sin mods: se lanza como siempre
    ModStore store(game_id, default_mods_root(game_id), extracted_dir(game_id, install_root));
    std::error_code ec;
    if (!fs::exists(store.root() / "profiles", ec) && profile_name.empty()) return true;   // nunca usó mods
    if (!store.open(error)) return false;
    info.profile = profile_name.empty() ? store.active_profile() : profile_name;
    Profile profile;
    if (!store.load_profile(info.profile, profile, error)) return false;
    info.epoch = profile.epoch;
    Effective eff = store.compute(profile);
    if (!eff.problems.empty()) {
        error = eff.problems.front();
        return false;
    }
    info.fingerprint = eff.fingerprint;
    info.mods = eff.manifest_mods.size();
    info.conflicts = eff.conflicts;
    if (eff.vanilla()) return true;
    if (!support->proven && !allow_unproven) {
        error = "los mods de este juego todavía no están verificados en el port: usa el perfil sin mods";
        return false;
    }
    fs::path dir;
    if (!store.build_vdisc(eff, dir, error)) return false;
    info.vdisc = dir;
    spec.args.insert(spec.args.begin(), to_utf8(ascii_path(dir)));
    return true;
}

std::string mod_compat_token(const std::string& base_token, const std::string& fingerprint) {
    if (fingerprint.empty() || fingerprint == kVanilla) return base_token;
    return base_token + "+mods:" + fingerprint.substr(0, 32);
}

}  // namespace wl::mods
