#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace clippy {

struct HostConfig {
    std::filesystem::path config_path;
    std::filesystem::path backup_directory;
    std::string bind_address = "127.0.0.1";
    std::string api_token;

    std::string postgres_host = "127.0.0.1";
    int postgres_port = 5432;
    std::string postgres_database = "clippy_virtual_cargo";
    std::string postgres_user = "clippy_virtual_cargo";
    std::string postgres_password;
    std::string postgres_library_path;
    std::string postgres_bin_directory;
    int postgres_pool_size = 16;
    int postgres_connect_timeout_seconds = 5;
    int postgres_statement_timeout_ms = 10000;
    int postgres_lock_timeout_ms = 3000;
    int postgres_idle_transaction_timeout_ms = 15000;

    int port = 27815;
    int http_threads = 16;
    int max_queued_requests = 1024;
    int max_backup_files = 10;
    int terminal_retention_days = 30;
    int player_telemetry_retention_days = 30;
    int player_snapshot_history_limit = 250;
    int admin_audit_retention_days = 90;
    int maintenance_prune_batch_rows = 500;
    int maintenance_interval_seconds = 300;
    std::size_t max_request_bytes = 2 * 1024 * 1024;
    std::size_t max_item_nodes = 4096;
    std::size_t max_page_nodes = 256;
    std::size_t max_item_depth = 16;
};

[[nodiscard]] HostConfig load_config(const std::filesystem::path& path);
void create_default_config(const std::filesystem::path& path);

} // namespace clippy
