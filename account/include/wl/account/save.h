#pragma once

#include "wl/adapter.h"

#include <functional>

namespace wl::account {

inline constexpr size_t kMaxSaveBytes = 32 * 1024 * 1024;

struct SaveTarget {
    fs::path directory;
    std::string game_id, slot_id, compatibility;
};

struct SaveBundle {
    std::string bytes, sha256;
};

// Una unidad de progreso por título de Wii. Deja fuera la NAND compartida y los Miis.
bool game_save_target(const std::string& game_id, const fs::path& root, const InstallStatus& status,
                      SaveTarget& out, std::string& error);
bool game_process_running(const std::string& game_id);

using GameIsRunning = std::function<bool()>;
bool snapshot_save(const SaveTarget& target, const GameIsRunning& running, SaveBundle& out, std::string& error);
bool restore_save(const SaveTarget& target, const GameIsRunning& running, const SaveBundle& incoming, std::string& error);
bool recover_save_restore(const SaveTarget& target, const GameIsRunning& running, std::string& error);

}  // namespace wl::account
