#include "admin_config.hpp"
#include "admin_database.hpp"
#include "embedded_assets.hpp"
#include "httplib.h"
#include "json.hpp"
#include "util.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

namespace {

using nlohmann::json;
constexpr const char* admin_version = "0.5.1-alpha.1";
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
                if (request.get_header_value("Origin") != expected_origin) {
                    send_json(response, 403, failure(clippy::random_hex(8), "origin_rejected", "The request origin is not allowed."));
                    return httplib::Server::HandlerResponse::Handled;
                }
            }
            security_headers(response);
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

        auto api_get = [&](const std::string& pattern, auto operation) {
            server.Get(pattern, [&, operation](const httplib::Request& request, httplib::Response& response) {
                if (!authenticated(request, response, false)) return;
                const auto request_id = clippy::random_hex(8);
                try { send_json(response, 200, envelope(operation(request))); }
                catch (const clippy::ApiError& error) { send_json(response, error.http_status(), failure(request_id, error.code(), error.what(), error.retryable())); }
                catch (const clippy::PgError& error) {
                    std::cerr << "[PG] request " << request_id << " sqlstate=" << error.sqlstate() << ": " << error.what() << '\n';
                    send_json(response, 503, failure(request_id, "database_error", "PostgreSQL could not complete the admin query.", true));
                }
                catch (const std::exception& error) {
                    std::cerr << "[ERROR] request " << request_id << ": " << error.what() << '\n';
                    send_json(response, 500, failure(request_id, "internal_error", "The admin host could not complete the request.", true));
                }
            });
        };

        server.Post("/api/session/bootstrap", [&](const httplib::Request& request, httplib::Response& response) {
            const auto request_id = clippy::random_hex(8);
            try {
                if (request.body.size() > config.max_request_bytes) throw clippy::ApiError(413, "request_too_large", "The request is too large.");
                const auto body = json::parse(request.body);
                if (!body.is_object() || !body.contains("token") || !body["token"].is_string()) throw clippy::ApiError(400, "invalid_json", "A bootstrap token is required.");
                auto created = sessions.bootstrap(body["token"].get<std::string>());
                if (!created) throw clippy::ApiError(401, "bootstrap_rejected", "The bootstrap token is invalid, expired, or already used.");
                last_activity.store(clippy::now_unix_ms(), std::memory_order_relaxed);
                response.set_header("Set-Cookie", std::string(cookie_name) + "=" + created->first + "; Path=/; HttpOnly; SameSite=Strict");
                send_json(response, 200, envelope(json{{"csrf", created->second}, {"read_only", true}}));
            } catch (const clippy::ApiError& error) {
                send_json(response, error.http_status(), failure(request_id, error.code(), error.what(), error.retryable()));
            } catch (const json::exception&) {
                send_json(response, 400, failure(request_id, "invalid_json", "The request body is not valid JSON."));
            }
        });

        api_get("/api/session", [&](const httplib::Request& request) {
            const auto session_id = request_cookie(request, cookie_name);
            auto session = sessions.authenticate(session_id);
            if (!session) throw clippy::ApiError(401, "unauthorized", "A valid local admin session is required.");
            return json{{"csrf", session->csrf}, {"read_only", true}};
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
                        {"read_only", true}, {"version", admin_version}};
        });

        api_get("/api/overview", [&](const httplib::Request&) {
            auto data = database.overview();
            data["postgres_ok"] = true;
            data["postgres_version"] = database.health().value("postgres_version", "unknown");
            data["storage_host_reachable"] = storage_host_reachable(config);
            return data;
        });
        api_get("/api/containers", [&](const httplib::Request& request) {
            return database.containers(bounded_query_string(request, "q", 128), bounded_query_string(request, "after", 128), bounded_query_int(request, "limit", 50, 1, 100));
        });
        api_get(R"(/api/containers/([0-9A-Za-z._:-]{1,128}))", [&](const httplib::Request& request) {
            return database.container(request.matches[1].str());
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
                bounded_query_string(request, "q", 128),
                bounded_query_string(request, "after_class", 256),
                bounded_query_string(request, "after_storage", 128),
                bounded_query_string(request, "after_root", 128),
                bounded_query_string(request, "after_item", 128),
                bounded_query_double(request, "min_quantity", 0.0, 0.0, search_maximum),
                bounded_query_double(request, "max_quantity", search_maximum, 0.0, search_maximum),
                bounded_query_double(request, "min_health", 0.0, 0.0, search_maximum),
                bounded_query_double(request, "max_health", search_maximum, 0.0, search_maximum),
                bounded_query_int(request, "limit", 50, 1, 100));
        });
        api_get("/api/sessions", [&](const httplib::Request& request) {
            return database.sessions(bounded_query_i64(request, "before_ms", 0), bounded_query_string(request, "before_id", 128),
                                     bounded_query_int(request, "limit", 75, 1, 100));
        });
        api_get("/api/recovery", [&](const httplib::Request&) { return database.recovery(); });
        api_get("/api/database/info", [&](const httplib::Request&) { return database.database_info(); });

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

        std::cout << "ClippyAdminHost " << admin_version << " listening on 127.0.0.1:" << config.port << " (read-only)\n";
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
