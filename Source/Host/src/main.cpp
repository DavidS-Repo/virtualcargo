#include "config.hpp"
#include "database.hpp"
#include "httplib.h"
#include "json.hpp"
#include "util.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

using nlohmann::json;
constexpr const char* host_version = "0.5.1";
constexpr const char* protocol_version = "1.0";

std::atomic<bool> stopping = false;
httplib::Server* active_server = nullptr;

void stop_server(int) {
    stopping.store(true, std::memory_order_release);
    if (active_server) active_server->stop();
}

json success(const std::string& request_id, json data) {
    return {{"ok", true}, {"protocol", protocol_version}, {"request_id", request_id},
            {"server_time_ms", clippy::now_unix_ms()}, {"data", std::move(data)}};
}

json failure(const std::string& request_id, const std::string& code,
             const std::string& message, bool retryable) {
    return {{"ok", false}, {"protocol", protocol_version}, {"request_id", request_id},
            {"server_time_ms", clippy::now_unix_ms()},
            {"error", {{"code", code}, {"message", message}, {"retryable", retryable}}}};
}

void send_json(httplib::Response& response, int status, const json& body) {
    response.status = status;
    response.set_header("Cache-Control", "no-store");
    response.set_header("X-Content-Type-Options", "nosniff");
    response.set_content(body.dump(), "application/json; charset=utf-8");
}

using Endpoint = std::function<json(const json&)>;

httplib::Server::Handler endpoint(const clippy::HostConfig& config, Endpoint operation) {
    return [&config, operation = std::move(operation)](const httplib::Request& request,
                                                       httplib::Response& response) {
        std::string request_id = clippy::random_hex(8);
        try {
            if (request.body.size() > config.max_request_bytes) {
                throw clippy::ApiError(413, "request_too_large", "The request exceeds maxRequestBytes.");
            }
            auto body = json::parse(request.body);
            if (!body.is_object()) {
                throw clippy::ApiError(400, "invalid_json", "The request body must be a JSON object.");
            }
            if (body.contains("request_id") && body["request_id"].is_string()) {
                request_id = body["request_id"].get<std::string>().substr(0, 128);
                for (char& character : request_id) {
                    const auto byte = static_cast<unsigned char>(character);
                    if (byte < 0x20 || byte == 0x7f) character = '_';
                }
            }
            const auto token = body.value("api_token", "");
            if (!clippy::constant_time_equal(token, config.api_token)) {
                throw clippy::ApiError(401, "unauthorized", "The API token is missing or invalid.");
            }
            body.erase("api_token");
            send_json(response, 200, success(request_id, operation(body)));
        } catch (const clippy::ApiError& error) {
            send_json(response, error.http_status(),
                      failure(request_id, error.code(), error.what(), error.retryable()));
        } catch (const json::exception& error) {
            send_json(response, 400, failure(request_id, "invalid_json", error.what(), false));
        } catch (const clippy::PgError& error) {
            const auto& state = error.sqlstate();
            const bool busy = state == "40001" || state == "40P01" || state == "55P03" ||
                              state == "57014" || state == "53300" || state == "57P03";
            std::cerr << "[PG] request " << request_id << " sqlstate=" << state
                      << ": " << error.what() << '\n';
            if (busy) {
                send_json(response, 503,
                          failure(request_id, "database_busy",
                                  "PostgreSQL is busy with another cargo transaction. Retry the request.", true));
            } else {
                send_json(response, 500,
                          failure(request_id, "database_error",
                                  "PostgreSQL could not complete the storage request.", true));
            }
        } catch (const std::exception& error) {
            std::cerr << "[ERROR] request " << request_id << ": " << error.what() << '\n';
            send_json(response, 500,
                      failure(request_id, "internal_error", "The storage host could not complete the request.", true));
        }
    };
}

struct CliOptions {
    std::filesystem::path config_path = "ClippyStorageHost.json";
    bool initialize = false;
    std::filesystem::path migrate_sqlite;
    std::string source_fingerprint;
};

