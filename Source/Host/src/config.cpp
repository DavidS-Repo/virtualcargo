#include "config.hpp"

#include "json.hpp"
#include "util.hpp"

#include <stdexcept>

namespace clippy {

using nlohmann::json;

namespace {

std::filesystem::path resolve_from_config(
    const std::filesystem::path& config_path,
    const std::string& configured_path) {
    std::filesystem::path value(configured_path);
    if (value.is_relative()) value = config_path.parent_path() / value;
    return std::filesystem::absolute(value).lexically_normal();
}

int bounded_int(const json& document, const char* key, int fallback, int minimum, int maximum) {
    const int value = document.value(key, fallback);
    if (value < minimum || value > maximum) {
        throw std::runtime_error(std::string(key) + " must be between " +
                                 std::to_string(minimum) + " and " + std::to_string(maximum));
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

} // namespace

HostConfig load_config(const std::filesystem::path& path) {
    const auto absolute_path = std::filesystem::absolute(path).lexically_normal();
    const auto document = json::parse(read_text_file(absolute_path));
    if (!document.is_object()) throw std::runtime_error("Host configuration must be a JSON object.");

    HostConfig config;
    config.config_path = absolute_path;
    config.bind_address = document.value("bindAddress", "127.0.0.1");
    if (config.bind_address != "127.0.0.1" && config.bind_address != "::1" && config.bind_address != "localhost") {
        throw std::runtime_error("bindAddress must be loopback (127.0.0.1, ::1, or localhost).");
    }
    config.port = bounded_int(document, "port", 27815, 1024, 65535);
    config.http_threads = bounded_int(document, "httpThreads", 16, 2, 64);
    config.max_queued_requests = bounded_int(document, "maxQueuedRequests", 1024, 16, 65536);
    config.max_backup_files = bounded_int(document, "maxBackupFiles", 10, 1, 1000);
    config.terminal_retention_days = bounded_int(document, "terminalRetentionDays", 30, 1, 3650);
    config.maintenance_prune_batch_rows = bounded_int(document, "maintenancePruneBatchRows", 500, 10, 10000);
    config.maintenance_interval_seconds = bounded_int(document, "maintenanceIntervalSeconds", 300, 30, 86400);
    config.max_request_bytes = static_cast<std::size_t>(
        bounded_int(document, "maxRequestBytes", 2 * 1024 * 1024, 4096, 16 * 1024 * 1024));
    config.max_item_nodes = static_cast<std::size_t>(
        bounded_int(document, "maxItemNodes", 4096, 1, 100000));
    config.max_page_nodes = static_cast<std::size_t>(
        bounded_int(document, "maxPageNodes", 256, 1, 100000));
    config.max_item_depth = static_cast<std::size_t>(
        bounded_int(document, "maxItemDepth", 16, 1, 64));

    config.api_token = bounded_string(document, "apiToken", "", 32, 256);

    config.postgres_host = document.value("postgresHost", "127.0.0.1");
    if (config.postgres_host != "127.0.0.1" && config.postgres_host != "::1" && config.postgres_host != "localhost") {
        throw std::runtime_error("postgresHost must be loopback. Remote PostgreSQL is intentionally disabled by default.");
    }
    config.postgres_port = bounded_int(document, "postgresPort", 5432, 1024, 65535);
    config.postgres_database = bounded_string(document, "postgresDatabase", "clippy_virtual_cargo", 1, 63);
    config.postgres_user = bounded_string(document, "postgresUser", "clippy_virtual_cargo", 1, 63);
    config.postgres_password = bounded_string(document, "postgresPassword", "", 32, 256);
    config.postgres_pool_size = bounded_int(document, "postgresPoolSize", 16, 4, 48);
    config.postgres_connect_timeout_seconds = bounded_int(document, "postgresConnectTimeoutSeconds", 5, 1, 60);
    config.postgres_statement_timeout_ms = bounded_int(document, "postgresStatementTimeoutMs", 10000, 100, 120000);
    config.postgres_lock_timeout_ms = bounded_int(document, "postgresLockTimeoutMs", 3000, 50, 60000);
    config.postgres_idle_transaction_timeout_ms = bounded_int(document, "postgresIdleTransactionTimeoutMs", 15000, 1000, 120000);

    const auto library = document.value("postgresLibraryPath", "");
    if (!library.empty()) config.postgres_library_path = resolve_from_config(absolute_path, library).string();
    const auto bin = document.value("postgresBinDirectory", "");
    if (!bin.empty()) config.postgres_bin_directory = resolve_from_config(absolute_path, bin).string();
    config.backup_directory = resolve_from_config(absolute_path, document.value("backupDirectory", "backups"));
    return config;
}

void create_default_config(const std::filesystem::path& path) {
    const auto absolute_path = std::filesystem::absolute(path).lexically_normal();
    if (std::filesystem::exists(absolute_path)) {
        throw std::runtime_error("Configuration already exists: " + absolute_path.string());
    }

    json document = {
        {"protocolVersion", 1},
        {"bindAddress", "127.0.0.1"},
        {"port", 27815},
        {"apiToken", random_hex(32)},
        {"postgresHost", "127.0.0.1"},
        {"postgresPort", 5432},
        {"postgresDatabase", "clippy_virtual_cargo"},
        {"postgresUser", "clippy_virtual_cargo"},
        {"postgresPassword", random_hex(32)},
        {"postgresLibraryPath", ""},
        {"postgresBinDirectory", ""},
        {"postgresPoolSize", 16},
        {"postgresConnectTimeoutSeconds", 5},
        {"postgresStatementTimeoutMs", 10000},
        {"postgresLockTimeoutMs", 3000},
        {"postgresIdleTransactionTimeoutMs", 15000},
        {"backupDirectory", "backups"},
        {"httpThreads", 16},
        {"maxQueuedRequests", 1024},
        {"maxBackupFiles", 10},
        {"terminalRetentionDays", 30},
        {"maintenancePruneBatchRows", 500},
        {"maintenanceIntervalSeconds", 300},
        {"maxRequestBytes", 2 * 1024 * 1024},
        {"maxItemNodes", 4096},
        {"maxPageNodes", 256},
        {"maxItemDepth", 16},
    };
    write_text_file_atomic(absolute_path, document.dump(2) + "\n");
}

} // namespace clippy
