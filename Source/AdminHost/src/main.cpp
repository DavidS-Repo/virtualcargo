#include "admin_config.hpp"
#include "admin_database.hpp"
#include "embedded_assets.hpp"
#include "httplib.h"
#include "json.hpp"
#include "util.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#endif

namespace {

using nlohmann::json;
constexpr const char* admin_version = "1.0.1";
constexpr const char* cookie_name = "ClippyAdminSession";

struct CliOptions {
    std::filesystem::path config_path = "ClippyAdminHost.json";
    std::filesystem::path bootstrap_file;
};

CliOptions parse_options(int argc, char** argv) {
    CliOptions result;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) result.config_path = argv[++i];
        else if (arg == "--bootstrap-file" && i + 1 < argc) result.bootstrap_file = argv[++i];
        else if (arg == "--help" || arg == "-h") {
            std::cout << "ClippyAdminHost " << admin_version << "\n"
                      << "  --config <file.json>       Select the admin host configuration\n"
                      << "  --bootstrap-file <file>   Read one short-lived local bootstrap token and delete the file\n";
            std::exit(0);
        } else {
            throw std::runtime_error("Unknown or incomplete argument: " + arg);
        }
    }
    if (result.bootstrap_file.empty()) throw std::runtime_error("--bootstrap-file is required.");
    return result;
}

std::string trim(std::string value) {
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' || value.back() == '\t')) value.pop_back();
    std::size_t start = 0;
    while (start < value.size() && (value[start] == ' ' || value[start] == '\t')) ++start;
    return value.substr(start);
}

std::string read_bootstrap_file(const std::filesystem::path& path) {
    const auto full = std::filesystem::absolute(path).lexically_normal();
    auto token = trim(clippy::read_text_file(full));
    std::error_code error;
    std::filesystem::remove(full, error);
    if (error) throw std::runtime_error("Could not remove the bootstrap token file after reading it.");
    if (token.size() < 64 || token.size() > 128) throw std::runtime_error("Bootstrap token has an invalid length.");
    for (const char c : token) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            throw std::runtime_error("Bootstrap token must be hexadecimal.");
        }
    }
    return token;
}

std::string request_cookie(const httplib::Request& request, const std::string& name) {
    const auto raw = request.get_header_value("Cookie");
    std::size_t position = 0;
    while (position < raw.size()) {
        while (position < raw.size() && (raw[position] == ' ' || raw[position] == ';')) ++position;
        const auto equals = raw.find('=', position);
        if (equals == std::string::npos) break;
        const auto end = raw.find(';', equals + 1);
        const auto key = trim(raw.substr(position, equals - position));
        const auto value = raw.substr(equals + 1, (end == std::string::npos ? raw.size() : end) - equals - 1);
        if (key == name) return value;
        if (end == std::string::npos) break;
        position = end + 1;
    }
    return {};
}

void security_headers(httplib::Response& response) {
    response.set_header("Cache-Control", "no-store, max-age=0");
    response.set_header("Pragma", "no-cache");
    response.set_header("X-Content-Type-Options", "nosniff");
    response.set_header("Referrer-Policy", "no-referrer");
    response.set_header("X-Frame-Options", "DENY");
    response.set_header("Cross-Origin-Resource-Policy", "same-origin");
    response.set_header("Cross-Origin-Opener-Policy", "same-origin");
    response.set_header("Permissions-Policy", "camera=(), microphone=(), geolocation=(), payment=(), usb=()");
    response.set_header("Content-Security-Policy",
        "default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' data:; connect-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'; form-action 'self'");
}

json envelope(json data) {
    return {{"ok", true}, {"server_time_ms", clippy::now_unix_ms()}, {"data", std::move(data)}};
}

json failure(const std::string& request_id, const std::string& code, const std::string& message, bool retryable=false) {
    return {{"ok", false}, {"request_id", request_id}, {"server_time_ms", clippy::now_unix_ms()},
            {"error", {{"code", code}, {"message", message}, {"retryable", retryable}}}};
}

void send_json(httplib::Response& response, int status, const json& body) {
    response.status = status;
    security_headers(response);
    response.set_content(body.dump(), "application/json; charset=utf-8");
}

int bounded_query_int(const httplib::Request& request, const char* key, int fallback, int minimum, int maximum) {
    if (!request.has_param(key)) return fallback;
    const auto value = request.get_param_value(key);
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (!end || end != value.c_str() + value.size() || parsed < minimum || parsed > maximum) {
        throw clippy::ApiError(400, "invalid_query", std::string(key) + " is outside the allowed range.");
    }
    return static_cast<int>(parsed);
}

double bounded_query_double(const httplib::Request& request, const char* key, double fallback,
                            double minimum, double maximum) {
    if (!request.has_param(key)) return fallback;
    const auto value = request.get_param_value(key);
    char* end = nullptr;
    const double parsed = std::strtod(value.c_str(), &end);
    if (!end || end != value.c_str() + value.size() || !std::isfinite(parsed) || parsed < minimum || parsed > maximum) {
        throw clippy::ApiError(400, "invalid_query", std::string(key) + " is outside the allowed range.");
    }
    return parsed;
}

std::int64_t bounded_query_i64(const httplib::Request& request, const char* key, std::int64_t fallback) {
    if (!request.has_param(key)) return fallback;
    const auto value = request.get_param_value(key);
    char* end = nullptr;
    const long long parsed = std::strtoll(value.c_str(), &end, 10);
    if (!end || end != value.c_str() + value.size() || parsed < 0) {
        throw clippy::ApiError(400, "invalid_query", std::string(key) + " must be a non-negative integer.");
    }
    return static_cast<std::int64_t>(parsed);
}

std::string bounded_query_string(const httplib::Request& request, const char* key, std::size_t maximum) {
    if (!request.has_param(key)) return {};
    auto value = request.get_param_value(key);
    if (value.size() > maximum || value.find('\0') != std::string::npos) {
        throw clippy::ApiError(400, "invalid_query", std::string(key) + " is too long.");
    }
    return value;
}

json parse_body(const httplib::Request& request, std::size_t maximum) {
    if (request.body.size() > maximum) throw clippy::ApiError(413, "request_too_large", "The request is too large.");
    json body;
    try { body = json::parse(request.body); }
    catch (const json::exception&) { throw clippy::ApiError(400, "invalid_json", "The request body is not valid JSON."); }
    if (!body.is_object()) throw clippy::ApiError(400, "invalid_json", "The request body must be a JSON object.");
    return body;
}

std::string body_string(const json& body, const char* key, std::size_t maximum, bool required=true) {
    if (!body.contains(key)) {
        if (required) throw clippy::ApiError(400, "invalid_request", std::string(key) + " is required.");
        return {};
    }
    if (!body[key].is_string()) throw clippy::ApiError(400, "invalid_request", std::string(key) + " must be a string.");
    auto value = body[key].get<std::string>();
    if ((required && value.empty()) || value.size() > maximum || value.find('\0') != std::string::npos) {
        throw clippy::ApiError(400, "invalid_request", std::string(key) + " has an invalid length.");
    }
    return value;
}

std::int64_t body_revision(const json& body, const char* key="expected_revision") {
    if (!body.contains(key) || !body[key].is_number_integer()) throw clippy::ApiError(400, "invalid_request", std::string(key) + " must be an integer.");
    const auto value = body[key].get<std::int64_t>();
    if (value < 0) throw clippy::ApiError(400, "invalid_request", std::string(key) + " cannot be negative.");
    return value;
}