CliOptions parse_options(int argc, char** argv) {
    CliOptions result;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--init") {
            result.initialize = true;
        } else if (argument == "--config" && index + 1 < argc) {
            result.config_path = argv[++index];
        } else if (argument == "--migrate-sqlite" && index + 1 < argc) {
            result.migrate_sqlite = argv[++index];
        } else if (argument == "--source-fingerprint" && index + 1 < argc) {
            result.source_fingerprint = argv[++index];
        } else if (argument == "--help" || argument == "-h") {
            std::cout << "ClippyStorageHost " << host_version << "\n"
                      << "  --init                         Create a PostgreSQL host configuration\n"
                      << "  --config <file.json>           Select the host configuration\n"
                      << "  --migrate-sqlite <legacy.db>   Import one legacy SQLite database and exit\n"
                      << "  --source-fingerprint <sha256>  Optional expected SHA-256 of the consistent SQLite snapshot\n";
            std::exit(0);
        } else {
            throw std::runtime_error("Unknown or incomplete command-line option: " + argument);
        }
    }
    return result;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        if (options.initialize) {
            clippy::create_default_config(options.config_path);
            std::cout << "Created " << std::filesystem::absolute(options.config_path).string() << '\n';
            return 0;
        }

        const auto config = clippy::load_config(options.config_path);
        clippy::StorageDatabase database(config);
        if (!options.migrate_sqlite.empty()) {
            const auto result = database.migrate_legacy_sqlite(options.migrate_sqlite, options.source_fingerprint);
            std::cout << result.dump(2) << '\n';
            return 0;
        }
        httplib::Server server;
        active_server = &server;
        std::signal(SIGINT, stop_server);
        std::signal(SIGTERM, stop_server);

        server.new_task_queue = [&config] {
            return new httplib::ThreadPool(static_cast<std::size_t>(config.http_threads),
                                           static_cast<std::size_t>(config.http_threads),
                                           static_cast<std::size_t>(config.max_queued_requests));
        };
        server.set_payload_max_length(config.max_request_bytes);
        server.set_logger([](const httplib::Request& request, const httplib::Response& response) {
            if (response.status >= 400) {
                std::string safe_request = "<unparseable>";
                try {
                    auto parsed = json::parse(request.body);
                    if (parsed.is_object() && parsed.contains("api_token")) parsed["api_token"] = "<redacted>";
                    safe_request = parsed.dump();
                } catch (...) {
                }
                std::cerr << "[HTTP] " << request.method << ' ' << request.path << " -> " << response.status
                          << " request=" << safe_request.substr(0, 2048)
                          << " response=" << response.body.substr(0, 1024) << '\n';
            }
        });

        server.Post("/v1/health", endpoint(config, [&database](const json&) {
            auto check = database.health();
            check["service"] = "ClippyStorageHost";
            check["version"] = host_version;
            check["database_healthy"] = check["healthy"];
            return check;
        }));
        server.Post("/v1/capabilities", endpoint(config, [&config](const json&) {
            return json{
                {"protocol_versions", {protocol_version}},
                {"operation_states", {"PREPARED", "QUARANTINED", "COMMITTED", "CLEANED", "ABORTED"}},
                {"operation_cleanup_states", {"NONE", "PENDING", "CLEANED"}},
                {"session_states", {"OPEN", "MATERIALIZED", "COMMITTED", "CLEANED", "ABORTED"}},
                {"migration_states", {"PREPARED", "COMMITTED", "CLEANED"}},
                {"item_tree_schema", 1},
                {"default_adapter", {{"id", "dayz.state-v2"}, {"version", 1}}},
                {"limits", {{"max_request_bytes", config.max_request_bytes},
                            {"max_item_nodes", config.max_item_nodes}, {"max_page_nodes", config.max_page_nodes}, {"max_item_depth", config.max_item_depth}}},
            };
        }));
        server.Post("/v1/storage/resolve", endpoint(config, [&database](const json& body) { return database.resolve_container(body); }));
        server.Post("/v1/storage/snapshot", endpoint(config, [&database](const json& body) { return database.snapshot(body); }));
        server.Post("/v1/item/tree", endpoint(config, [&database](const json& body) { return database.item_tree(body); }));
        server.Post("/v1/operation/deposit/prepare", endpoint(config, [&database](const json& body) { return database.prepare_deposit(body); }));
        server.Post("/v1/operation/withdraw/prepare", endpoint(config, [&database](const json& body) { return database.prepare_withdrawal(body); }));
        server.Post("/v1/operation/mark-quarantined", endpoint(config, [&database](const json& body) { return database.mark_quarantined(body); }));
        server.Post("/v1/operation/deposit/commit", endpoint(config, [&database](const json& body) { return database.commit_deposit(body); }));
        server.Post("/v1/operation/withdraw/commit", endpoint(config, [&database](const json& body) { return database.commit_withdrawal(body); }));
        server.Post("/v1/operation/abort", endpoint(config, [&database](const json& body) { return database.abort_operation(body); }));
        server.Post("/v1/operation/ack-cleaned", endpoint(config, [&database](const json& body) { return database.acknowledge_operation_cleanup(body); }));
        server.Post("/v1/operation/incomplete", endpoint(config, [&database](const json& body) { return database.incomplete_operations(body); }));
        server.Post("/v1/session/open", endpoint(config, [&database](const json& body) { return database.open_session(body); }));
        server.Post("/v1/session/mark-materialized", endpoint(config, [&database](const json& body) { return database.mark_session_materialized(body); }));
        server.Post("/v1/session/commit", endpoint(config, [&database](const json& body) { return database.commit_session(body); }));
        server.Post("/v1/session/ack-cleaned", endpoint(config, [&database](const json& body) { return database.acknowledge_session_cleanup(body); }));
        server.Post("/v1/session/abort", endpoint(config, [&database](const json& body) { return database.abort_session(body); }));
        server.Post("/v1/session/incomplete", endpoint(config, [&database](const json& body) { return database.incomplete_sessions(body); }));
        server.Post("/v1/migration/prepare", endpoint(config, [&database](const json& body) { return database.prepare_migration(body); }));
        server.Post("/v1/migration/commit", endpoint(config, [&database](const json& body) { return database.commit_migration(body); }));
        server.Post("/v1/migration/ack-cleaned", endpoint(config, [&database](const json& body) { return database.acknowledge_migration_cleanup(body); }));
        server.Post("/v1/migration/incomplete", endpoint(config, [&database](const json& body) { return database.incomplete_migrations(body); }));
        server.Post("/v1/migration/observe", endpoint(config, [&database](const json& body) { return database.observe_migration(body); }));
        server.Post("/v1/admin/item-index/status", endpoint(config, [&database](const json&) { return database.item_index_status(); }));
        server.Post("/v1/admin/item-index/rebuild-batch", endpoint(config, [&database](const json& body) { return database.rebuild_item_index_batch(body); }));
        server.Post("/v1/admin/integrity", endpoint(config, [&database](const json&) { return database.quick_check(); }));
        server.Post("/v1/admin/backup", endpoint(config, [&database](const json& body) { return database.backup(body); }));
        server.Post("/v1/admin/checkpoint", endpoint(config, [&database](const json&) {
            database.checkpoint();
            return json{{"checkpointed", false}, {"managed_by", "postgresql"}};
        }));
        server.Post("/v1/admin/shutdown", endpoint(config, [&database, &server](const json&) {
            database.checkpoint();
            std::thread([&server] {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                stopping.store(true, std::memory_order_release);
                server.stop();
            }).detach();
            return json{{"stopping", true}};
        }));
        server.Post("/v1/metrics", endpoint(config, [&database](const json& body) { return database.metrics(body); }));

        server.set_error_handler([](const httplib::Request&, httplib::Response& response) {
            if (!response.body.empty()) return;
            const auto code = response.status == 404 ? "endpoint_not_found" : "http_error";
            send_json(response, response.status, failure(clippy::random_hex(8), code, "The API request was rejected.", false));
        });

        std::thread maintenance([&] {
            while (!stopping.load(std::memory_order_acquire)) {
                const auto deadline = std::chrono::steady_clock::now() +
                                      std::chrono::seconds(config.maintenance_interval_seconds);
                while (!stopping.load(std::memory_order_acquire) &&
                       std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                }
                if (stopping.load(std::memory_order_acquire)) break;
                try {
                    database.optimize();
                } catch (const std::exception& error) {
                    std::cerr << "[WARN] maintenance: " << error.what() << '\n';
                }
            }
        });

        std::cout << "ClippyStorageHost " << host_version << " listening on "
                  << config.bind_address << ':' << config.port << '\n';
        const bool listened = server.listen(config.bind_address, config.port);
        stopping.store(true, std::memory_order_release);
        if (maintenance.joinable()) maintenance.join();
        active_server = nullptr;
        if (!listened) throw std::runtime_error("Could not bind the configured loopback address and port.");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FATAL: " << error.what() << '\n';
        return 1;
    }
}
