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
        bounded_int(document, "maxRequestBytes", 64 * 1024, 4096, 1024 * 1024));

    config.storage_host_address = document.value("storageHostAddress", "127.0.0.1");
    if (config.storage_host_address != "127.0.0.1") {
        throw std::runtime_error("storageHostAddress must be 127.0.0.1.");
    }
    config.storage_host_port = bounded_int(document, "storageHostPort", 27815, 1024, 65535);

    auto& pg = config.postgres;
    pg.config_path = absolute;
    pg.bind_address = "127.0.0.1";
    pg.postgres_host = document.value("postgresHost", "127.0.0.1");
    if (pg.postgres_host != "127.0.0.1") {
        throw std::runtime_error("postgresHost must be 127.0.0.1.");
    }
    pg.postgres_port = bounded_int(document, "postgresPort", 27816, 1024, 65535);
    pg.postgres_database = bounded_string(document, "postgresDatabase", "clippy_virtual_cargo", 1, 63);
    pg.postgres_user = bounded_string(document, "postgresUser", "clippy_virtual_cargo_admin_read", 1, 63);
    pg.postgres_password = bounded_string(document, "postgresPassword", "", 32, 256);
    pg.postgres_pool_size = bounded_int(document, "postgresPoolSize", 4, 2, 12);
    pg.postgres_connect_timeout_seconds = bounded_int(document, "postgresConnectTimeoutSeconds", 3, 1, 15);
    pg.postgres_statement_timeout_ms = bounded_int(document, "postgresStatementTimeoutMs", 3000, 100, 10000);
    pg.postgres_lock_timeout_ms = bounded_int(document, "postgresLockTimeoutMs", 500, 50, 3000);
    pg.postgres_idle_transaction_timeout_ms = bounded_int(document, "postgresIdleTransactionTimeoutMs", 5000, 1000, 30000);

    const auto library = document.value("postgresLibraryPath", "");
    if (!library.empty()) pg.postgres_library_path = resolve_from_config(absolute, library).string();
    const auto bin = document.value("postgresBinDirectory", "");
    if (!bin.empty()) pg.postgres_bin_directory = resolve_from_config(absolute, bin).string();
    return config;
}

} // namespace clippy_admin