std::string current_identity() {
    if (const char* value = std::getenv("USERNAME"); value && *value) return std::string(value).substr(0, 256);
    if (const char* value = std::getenv("USER"); value && *value) return std::string(value).substr(0, 256);
    return {};
}

bool open_directory(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto result = reinterpret_cast<std::intptr_t>(ShellExecuteW(nullptr, L"open", path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return result > 32;
#else
    (void)path;
    return false;
#endif
}

bool process_running_named(const std::string& executable) {
#ifdef _WIN32
    std::wstring target(executable.begin(), executable.end());
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, target.c_str()) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
#else
    (void)executable;
    return false;
#endif
}

bool storage_host_reachable(const clippy_admin::AdminConfig& config) {
    try {
        httplib::Client client(config.storage_host_address, config.storage_host_port);
        client.set_connection_timeout(0, 250000);
        client.set_read_timeout(0, 250000);
        auto result = client.Get("/__clippy_admin_probe__");
        return static_cast<bool>(result);
    } catch (...) {
        return false;
    }
}

json storage_host_post(const clippy_admin::AdminConfig& config, const std::string& path,
                       json body, const std::string& request_id) {
    body["api_token"] = config.storage_host_api_token;
    body["request_id"] = request_id;
    httplib::Client client(config.storage_host_address, config.storage_host_port);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(15, 0);
    client.set_write_timeout(5, 0);
    auto response = client.Post(path, body.dump(), "application/json");
    if (!response) throw clippy::ApiError(503, "storage_host_unavailable", "ClippyStorageHost is not reachable.", true);
    json parsed;
    try { parsed = json::parse(response->body); }
    catch (...) { throw clippy::ApiError(502, "storage_host_invalid_response", "ClippyStorageHost returned an invalid response.", true); }
    if (response->status < 200 || response->status >= 300 || parsed.value("ok", false) == false) {
        const auto error = parsed.value("error", json::object());
        throw clippy::ApiError(response->status >= 400 ? response->status : 502,
                               error.value("code", "storage_host_error"),
                               error.value("message", "ClippyStorageHost rejected the admin operation."),
                               error.value("retryable", false));
    }
    return parsed.value("data", json::object());
}



bool manager_settings_available(const clippy_admin::AdminConfig& config) {
    std::error_code error;
    return !config.manager_config_path.empty() && std::filesystem::is_regular_file(config.manager_config_path, error) && !error;
}

json manager_settings_view(const clippy_admin::AdminConfig& config) {
    json result = {{"available", false}};
    if (!manager_settings_available(config)) {
        result["note"] = "ClippyServerManager.json was not supplied to this AdminHost instance.";
        return result;
    }

    std::string raw;
    json document;
    try {
        raw = clippy::read_text_file(config.manager_config_path);
        document = json::parse(raw);
    } catch (const std::exception&) {
        throw clippy::ApiError(500, "manager_config_invalid", "ClippyServerManager.json could not be read as JSON.");
    }
    if (!document.is_object() || document.value("ConfigVersion", 0) != 6) {
        throw clippy::ApiError(409, "manager_config_version", "ClippyServerManager.json is not a supported version 6 configuration.");
    }
    const auto admin = document.value("AdminPanel", json::object());
    if (!admin.is_object()) throw clippy::ApiError(409, "manager_config_invalid", "ClippyServerManager.json AdminPanel must be an object.");

    result = {
        {"available", true},
        {"file_name", config.manager_config_path.filename().string()},
        {"config_fingerprint", clippy::fingerprint(raw)},
        {"admin_panel_enabled", admin.value("Enabled", true)},
        {"port", admin.value("Port", 27817)},
        {"enable_editing", admin.value("EnableEditing", false)},
        {"auto_open_browser", admin.value("AutoOpenBrowser", true)},
        {"idle_shutdown_minutes", admin.value("IdleShutdownMinutes", 30)},
        {"http_threads", admin.value("HttpThreads", 8)},
        {"max_queued_requests", admin.value("MaxQueuedRequests", 256)},
        {"max_request_bytes", admin.value("MaxRequestBytes", 65536)},
        {"postgres_pool_size", admin.value("PostgresPoolSize", 4)},
        {"postgres_connect_timeout_seconds", admin.value("PostgresConnectTimeoutSeconds", 3)},
        {"postgres_statement_timeout_ms", admin.value("PostgresStatementTimeoutMs", 3000)},
        {"postgres_lock_timeout_ms", admin.value("PostgresLockTimeoutMs", 500)},
        {"postgres_idle_transaction_timeout_ms", admin.value("PostgresIdleTransactionTimeoutMs", 5000)},
        {"maintenance_lock_seconds", admin.value("MaintenanceLockSeconds", 300)},
        {"postgres_write_pool_size", admin.value("PostgresWritePoolSize", 2)},
        {"postgres_write_statement_timeout_ms", admin.value("PostgresWriteStatementTimeoutMs", 5000)},
        {"postgres_write_lock_timeout_ms", admin.value("PostgresWriteLockTimeoutMs", 1500)},
        {"enable_player_telemetry", admin.value("EnablePlayerTelemetry", false)},
        {"enable_player_network_telemetry", admin.value("EnablePlayerNetworkTelemetry", true)},
        {"enable_player_position_telemetry", admin.value("EnablePlayerPositionTelemetry", true)},
        {"player_snapshot_interval_seconds", admin.value("PlayerSnapshotIntervalSeconds", 120)},
        {"player_telemetry_retention_days", admin.value("PlayerTelemetryRetentionDays", 30)},
        {"player_snapshot_history_limit", admin.value("PlayerSnapshotHistoryLimit", 250)},
        {"admin_audit_retention_days", admin.value("AdminAuditRetentionDays", 90)},
        {"enable_live_player_control", admin.value("EnableLivePlayerControl", false)},
        {"player_command_poll_interval_seconds", admin.value("PlayerCommandPollIntervalSeconds", 2)},
        {"player_command_expiry_seconds", admin.value("PlayerCommandExpirySeconds", 30)}
    };
    return result;
}

bool request_bool(const json& body, const char* key) {
    if (!body.contains(key) || !body[key].is_boolean()) throw clippy::ApiError(400, "invalid_setting", std::string(key) + " must be true or false.");
    return body[key].get<bool>();
}

int request_setting_int(const json& body, const char* key, int minimum, int maximum) {
    if (!body.contains(key) || !body[key].is_number_integer()) throw clippy::ApiError(400, "invalid_setting", std::string(key) + " must be an integer.");
    const auto value = body[key].get<long long>();
    if (value < minimum || value > maximum) throw clippy::ApiError(400, "invalid_setting", std::string(key) + " is outside the allowed range.");
    return static_cast<int>(value);
}

void replace_text_file(const std::filesystem::path& path, std::string_view contents) {
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    const auto temporary = std::filesystem::path(path.string() + ".tmp-" + clippy::random_hex(8));
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw clippy::ApiError(500, "config_write_failed", "Could not create a temporary manager configuration file.");
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.flush();
        if (!output) {
            std::error_code remove_error;
            std::filesystem::remove(temporary, remove_error);
            throw clippy::ApiError(500, "config_write_failed", "Could not finish writing the manager configuration file.");
        }
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.wstring().c_str(), path.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code remove_error;
        std::filesystem::remove(temporary, remove_error);
        throw clippy::ApiError(500, "config_write_failed", "Windows could not replace ClippyServerManager.json.");
    }
#else
    std::error_code rename_error;
    std::filesystem::rename(temporary, path, rename_error);
    if (rename_error) {
        std::filesystem::remove(temporary);
        throw clippy::ApiError(500, "config_write_failed", "Could not replace ClippyServerManager.json.");
    }
#endif
}

