#pragma once

#include "wl/account/save.h"

#include <optional>

namespace wl::account {

struct SaveLocation {
    std::string owner;
    fs::path work, nand;
    SaveTarget target;
};

struct SaveBaseline {
    std::string revision, sha256;
};

std::optional<SaveLocation> save_location(const fs::path& account_root, const std::string& owner,
                                          const std::string& game, const fs::path& install_root,
                                          const InstallStatus& status);
bool save_location_enabled(const SaveLocation& location);
bool enable_save_location(const SaveLocation& location);
bool launch_with_save_location(LaunchSpec& spec, const SaveLocation& location, const GameIsRunning& running, std::string& error);
bool read_save_baseline(const SaveLocation& location, SaveBaseline& out);
bool write_save_baseline(const SaveLocation& location, const SaveBaseline& data);
std::string load_device_id(const fs::path& account_root);

}  // namespace wl::account
