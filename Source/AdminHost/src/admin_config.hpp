#pragma once

#include "config.hpp"

#include <filesystem>
#include <string>

namespace clippy_admin {

struct AdminConfig {
    std::filesystem::path config_path;
    int port = 27817;
    int http_threads = 8;
    int max_queued_requests = 256;
    int idle_shutdown_minutes = 30;
    std::size_t max_request_bytes = 128 * 1024;
    std::string storage_host_address = "127.0.0.1";
    int storage_host_port = 27815;
    std::string storage_host_api_token;
    std::string dayz_executable_name = "DayZServer_x64.exe";
    std::filesystem::path backup_directory;
    std::filesystem::path export_directory;
    bool editing_enabled = false;
    bool player_telemetry_enabled = false;
    bool player_network_telemetry_enabled = true;
    bool player_position_telemetry_enabled = true;
    bool live_player_control_enabled = false;
    int player_snapshot_interval_seconds = 120;
    int player_command_expiry_seconds = 30;
    int player_telemetry_retention_days = 30;
    int player_snapshot_history_limit = 250;
    int admin_audit_retention_days = 90;
    int maintenance_lock_seconds = 300;
    clippy::HostConfig postgres;
    clippy::HostConfig postgres_write;
};

AdminConfig load_admin_config(const std::filesystem::path& path);

} // namespace clippy_admin