json update_manager_settings(const clippy_admin::AdminConfig& config, const json& body) {
    if (!manager_settings_available(config)) {
        throw clippy::ApiError(409, "manager_config_unavailable", "This AdminHost instance does not have an editable ClippyServerManager.json path.");
    }
    if (!body.contains("settings") || !body["settings"].is_object()) {
        throw clippy::ApiError(400, "invalid_setting", "settings must be a JSON object.");
    }
    if (!body.contains("expected_config_fingerprint") || !body["expected_config_fingerprint"].is_string()) {
        throw clippy::ApiError(400, "config_fingerprint_required", "expected_config_fingerprint is required. Reload Settings and try again.");
    }
    const auto expected_fingerprint = body["expected_config_fingerprint"].get<std::string>();
    if (expected_fingerprint.size() != 64) {
        throw clippy::ApiError(400, "config_fingerprint_invalid", "expected_config_fingerprint is invalid.");
    }
    const auto& requested = body["settings"];

    std::string raw;
    json document;
    try {
        raw = clippy::read_text_file(config.manager_config_path);
        if (!clippy::constant_time_equal(clippy::fingerprint(raw), expected_fingerprint)) {
            throw clippy::ApiError(409, "manager_config_changed", "ClippyServerManager.json changed after this Settings page was loaded. Reload the page before saving.", true);
        }
        document = json::parse(raw);
    } catch (const clippy::ApiError&) { throw; }
    catch (const std::exception&) {
        throw clippy::ApiError(500, "manager_config_invalid", "ClippyServerManager.json could not be read as JSON.");
    }
    if (!document.is_object() || document.value("ConfigVersion", 0) != 6) {
        throw clippy::ApiError(409, "manager_config_version", "ClippyServerManager.json is not a supported version 6 configuration.");
    }
    if (!document.contains("AdminPanel") || !document["AdminPanel"].is_object()) document["AdminPanel"] = json::object();
    auto& admin = document["AdminPanel"];
    json changed = json::array();

    auto set_bool = [&](const char* request_key, const char* document_key) {
        if (!requested.contains(request_key)) return;
        const bool value = request_bool(requested, request_key);
        if (!admin.contains(document_key) || !admin[document_key].is_boolean() || admin[document_key].get<bool>() != value) {
            admin[document_key] = value;
            changed.push_back(request_key);
        }
    };
    auto set_int = [&](const char* request_key, const char* document_key, int minimum, int maximum) {
        if (!requested.contains(request_key)) return;
        const int value = request_setting_int(requested, request_key, minimum, maximum);
        if (!admin.contains(document_key) || !admin[document_key].is_number_integer() || admin[document_key].get<long long>() != value) {
            admin[document_key] = value;
            changed.push_back(request_key);
        }
    };

    set_bool("admin_panel_enabled", "Enabled");
    set_int("port", "Port", 1024, 65535);
    set_bool("enable_editing", "EnableEditing");
    set_bool("auto_open_browser", "AutoOpenBrowser");
    set_int("idle_shutdown_minutes", "IdleShutdownMinutes", 5, 240);
    set_int("http_threads", "HttpThreads", 2, 32);
    set_int("max_queued_requests", "MaxQueuedRequests", 16, 4096);
    set_int("max_request_bytes", "MaxRequestBytes", 4096, 1048576);
    set_int("postgres_pool_size", "PostgresPoolSize", 2, 12);
    set_int("postgres_connect_timeout_seconds", "PostgresConnectTimeoutSeconds", 1, 15);
    set_int("postgres_statement_timeout_ms", "PostgresStatementTimeoutMs", 100, 10000);
    set_int("postgres_lock_timeout_ms", "PostgresLockTimeoutMs", 50, 3000);
    set_int("postgres_idle_transaction_timeout_ms", "PostgresIdleTransactionTimeoutMs", 1000, 30000);
    set_int("maintenance_lock_seconds", "MaintenanceLockSeconds", 30, 900);
    set_int("postgres_write_pool_size", "PostgresWritePoolSize", 1, 4);
    set_int("postgres_write_statement_timeout_ms", "PostgresWriteStatementTimeoutMs", 500, 15000);
    set_int("postgres_write_lock_timeout_ms", "PostgresWriteLockTimeoutMs", 100, 5000);
    set_bool("enable_player_telemetry", "EnablePlayerTelemetry");
    set_bool("enable_player_network_telemetry", "EnablePlayerNetworkTelemetry");
    set_bool("enable_player_position_telemetry", "EnablePlayerPositionTelemetry");
    set_int("player_snapshot_interval_seconds", "PlayerSnapshotIntervalSeconds", 30, 3600);
    set_int("player_telemetry_retention_days", "PlayerTelemetryRetentionDays", 1, 3650);
    set_int("player_snapshot_history_limit", "PlayerSnapshotHistoryLimit", 2, 10000);
    set_int("admin_audit_retention_days", "AdminAuditRetentionDays", 7, 3650);
    set_bool("enable_live_player_control", "EnableLivePlayerControl");
    set_int("player_command_poll_interval_seconds", "PlayerCommandPollIntervalSeconds", 1, 30);
    set_int("player_command_expiry_seconds", "PlayerCommandExpirySeconds", 5, 300);

    const bool telemetry_enabled = admin.value("EnablePlayerTelemetry", false);
    const bool live_enabled = admin.value("EnableLivePlayerControl", false);
    if (live_enabled && !telemetry_enabled) {
        throw clippy::ApiError(400, "invalid_setting", "Live player control requires player telemetry to be enabled.");
    }
    const int admin_port = admin.value("Port", 27817);
    const auto storage = document.value("StorageHostSettings", json::object());
    const auto postgres = document.value("PostgreSQL", json::object());
    const int storage_port = storage.is_object() ? storage.value("Port", 27815) : 27815;
    const int postgres_port = postgres.is_object() ? postgres.value("Port", 27816) : 27816;
    if (admin_port == storage_port || admin_port == postgres_port) {
        throw clippy::ApiError(400, "invalid_setting", "Admin Panel port must be different from the StorageHost and PostgreSQL ports.");
    }

    if (!changed.empty()) {
        std::string current_raw;
        try { current_raw = clippy::read_text_file(config.manager_config_path); }
        catch (const std::exception&) {
            throw clippy::ApiError(500, "manager_config_invalid", "ClippyServerManager.json could not be re-read before saving.");
        }
        if (!clippy::constant_time_equal(clippy::fingerprint(current_raw), expected_fingerprint)) {
            throw clippy::ApiError(409, "manager_config_changed", "ClippyServerManager.json changed while these settings were being saved. Reload Settings and try again.", true);
        }
        std::error_code copy_error;
        const auto backup = std::filesystem::path(config.manager_config_path.string() + ".before-admin-settings.bak");
        std::filesystem::copy_file(config.manager_config_path, backup, std::filesystem::copy_options::overwrite_existing, copy_error);
        if (copy_error) throw clippy::ApiError(500, "config_backup_failed", "Could not create the safety copy of ClippyServerManager.json.");
        replace_text_file(config.manager_config_path, document.dump(2) + "\n");
    }

    auto saved = manager_settings_view(config);
    saved["changed"] = !changed.empty();
    saved["changed_fields"] = std::move(changed);
    saved["restart_required"] = saved["changed"].get<bool>();
    saved["restart_note"] = "Changes are saved to ClippyServerManager.json. Reopen the Admin Panel, and restart DayZ through START-CLIPPY-SERVER.bat for DayZ-side telemetry changes.";
    return saved;
}

