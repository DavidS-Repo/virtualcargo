#include "admin_config.hpp"

#include "json.hpp"
#include "util.hpp"

#include <stdexcept>

namespace clippy_admin {

using nlohmann::json;

namespace {

int bounded_int(const json& document, const char* key, int fallback, int minimum, int maximum) {
    const int value = document.value(key, fallback);
    if (value < minimum || value > maximum) {
        throw std::runtime_error(std::string(key) + " is outside the allowed range.");
    }
    return value;
}

std::string bounded_string(const json& document, const char* key, const char* fallback,
                           std::size_t minimum, std::size_t maximum) {
    const auto value = document.value(key, std::string(fallback));
    if (value.size() < minimum || value.size() > maximum || value.find('\0') != std::string::npos) {
        throw std::runtime_error(std::string(key) + " has an invalid length or contains a NUL byte.");
    }
    return value;
}

std::filesystem::path resolve_from_config(const std::filesystem::path& config_path,
                                          const std::string& configured_path) {
    std::filesystem::path value(configured_path);
    if (value.is_relative()) value = config_path.parent_path() / value;
    return std::filesystem::absolute(value).lexically_normal();
}

void load_common_postgres(clippy::HostConfig& pg, const json& document,
                          const std::filesystem::path& absolute) {
    pg.config_path = absolute;
    pg.bind_address = "127.0.0.1";
    pg.postgres_host = document.value("postgresHost", "127.0.0.1");
    if (pg.postgres_host != "127.0.0.1") {
        throw std::runtime_error("postgresHost must be 127.0.0.1.");
    }
    pg.postgres_port = bounded_int(document, "postgresPort", 27816, 1024, 65535);
    pg.postgres_database = bounded_string(document, "postgresDatabase", "clippy_virtual_cargo", 1, 63);
    pg.postgres_connect_timeout_seconds = bounded_int(document, "postgresConnectTimeoutSeconds", 3, 1, 15);
    pg.postgres_statement_timeout_ms = bounded_int(document, "postgresStatementTimeoutMs", 3000, 100, 10000);
    pg.postgres_lock_timeout_ms = bounded_int(document, "postgresLockTimeoutMs", 500, 50, 3000);
    pg.postgres_idle_transaction_timeout_ms = bounded_int(document, "postgresIdleTransactionTimeoutMs", 5000, 1000, 30000);

    const auto library = document.value("postgresLibraryPath", "");
    if (!library.empty()) pg.postgres_library_path = resolve_from_config(absolute, library).string();
    const auto bin = document.value("postgresBinDirectory", "");
    if (!bin.empty()) pg.postgres_bin_directory = resolve_from_config(absolute, bin).string();
}

} // namespace

AdminConfig load_admin_config(const std::filesystem::path& path) {
    const auto absolute = std::filesystem::absolute(path).lexically_normal();
    const auto document = json::parse(clippy::read_text_file(absolute));
    if (!document.is_object()) throw std::runtime_error("Admin host configuration must be a JSON object.");

    AdminConfig config;
    config.config_path = absolute;
    config.port = bounded_int(document, "port", 27817, 1024, 65535);
    config.http_threads = bounded_int(document, "httpThreads", 8, 2, 32);
    config.max_queued_requests = bounded_int(document, "maxQueuedRequests", 256, 16, 4096);
    config.idle_shutdown_minutes = bounded_int(document, "idleShutdownMinutes", 30, 5, 240);
    config.max_request_bytes = static_cast<std::size_t>(
        bounded_int(document, "maxRequestBytes", 128 * 1024, 4096, 1024 * 1024));

    config.storage_host_address = document.value("storageHostAddress", "127.0.0.1");
    if (config.storage_host_address != "127.0.0.1") {
        throw std::runtime_error("storageHostAddress must be 127.0.0.1.");
    }
    config.storage_host_port = bounded_int(document, "storageHostPort", 27815, 1024, 65535);
    config.storage_host_api_token = bounded_string(document, "storageHostApiToken", "", 32, 256);
    config.dayz_executable_name = bounded_string(document, "dayzExecutableName", "DayZServer_x64.exe", 1, 260);
    if (config.dayz_executable_name.find('/') != std::string::npos || config.dayz_executable_name.find('\\') != std::string::npos) {
        throw std::runtime_error("dayzExecutableName must be a file name, not a path.");
    }
    const auto backup = bounded_string(document, "backupDirectory", "backups", 1, 2048);
    config.backup_directory = resolve_from_config(absolute, backup);
    const auto exports = bounded_string(document, "exportDirectory", "exports", 1, 2048);
    config.export_directory = resolve_from_config(absolute, exports);
    config.editing_enabled = document.value("enableEditing", false);
    config.player_telemetry_enabled = document.value("enablePlayerTelemetry", false);
    config.player_network_telemetry_enabled = document.value("enablePlayerNetworkTelemetry", true);
    config.player_position_telemetry_enabled = document.value("enablePlayerPositionTelemetry", true);
    config.live_player_control_enabled = document.value("enableLivePlayerControl", false);
    if (config.live_player_control_enabled && !config.player_telemetry_enabled) {
        throw std::runtime_error("enableLivePlayerControl requires enablePlayerTelemetry.");
    }
    config.player_snapshot_interval_seconds = bounded_int(document, "playerSnapshotIntervalSeconds", 120, 30, 3600);
    config.player_command_expiry_seconds = bounded_int(document, "playerCommandExpirySeconds", 30, 5, 300);
    config.player_telemetry_retention_days = bounded_int(document, "playerTelemetryRetentionDays", 30, 1, 3650);
    config.player_snapshot_history_limit = bounded_int(document, "playerSnapshotHistoryLimit", 250, 2, 10000);
    config.admin_audit_retention_days = bounded_int(document, "adminAuditRetentionDays", 90, 7, 3650);
    config.maintenance_lock_seconds = bounded_int(document, "maintenanceLockSeconds", 300, 30, 900);

    load_common_postgres(config.postgres, document, absolute);
    config.postgres.postgres_user = bounded_string(document, "postgresUser", "clippy_virtual_cargo_admin_read", 1, 63);
    config.postgres.postgres_password = bounded_string(document, "postgresPassword", "", 32, 256);
    config.postgres.postgres_pool_size = bounded_int(document, "postgresPoolSize", 4, 2, 12);

    config.postgres_write = config.postgres;
    if (config.editing_enabled) {
        config.postgres_write.postgres_user = bounded_string(document, "postgresWriteUser", "clippy_virtual_cargo_admin_edit", 1, 63);
        config.postgres_write.postgres_password = bounded_string(document, "postgresWritePassword", "", 32, 256);
        config.postgres_write.postgres_pool_size = bounded_int(document, "postgresWritePoolSize", 2, 1, 4);
        config.postgres_write.postgres_statement_timeout_ms = bounded_int(document, "postgresWriteStatementTimeoutMs", 5000, 500, 15000);
        config.postgres_write.postgres_lock_timeout_ms = bounded_int(document, "postgresWriteLockTimeoutMs", 1500, 100, 5000);
    }
    return config;
}

} // namespace clippy_admin