json list_backups(const clippy_admin::AdminConfig& config) {
    json rows = json::array();
    std::error_code error;
    if (!std::filesystem::is_directory(config.backup_directory, error)) return {{"rows", rows}};
    for (const auto& entry : std::filesystem::directory_iterator(config.backup_directory, error)) {
        if (error) break;
        if (!entry.is_regular_file()) continue;
        const auto name = entry.path().filename().string();
        if (name.rfind("ClippyVirtualCargo-", 0) != 0 || entry.path().extension() != ".dump") continue;
        std::int64_t created_ms = 0;
        const auto start = std::string("ClippyVirtualCargo-").size();
        const auto end = name.find('-', start);
        if (end != std::string::npos) {
            try { created_ms = std::stoll(name.substr(start, end - start)); } catch (...) {}
        }
        rows.push_back({{"file", name}, {"path", entry.path().string()}, {"bytes", static_cast<std::int64_t>(entry.file_size())},
                        {"created_ms", created_ms}, {"verification", "verified_when_created"}});
    }
    std::sort(rows.begin(), rows.end(), [](const json& a, const json& b) { return a.value("created_ms", 0LL) > b.value("created_ms", 0LL); });
    if (rows.size() > 100) rows.erase(rows.begin() + 100, rows.end());
    return {{"rows", rows}};
}

class SessionStore {
public:
    struct Session { std::string csrf; std::int64_t created_ms; std::int64_t last_ms; };

    SessionStore(std::string bootstrap_token, std::int64_t bootstrap_expires_ms)
        : bootstrap_token_(std::move(bootstrap_token)), bootstrap_expires_ms_(bootstrap_expires_ms) {}

    std::optional<std::pair<std::string,std::string>> bootstrap(const std::string& token) {
        std::lock_guard lock(mutex_);
        const auto now = clippy::now_unix_ms();
        if (bootstrap_used_ || now > bootstrap_expires_ms_ || !clippy::constant_time_equal(token, bootstrap_token_)) return std::nullopt;
        bootstrap_used_ = true;
        bootstrap_token_.assign(bootstrap_token_.size(), '0');
        const auto session_id = clippy::random_hex(32);
        const auto csrf = clippy::random_hex(32);
        sessions_[session_id] = Session{csrf, now, now};
        return std::make_pair(session_id, csrf);
    }

    std::optional<Session> authenticate(const std::string& session_id) {
        std::lock_guard lock(mutex_);
        const auto now = clippy::now_unix_ms();
        auto found = sessions_.find(session_id);
        if (found == sessions_.end()) return std::nullopt;
        if (now - found->second.created_ms > 8LL * 60 * 60 * 1000) {
            sessions_.erase(found);
            return std::nullopt;
        }
        found->second.last_ms = now;
        return found->second;
    }

    bool erase(const std::string& session_id) {
        std::lock_guard lock(mutex_);
        return sessions_.erase(session_id) != 0;
    }

private:
    std::mutex mutex_;
    std::string bootstrap_token_;
    std::int64_t bootstrap_expires_ms_ = 0;
    bool bootstrap_used_ = false;
    std::unordered_map<std::string, Session> sessions_;
};

bool is_api(const httplib::Request& request) { return request.path.rfind("/api/", 0) == 0; }
bool is_state_method(const httplib::Request& request) { return request.method == "POST" || request.method == "PATCH" || request.method == "DELETE"; }

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        const auto config = clippy_admin::load_admin_config(options.config_path);
        const auto bootstrap_token = read_bootstrap_file(options.bootstrap_file);
        const auto bootstrap_expires = clippy::now_unix_ms() + 2 * 60 * 1000;
        SessionStore sessions(bootstrap_token, bootstrap_expires);
        clippy_admin::AdminDatabase database(config);
        const auto windows_identity = current_identity();

        std::atomic<std::int64_t> last_activity{clippy::now_unix_ms()};
        std::atomic<bool> stopping{false};
        httplib::Server server;
        server.set_payload_max_length(config.max_request_bytes);
        server.new_task_queue = [&config] { return new httplib::ThreadPool(config.http_threads, config.max_queued_requests); };

        const std::string expected_host = "127.0.0.1:" + std::to_string(config.port);
        const std::string expected_origin = "http://" + expected_host;

        server.set_pre_routing_handler([&](const httplib::Request& request, httplib::Response& response) {
            if (request.remote_addr != "127.0.0.1" && request.remote_addr != "::1" && request.remote_addr != "::ffff:127.0.0.1") {
                send_json(response, 403, failure(clippy::random_hex(8), "remote_rejected", "Only local requests are allowed."));
                return httplib::Server::HandlerResponse::Handled;
            }
            if (request.get_header_value("Host") != expected_host) {
                send_json(response, 400, failure(clippy::random_hex(8), "host_rejected", "The Host header is not allowed."));
                return httplib::Server::HandlerResponse::Handled;
            }
            const auto fetch_site = request.get_header_value("Sec-Fetch-Site");
            if (!fetch_site.empty() && fetch_site != "same-origin" && fetch_site != "none") {
                send_json(response, 403, failure(clippy::random_hex(8), "fetch_metadata_rejected", "Cross-site requests are not allowed."));
                return httplib::Server::HandlerResponse::Handled;
            }
            if (is_state_method(request)) {
                const auto content_type = request.get_header_value("Content-Type");
                if (content_type.rfind("application/json", 0) != 0) {
                    send_json(response, 415, failure(clippy::random_hex(8), "json_required", "State-changing requests require application/json."));
                    return httplib::Server::HandlerResponse::Handled;
                }
                const auto origin = request.get_header_value("Origin");
                if (origin != expected_origin) {
                    send_json(response, 403, failure(clippy::random_hex(8), "origin_rejected", "The request Origin is not allowed."));
                    return httplib::Server::HandlerResponse::Handled;
                }
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });

        auto authenticated = [&](const httplib::Request& request, httplib::Response& response, bool require_csrf) -> std::optional<SessionStore::Session> {
            const auto session_id = request_cookie(request, cookie_name);
            auto session = sessions.authenticate(session_id);
            if (!session) {
                send_json(response, 401, failure(clippy::random_hex(8), "unauthorized", "A valid local admin session is required."));
                return std::nullopt;
            }
            if (require_csrf && !clippy::constant_time_equal(request.get_header_value("X-Clippy-CSRF"), session->csrf)) {
                send_json(response, 403, failure(clippy::random_hex(8), "csrf_rejected", "The CSRF token is missing or invalid."));
                return std::nullopt;
            }
            last_activity.store(clippy::now_unix_ms(), std::memory_order_relaxed);
            return session;
        };

        auto handle_exception = [&](httplib::Response& response, const std::string& request_id, const std::exception& error) {
            if (const auto* api = dynamic_cast<const clippy::ApiError*>(&error)) {
                send_json(response, api->http_status(), failure(request_id, api->code(), api->what(), api->retryable()));
                return;
            }
            if (const auto* pg = dynamic_cast<const clippy::PgError*>(&error)) {
                std::cerr << "[PG] request " << request_id << " sqlstate=" << pg->sqlstate() << ": " << pg->what() << '\n';
                send_json(response, 503, failure(request_id, "database_error", "PostgreSQL could not complete the admin request.", true));
                return;
            }
            std::cerr << "[ERROR] request " << request_id << ": " << error.what() << '\n';
            send_json(response, 500, failure(request_id, "internal_error", "The admin host could not complete the request.", true));
        };

        auto api_get = [&](const std::string& pattern, auto operation) {
            server.Get(pattern, [&, operation](const httplib::Request& request, httplib::Response& response) {
                if (!authenticated(request, response, false)) return;
                const auto request_id = clippy::random_hex(8);
                try { send_json(response, 200, envelope(operation(request))); }
                catch (const std::exception& error) { handle_exception(response, request_id, error); }
            });
        };

        auto api_write = [&](const std::string& pattern, const std::string& action,
                             const std::string& target_type, auto target_id, auto operation) {
            server.Post(pattern, [&, action, target_type, target_id, operation](const httplib::Request& request, httplib::Response& response) {
                if (!authenticated(request, response, true)) return;
                const auto request_id = clippy::random_hex(8);
                const auto session_id = request_cookie(request, cookie_name);
                std::string target;
                std::string reason;
                try {
                    target = target_id(request);
                    const auto body = parse_body(request, config.max_request_bytes);
                    if (body.contains("reason") && body["reason"].is_string()) reason = body["reason"].get<std::string>().substr(0, 512);
                    send_json(response, 200, envelope(operation(request, body, session_id, request_id)));
                } catch (const std::exception& error) {
                    try { database.record_external_audit(session_id, windows_identity, action, target_type, target.empty() ? "unknown" : target,
                                                         "FAILURE", reason, error.what(), request_id); } catch (...) {}
                    handle_exception(response, request_id, error);
                }
            });
        };

        server.Post("/api/session/bootstrap", [&](const httplib::Request& request, httplib::Response& response) {
            const auto request_id = clippy::random_hex(8);
            try {
                const auto body = parse_body(request, config.max_request_bytes);
                if (!body.contains("token") || !body["token"].is_string()) throw clippy::ApiError(400, "invalid_json", "A bootstrap token is required.");
                auto created = sessions.bootstrap(body["token"].get<std::string>());
                if (!created) throw clippy::ApiError(401, "bootstrap_rejected", "The bootstrap token is invalid, expired, or already used.");
                last_activity.store(clippy::now_unix_ms(), std::memory_order_relaxed);
                response.set_header("Set-Cookie", std::string(cookie_name) + "=" + created->first + "; Path=/; HttpOnly; SameSite=Strict");
                send_json(response, 200, envelope(json{{"csrf", created->second}, {"read_only", !database.editing_enabled()}, {"editing_enabled", database.editing_enabled()}}));
            } catch (const std::exception& error) { handle_exception(response, request_id, error); }
        });

        api_get("/api/session", [&](const httplib::Request& request) {
            const auto session_id = request_cookie(request, cookie_name);
            auto session = sessions.authenticate(session_id);
            if (!session) throw clippy::ApiError(401, "unauthorized", "A valid local admin session is required.");
            return json{{"csrf", session->csrf}, {"read_only", !database.editing_enabled()}, {"editing_enabled", database.editing_enabled()}, {"identity", windows_identity}};
        });

        server.Post("/api/session/logout", [&](const httplib::Request& request, httplib::Response& response) {
            if (!authenticated(request, response, true)) return;
            sessions.erase(request_cookie(request, cookie_name));
            response.set_header("Set-Cookie", std::string(cookie_name) + "=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
            send_json(response, 200, envelope(json{{"logged_out", true}}));
        });

        api_get("/api/health", [&](const httplib::Request&) {
            json pg;
            bool pg_ok = false;
            try { pg = database.health(); pg_ok = true; }
            catch (const std::exception& error) { std::cerr << "[PG] health check failed: " << error.what() << '\n'; pg = {{"error", "unavailable"}}; }
            return json{{"postgres", {{"ok", pg_ok}, {"detail", pg}}}, {"storage_host_reachable", storage_host_reachable(config)},
                        {"dayz_server_running", process_running_named(config.dayz_executable_name)}, {"read_only", !database.editing_enabled()}, {"editing_enabled", database.editing_enabled()}, {"version", admin_version}};
        });

        api_get("/api/overview", [&](const httplib::Request&) {
            auto data = database.overview();
            data["postgres_ok"] = true;
            data["postgres_version"] = database.health().value("postgres_version", "unknown");
            data["storage_host_reachable"] = storage_host_reachable(config);
            data["dayz_server_running"] = process_running_named(config.dayz_executable_name);
            return data;
        });
        api_get("/api/settings", [&](const httplib::Request&) {
            return json{{"listen_address","127.0.0.1"},{"port",config.port},{"idle_shutdown_minutes",config.idle_shutdown_minutes},
                        {"http_threads",config.http_threads},{"max_queued_requests",config.max_queued_requests},
                        {"max_request_bytes",static_cast<std::int64_t>(config.max_request_bytes)},
                        {"storage_host_address",config.storage_host_address},{"storage_host_port",config.storage_host_port},
                        {"postgres_host",config.postgres.postgres_host},{"postgres_port",config.postgres.postgres_port},
                        {"postgres_database",config.postgres.postgres_database},{"postgres_read_role",config.postgres.postgres_user},
                        {"postgres_read_pool_size",config.postgres.postgres_pool_size},
                        {"postgres_connect_timeout_seconds",config.postgres.postgres_connect_timeout_seconds},
                        {"postgres_statement_timeout_ms",config.postgres.postgres_statement_timeout_ms},
                        {"postgres_lock_timeout_ms",config.postgres.postgres_lock_timeout_ms},
                        {"postgres_idle_transaction_timeout_ms",config.postgres.postgres_idle_transaction_timeout_ms},
                        {"postgres_write_pool_size",config.postgres_write.postgres_pool_size},
                        {"postgres_write_statement_timeout_ms",config.postgres_write.postgres_statement_timeout_ms},
                        {"postgres_write_lock_timeout_ms",config.postgres_write.postgres_lock_timeout_ms},
                        {"editing_enabled",database.editing_enabled()},
                        {"maintenance_lock_seconds",config.maintenance_lock_seconds},{"export_directory",config.export_directory.string()},
                        {"player_telemetry_enabled",config.player_telemetry_enabled},
                        {"player_network_telemetry_enabled",config.player_network_telemetry_enabled},
                        {"player_position_telemetry_enabled",config.player_position_telemetry_enabled},
                        {"live_player_control_enabled",config.live_player_control_enabled},
                        {"player_snapshot_interval_seconds",config.player_snapshot_interval_seconds},
                        {"player_command_expiry_seconds",config.player_command_expiry_seconds},
                        {"player_telemetry_retention_days",config.player_telemetry_retention_days},
                        {"player_snapshot_history_limit",config.player_snapshot_history_limit},
                        {"admin_audit_retention_days",config.admin_audit_retention_days},
                        {"player_ip_collection_supported",false},
                        {"player_ip_collection_note","The supported DayZ server script API does not expose player IP addresses to this mod."},
                        {"dayz_executable_name",config.dayz_executable_name},
                        {"manager_settings",manager_settings_view(config)}};
        });
        api_write("/api/settings", "update_manager_settings", "configuration",
                  [](const httplib::Request&) { return std::string("ClippyServerManager.json"); },
                  [&](const httplib::Request&, const json& body, const std::string& session_id, const std::string& request_id) {
            auto result = update_manager_settings(config, body);
            database.record_external_audit(session_id, windows_identity, "update_manager_settings", "configuration", "ClippyServerManager.json",
                                           "SUCCESS", body_string(body,"reason",512,false), "", request_id,
                                           {{"changed_fields",result.value("changed_fields",json::array())}});
            return result;
        });
        api_get("/api/containers", [&](const httplib::Request& request) {
            return database.containers(bounded_query_string(request, "q", 128), bounded_query_string(request, "after", 128),
                                       bounded_query_string(request, "contains", 128), bounded_query_string(request, "status", 32),
                                       bounded_query_i64(request, "min_nodes", 0), bounded_query_int(request, "stale_days", 0, 0, 36500),
                                       bounded_query_int(request, "limit", 50, 1, 100));
        });
        api_get(R"(/api/containers/([0-9A-Za-z._:-]{1,128}))", [&](const httplib::Request& request) {
            return database.container(request.matches[1].str(), request_cookie(request, cookie_name));
        });
        api_get(R"(/api/containers/([0-9A-Za-z._:-]{1,128})/roots)", [&](const httplib::Request& request) {
            return database.roots(request.matches[1].str(), bounded_query_string(request, "after", 128), bounded_query_int(request, "limit", 50, 1, 100));
        });
        api_get(R"(/api/containers/([0-9A-Za-z._:-]{1,128})/roots/([0-9A-Za-z._:-]{1,128})/tree)", [&](const httplib::Request& request) {
            return database.tree(request.matches[1].str(), request.matches[2].str());
        });
        api_get("/api/items/search", [&](const httplib::Request& request) {
            constexpr double search_maximum = 1.0e15;
            return database.search_items(
                bounded_query_string(request, "q", 128), bounded_query_string(request, "after_class", 256),
                bounded_query_string(request, "after_storage", 128), bounded_query_string(request, "after_root", 128),
                bounded_query_string(request, "after_item", 128),
                bounded_query_double(request, "min_quantity", 0.0, 0.0, search_maximum),
                bounded_query_double(request, "max_quantity", search_maximum, 0.0, search_maximum),
                bounded_query_double(request, "min_health", 0.0, 0.0, search_maximum),
                bounded_query_double(request, "max_health", search_maximum, 0.0, search_maximum),
                bounded_query_string(request, "adapter_id", 128),
                bounded_query_string(request, "location_type", 64),
                bounded_query_int(request, "limit", 50, 1, 100));
        });
        api_get("/api/sessions", [&](const httplib::Request& request) {
            return database.sessions(bounded_query_i64(request, "before_ms", 0), bounded_query_string(request, "before_id", 128), bounded_query_int(request, "limit", 75, 1, 100));
        });
        api_get("/api/players", [&](const httplib::Request& request) {
            const auto online_window = static_cast<std::int64_t>(config.player_snapshot_interval_seconds) * 2500;
            return database.players(bounded_query_string(request,"q",128), bounded_query_i64(request,"before_ms",0),
                                    bounded_query_string(request,"before_id",128), bounded_query_int(request,"limit",50,1,100), online_window);
        });
        api_get(R"(/api/players/([0-9A-Za-z._:-]{1,128}))", [&](const httplib::Request& request) {
            const auto online_window = static_cast<std::int64_t>(config.player_snapshot_interval_seconds) * 2500;
            return database.player_detail(request.matches[1].str(), online_window);
        });
        api_get(R"(/api/players/([0-9A-Za-z._:-]{1,128})/snapshots/([0-9A-Za-z._:-]{1,128}))", [&](const httplib::Request& request) {
            return database.player_snapshot_tree(request.matches[1].str(),request.matches[2].str());
        });
        api_get("/api/player-items/search", [&](const httplib::Request& request) {
            return database.search_player_items(bounded_query_string(request,"q",128), bounded_query_string(request,"player_id",128),
                                                bounded_query_string(request,"after_class",256), bounded_query_string(request,"after_player",128),
                                                bounded_query_string(request,"after_item",192), bounded_query_int(request,"limit",50,1,100));
        });
        api_get("/api/player-commands", [&](const httplib::Request& request) {
            return database.player_commands(bounded_query_string(request,"player_id",128), bounded_query_i64(request,"before_ms",0),
                                            bounded_query_string(request,"before_id",128), bounded_query_int(request,"limit",50,1,100));
        });
        api_get("/api/player-quarantine", [&](const httplib::Request& request) {
            return database.player_quarantine(bounded_query_string(request,"player_id",128), bounded_query_i64(request,"before_ms",0),
                                              bounded_query_string(request,"before_id",128), bounded_query_int(request,"limit",50,1,100));
        });
        api_get("/api/recovery", [&](const httplib::Request&) { return database.recovery(); });
        api_get("/api/database/info", [&](const httplib::Request&) { return database.database_info(); });
        api_get("/api/reports", [&](const httplib::Request& request) {
            return database.report(bounded_query_string(request,"kind",64), bounded_query_int(request,"limit",25,1,50));
        });
        api_get(R"(/api/database/table/([A-Za-z0-9_]{1,64}))", [&](const httplib::Request& request) {
            return database.table_preview(request.matches[1].str(), bounded_query_string(request, "after", 32),
                                          bounded_query_int(request, "limit", 50, 1, 100));
        });
        api_get("/api/admin/changes", [&](const httplib::Request& request) {
            return database.changes(bounded_query_string(request, "storage_id", 128), bounded_query_i64(request, "before_ms", 0),
                                    bounded_query_string(request, "before_id", 128), bounded_query_int(request, "limit", 75, 1, 100));
        });
        api_get(R"(/api/admin/changes/([0-9A-Za-z._:-]{1,128}))", [&](const httplib::Request& request) {
            return database.change_detail(request.matches[1].str());
        });
        api_get(R"(/api/items/([0-9A-Za-z._:-]{1,128})/history)", [&](const httplib::Request& request) {
            return database.item_history(request.matches[1].str(), bounded_query_i64(request, "before_ms", 0),
                                         bounded_query_string(request, "before_id", 128), bounded_query_int(request, "limit", 50, 1, 100));
        });
        api_get("/api/audit", [&](const httplib::Request& request) {
            return database.audit(bounded_query_i64(request, "before_ms", 0), bounded_query_i64(request, "before_id", 0),
                                  bounded_query_i64(request, "from_ms", 0), bounded_query_i64(request, "to_ms", 0),
                                  bounded_query_int(request, "limit", 75, 1, 100),
                                  bounded_query_string(request, "admin", 128), bounded_query_string(request, "action", 128),
                                  bounded_query_string(request, "target_type", 64), bounded_query_string(request, "target_id", 128),
                                  bounded_query_string(request, "result", 32));
        });
        api_get("/api/activity", [&](const httplib::Request& request) {
            return database.activity(bounded_query_i64(request, "before_ms", 0), bounded_query_i64(request, "before_id", 0),
                                     bounded_query_i64(request, "from_ms", 0), bounded_query_i64(request, "to_ms", 0),
                                     bounded_query_int(request, "limit", 75, 1, 100),
                                     bounded_query_string(request, "target", 128), bounded_query_string(request, "event", 128),
                                     bounded_query_string(request, "source", 32));
        });
        api_get("/api/quarantine", [&](const httplib::Request& request) {
            return database.quarantine(bounded_query_i64(request, "before_ms", 0), bounded_query_string(request, "before_id", 128), bounded_query_int(request, "limit", 75, 1, 100));
        });
        api_get("/api/snapshots", [&](const httplib::Request& request) {
            return database.snapshots(bounded_query_string(request, "storage_id", 128), bounded_query_i64(request, "before_ms", 0),
                                      bounded_query_string(request, "before_id", 128), bounded_query_int(request, "limit", 75, 1, 100));
        });
        api_get(R"(/api/snapshots/([0-9A-Za-z._:-]{1,128})/compare)", [&](const httplib::Request& request) {
            return database.snapshot_compare(request.matches[1].str(), bounded_query_string(request, "after", 128),
                                             bounded_query_int(request, "limit", 75, 1, 100));
        });
        api_get("/api/backups", [&](const httplib::Request&) { return list_backups(config); });
        api_get("/api/locks", [&](const httplib::Request& request) {
            return database.locks(bounded_query_i64(request, "before_expiry_ms", 0),
                                  bounded_query_string(request, "before_storage_id", 128),
                                  bounded_query_int(request, "limit", 75, 1, 100));
        });

        api_write("/api/bulk/preview", "bulk_preview", "selection",
                  [](const httplib::Request&) { return std::string("bulk"); },
                  [&](const httplib::Request&, const json& body, const std::string& session_id, const std::string&) {
            if (!body.contains("items")) throw clippy::ApiError(400, "invalid_request", "items is required.");
            return database.bulk_preview(body["items"], session_id);
        });
        api_write("/api/bulk/roots", "bulk_root_change", "selection",
                  [](const httplib::Request&) { return std::string("bulk"); },
                  [&](const httplib::Request&, const json& body, const std::string& session_id, const std::string& request_id) {
            if (!body.contains("items")) throw clippy::ApiError(400, "invalid_request", "items is required.");
            const auto action = body_string(body, "action", 32);
            if (action != "quarantine" && action != "remove") {
                throw clippy::ApiError(400, "invalid_bulk_action", "Bulk root action must be quarantine or remove.");
            }
            return database.bulk_remove_roots(body["items"], action == "quarantine", body_string(body,"reason",512),
                                              session_id, windows_identity, request_id);
        });
        api_write("/api/bulk/transfer/preview", "bulk_transfer_preview", "selection",
                  [](const httplib::Request&) { return std::string("bulk-transfer"); },
                  [&](const httplib::Request&, const json& body, const std::string& session_id, const std::string&) {
            if (!body.contains("items")) throw clippy::ApiError(400, "invalid_request", "items is required.");
            return database.bulk_transfer_preview(body["items"], body_string(body,"target_storage_id",128),
                                                   body_revision(body,"target_expected_revision"), session_id);
        });
        api_write("/api/bulk/transfer", "bulk_transfer_roots", "selection",
                  [](const httplib::Request&) { return std::string("bulk-transfer"); },
                  [&](const httplib::Request&, const json& body, const std::string& session_id, const std::string& request_id) {
            if (!body.contains("items")) throw clippy::ApiError(400, "invalid_request", "items is required.");
            const auto action = body_string(body, "action", 16);
            if (action != "move" && action != "copy") throw clippy::ApiError(400, "invalid_bulk_action", "Bulk transfer action must be move or copy.");
            return database.bulk_transfer_roots(body["items"], body_string(body,"target_storage_id",128),
                                                body_revision(body,"target_expected_revision"), action == "copy",
                                                body_string(body,"reason",512), session_id, windows_identity, request_id);
        });

        api_write(R"(/api/items/([0-9A-Za-z._:-]{1,128})/edit)", "edit_item", "item",
                  [](const httplib::Request& request) { return request.matches[1].str(); },
                  [&](const httplib::Request& request, const json& body, const std::string& session_id, const std::string& request_id) {
            const auto item_id = request.matches[1].str();
            if (!body.contains("patch")) throw clippy::ApiError(400, "invalid_request", "patch is required.");
            return database.edit_item(body_string(body,"storage_id",128), body_string(body,"root_item_id",128), item_id,
                                      body_revision(body), body["patch"], body_string(body,"reason",512,false),
                                      session_id, windows_identity, request_id);
        });
        api_write(R"(/api/items/([0-9A-Za-z._:-]{1,128})/remove)", "remove_item", "item",
                  [](const httplib::Request& request) { return request.matches[1].str(); },
                  [&](const httplib::Request& request, const json& body, const std::string& session_id, const std::string& request_id) {
            return database.remove_item(body_string(body,"storage_id",128), body_string(body,"root_item_id",128), request.matches[1].str(),
                                        body_revision(body), false, body_string(body,"reason",512,false), session_id, windows_identity, request_id);
        });
        api_write(R"(/api/items/([0-9A-Za-z._:-]{1,128})/quarantine)", "quarantine_item", "item",
                  [](const httplib::Request& request) { return request.matches[1].str(); },
                  [&](const httplib::Request& request, const json& body, const std::string& session_id, const std::string& request_id) {
            return database.remove_item(body_string(body,"storage_id",128), body_string(body,"root_item_id",128), request.matches[1].str(),
                                        body_revision(body), true, body_string(body,"reason",512,false), session_id, windows_identity, request_id);
        });
        api_write(R"(/api/items/([0-9A-Za-z._:-]{1,128})/(move|copy))", "move_or_copy_item", "item",
                  [](const httplib::Request& request) { return request.matches[1].str(); },
                  [&](const httplib::Request& request, const json& body, const std::string& session_id, const std::string& request_id) {
            return database.copy_or_move_item(body_string(body,"storage_id",128), body_string(body,"root_item_id",128), request.matches[1].str(),
                                              body_revision(body), body_string(body,"target_storage_id",128), body_revision(body,"target_expected_revision"),
                                              request.matches[2].str()=="copy", body_string(body,"reason",512,false), session_id, windows_identity, request_id);
        });
        api_write(R"(/api/containers/([0-9A-Za-z._:-]{1,128})/export)", "export_container", "container",
                  [](const httplib::Request& request) { return request.matches[1].str(); },
                  [&](const httplib::Request& request, const json&, const std::string&, const std::string&) {
            return database.export_container(request.matches[1].str(), config.export_directory);
        });
        api_write("/api/exports/open-folder", "open_export_folder", "export_folder",
                  [](const httplib::Request&) { return std::string("configured"); },
                  [&](const httplib::Request&, const json&, const std::string&, const std::string&) {
            std::error_code error;
            std::filesystem::create_directories(config.export_directory, error);
            if (error || !open_directory(config.export_directory)) {
                throw clippy::ApiError(500, "open_folder_failed", "Windows could not open the admin export folder.");
            }
            return json{{"opened",true}};
        });

        api_write(R"(/api/containers/([0-9A-Za-z._:-]{1,128})/lock)", "acquire_maintenance_lock", "container",
                  [](const httplib::Request& request) { return request.matches[1].str(); },
                  [&](const httplib::Request& request, const json& body, const std::string& session_id, const std::string& request_id) {
            return database.acquire_manual_lock(request.matches[1].str(), body_revision(body), body_string(body,"reason",512),
                                                session_id, windows_identity, request_id);
        });
        api_write(R"(/api/containers/([0-9A-Za-z._:-]{1,128})/unlock)", "release_maintenance_lock", "container",
                  [](const httplib::Request& request) { return request.matches[1].str(); },
                  [&](const httplib::Request& request, const json& body, const std::string& session_id, const std::string& request_id) {
            return database.release_manual_lock(request.matches[1].str(), body_string(body,"reason",512,false),
                                                session_id, windows_identity, request_id);
        });

        api_write(R"(/api/containers/([0-9A-Za-z._:-]{1,128})/snapshot)", "create_snapshot", "container",
                  [](const httplib::Request& request) { return request.matches[1].str(); },
                  [&](const httplib::Request& request, const json& body, const std::string& session_id, const std::string& request_id) {
            return database.create_snapshot(request.matches[1].str(), body_revision(body), body_string(body,"reason",512,false),
                                            session_id, windows_identity, request_id);
        });
        api_write(R"(/api/quarantine/([0-9A-Za-z._:-]{1,128})/restore)", "restore_quarantine", "quarantine",
                  [](const httplib::Request& request) { return request.matches[1].str(); },
                  [&](const httplib::Request& request, const json& body, const std::string& session_id, const std::string& request_id) {
            return database.restore_quarantine(request.matches[1].str(), body_revision(body), body_string(body,"reason",512,false),
                                               session_id, windows_identity, request_id);
        });
        api_write(R"(/api/admin/changes/([0-9A-Za-z._:-]{1,128})/undo)", "undo_change", "change",
                  [](const httplib::Request& request) { return request.matches[1].str(); },
                  [&](const httplib::Request& request, const json& body, const std::string& session_id, const std::string& request_id) {
            return database.undo_change(request.matches[1].str(), body_string(body,"reason",512,false), session_id, windows_identity, request_id);
        });
        api_write(R"(/api/recovery/operations/([0-9A-Za-z._:-]{1,128})/abort)", "abort_operation", "operation",
                  [](const httplib::Request& request) { return request.matches[1].str(); },
                  [&](const httplib::Request& request, const json& body, const std::string& session_id, const std::string& request_id) {
            if (!database.editing_enabled()) throw clippy::ApiError(403,"editing_disabled","Admin editing is disabled in ClippyServerManager.json.");
            const auto reason = body_string(body,"reason",512,false);
            auto result = storage_host_post(config, "/v1/operation/abort", {{"operation_id",request.matches[1].str()},{"reason",reason.empty()?"aborted from Clippy Admin":reason}}, request_id);
            database.record_external_audit(session_id, windows_identity, "abort_operation", "operation", request.matches[1].str(), "SUCCESS", reason, "", request_id, result);
            return result;
        });
        api_write(R"(/api/recovery/sessions/([0-9A-Za-z._:-]{1,128})/abort)", "abort_session", "session",
                  [](const httplib::Request& request) { return request.matches[1].str(); },
                  [&](const httplib::Request& request, const json& body, const std::string& session_id, const std::string& request_id) {
            if (!database.editing_enabled()) throw clippy::ApiError(403,"editing_disabled","Admin editing is disabled in ClippyServerManager.json.");
            const auto reason = body_string(body,"reason",512,false);
            auto result = storage_host_post(config, "/v1/session/abort", {{"session_id",request.matches[1].str()},{"reason",reason.empty()?"aborted from Clippy Admin":reason}}, request_id);
            database.record_external_audit(session_id, windows_identity, "abort_session", "session", request.matches[1].str(), "SUCCESS", reason, "", request_id, result);
            return result;
        });
        api_write("/api/recovery/integrity", "run_integrity_check", "database",
                  [](const httplib::Request&) { return std::string("clippy_virtual_cargo"); },
                  [&](const httplib::Request&, const json& body, const std::string& session_id, const std::string& request_id) {
            const auto reason = body_string(body,"reason",512,false);
            auto result = storage_host_post(config, "/v1/admin/integrity", json::object(), request_id);
            database.record_external_audit(session_id, windows_identity, "run_integrity_check", "database", "clippy_virtual_cargo", "SUCCESS", reason, "", request_id, result);
            return result;
        });
        api_write("/api/backups/open-folder", "open_backup_folder", "backup_folder",
                  [](const httplib::Request&) { return std::string("configured"); },
                  [&](const httplib::Request&, const json&, const std::string&, const std::string&) {
            std::error_code error;
            std::filesystem::create_directories(config.backup_directory, error);
            if (error || !open_directory(config.backup_directory)) {
                throw clippy::ApiError(500, "open_folder_failed", "Windows could not open the configured backup folder.");
            }
            return json{{"opened",true}};
        });
        api_write("/api/backups", "create_backup", "database",
                  [](const httplib::Request&) { return std::string("clippy_virtual_cargo"); },
                  [&](const httplib::Request&, const json& body, const std::string& session_id, const std::string& request_id) {
            const auto reason = body_string(body,"reason",512,false);
            auto result = storage_host_post(config, "/v1/admin/backup", json::object(), request_id);
            database.record_external_audit(session_id, windows_identity, "create_backup", "database", "clippy_virtual_cargo", "SUCCESS", reason, "", request_id, result);
            return result;
        });
        api_write(R"(/api/backups/([A-Za-z0-9._-]{1,128})/verify)", "verify_backup", "backup",
                  [](const httplib::Request& request) { return request.matches[1].str(); },
                  [&](const httplib::Request& request, const json& body, const std::string& session_id, const std::string& request_id) {
            const auto reason = body_string(body,"reason",512,false);
            auto result = storage_host_post(config, "/v1/admin/backup/verify", {{"filename",request.matches[1].str()}}, request_id);
            database.record_external_audit(session_id, windows_identity, "verify_backup", "backup", request.matches[1].str(), "SUCCESS", reason, "", request_id, result);
            return result;
        });

        api_write(R"(/api/players/([0-9A-Za-z._:-]{1,128})/commands)", "enqueue_player_command", "player",
                  [](const httplib::Request& request) { return request.matches[1].str(); },
                  [&](const httplib::Request& request, const json& body, const std::string& session_id, const std::string& request_id) {
            if (!config.player_telemetry_enabled || !config.live_player_control_enabled) {
                throw clippy::ApiError(409,"live_player_control_disabled","Player telemetry and live player control must be enabled in ClippyServerManager.json.");
            }
            if (!body.contains("action") || !body["action"].is_string()) throw clippy::ApiError(400,"invalid_json","action is required.");
            const auto payload = body.contains("payload") ? body["payload"] : json::object();
            return database.enqueue_player_command(request.matches[1].str(), body["action"].get<std::string>(), payload,
                                                   body_string(body,"reason",512,false), config.player_command_expiry_seconds,
                                                   session_id, windows_identity, request_id);
        });

        server.Get("/assets/styles.css", [&](const httplib::Request&, httplib::Response& response) {
            security_headers(response); response.set_content(clippy_admin::assets::styles_css.data(), clippy_admin::assets::styles_css.size(), "text/css; charset=utf-8");
        });
        server.Get("/assets/app.js", [&](const httplib::Request&, httplib::Response& response) {
            security_headers(response); response.set_content(clippy_admin::assets::app_js.data(), clippy_admin::assets::app_js.size(), "application/javascript; charset=utf-8");
        });
        server.Get("/", [&](const httplib::Request&, httplib::Response& response) {
            security_headers(response); response.set_content(clippy_admin::assets::index_html.data(), clippy_admin::assets::index_html.size(), "text/html; charset=utf-8");
        });

        server.set_error_handler([&](const httplib::Request& request, httplib::Response& response) {
            if (!response.body.empty()) return;
            if (is_api(request)) send_json(response, response.status, failure(clippy::random_hex(8), "endpoint_not_found", "The admin API endpoint was not found."));
            else { security_headers(response); response.status = 404; response.set_content("Not found", "text/plain; charset=utf-8"); }
        });

        std::thread idle_thread([&] {
            const auto idle_ms = static_cast<std::int64_t>(config.idle_shutdown_minutes) * 60 * 1000;
            while (!stopping.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                if (clippy::now_unix_ms() - last_activity.load(std::memory_order_relaxed) >= idle_ms) {
                    stopping.store(true, std::memory_order_release);
                    server.stop();
                    break;
                }
            }
        });

        std::cout << "ClippyAdminHost " << admin_version << " listening on 127.0.0.1:" << config.port
                  << (database.editing_enabled() ? " (safe editing enabled)\n" : " (read-only)\n");
        const bool listened = server.listen("127.0.0.1", config.port);
        stopping.store(true, std::memory_order_release);
        if (idle_thread.joinable()) idle_thread.join();
        if (!listened) throw std::runtime_error("Could not bind 127.0.0.1 on the configured admin port.");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FATAL: " << error.what() << '\n';
        return 1;
    }
}
