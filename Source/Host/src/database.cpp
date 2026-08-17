#include "database.hpp"

#include "util.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <regex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace clippy {

using nlohmann::json;

namespace {

constexpr int schema_version = 7;

std::string required_string(const json& value, const char* key, std::size_t maximum = 512) {
    if (!value.contains(key) || !value[key].is_string()) {
        throw ApiError(400, "invalid_request", std::string(key) + " must be a string.");
    }
    const auto result = value[key].get<std::string>();
    if (result.empty() || result.size() > maximum) {
        throw ApiError(400, "invalid_request", std::string(key) + " has an invalid length.");
    }
    return result;
}

std::int64_t required_revision(const json& value) {
    if (!value.contains("expected_revision") || !value["expected_revision"].is_number_integer()) {
        throw ApiError(400, "invalid_request", "expected_revision must be an integer.");
    }
    const auto revision = value["expected_revision"].get<std::int64_t>();
    if (revision < 0) throw ApiError(400, "invalid_request", "expected_revision cannot be negative.");
    return revision;
}

json required_source_keys(const json& value, const char* field,
                          std::size_t maximum_count = 5000) {
    if (!value.contains(field) || !value[field].is_array()) {
        throw ApiError(400, "invalid_request", std::string(field) + " must be an array.");
    }
    if (value[field].size() > maximum_count) {
        throw ApiError(400, "invalid_request", std::string(field) + " contains too many keys.");
    }
    std::vector<std::string> keys;
    keys.reserve(value[field].size());
    std::unordered_set<std::string> unique;
    for (const auto& entry : value[field]) {
        if (!entry.is_string()) {
            throw ApiError(400, "invalid_request", std::string("Every ") + field + " entry must be a string.");
        }
        const auto key = entry.get<std::string>();
        if (key.empty() || key.size() > 512) {
            throw ApiError(400, "invalid_request", std::string(field) + " contains a key with an invalid length.");
        }
        if (!unique.insert(key).second) {
            throw ApiError(400, "invalid_request", std::string(field) + " contains a duplicate key.");
        }
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

json container_row(Statement& statement) {
    return {
        {"storage_id", statement.text(0)},
        {"provider_id", statement.text(1)},
        {"provider_key", statement.text(2)},
        {"display_name", statement.text(3)},
        {"capacity_slots", statement.integer(4)},
        {"revision", statement.integer(5)},
        {"updated_ms", statement.integer(6)},
    };
}

json operation_row(Statement& statement) {
    json result = {
        {"operation_id", statement.text(0)},
        {"kind", statement.text(1)},
        {"status", statement.text(2)},
        {"storage_id", statement.text(3)},
        {"root_item_id", statement.text(4)},
        {"expected_revision", statement.integer(5)},
        {"created_ms", statement.integer(7)},
        {"updated_ms", statement.integer(8)},
        {"cleanup_state", statement.text(11)},
        {"cleanup_source_keys", json::parse(statement.text(14))},
    };
    if (!statement.is_null(6)) result["item"] = json::parse(statement.text(6));
    if (!statement.is_null(9)) result["error"] = statement.text(9);
    if (!statement.is_null(10)) {
        result["result_revision"] = statement.integer(10);
        result["revision"] = statement.integer(10);
    }
    if (!statement.is_null(12)) result["physical_source_key"] = statement.text(12);
    if (!statement.is_null(13)) result["cleanup_action"] = statement.text(13);
    return result;
}

json session_row(Statement& statement) {
    json result = {
        {"session_id", statement.text(0)},
        {"storage_id", statement.text(1)},
        {"provider_key", statement.text(2)},
        {"player_id", statement.text(3)},
        {"status", statement.text(4)},
        {"expected_revision", statement.integer(5)},
        {"cursor", statement.text(6)},
        {"next_cursor", statement.text(7)},
        {"original_root_ids", json::parse(statement.text(8))},
        {"created_ms", statement.integer(9)},
        {"updated_ms", statement.integer(10)},
        {"physical_source_keys", json::parse(statement.text(13))},
        {"cleanup_source_keys", json::parse(statement.text(14))},
    };
    if (!statement.is_null(11)) result["result_revision"] = statement.integer(11);
    if (!statement.is_null(12)) result["error"] = statement.text(12);
    return result;
}

json migration_row(Statement& statement) {
    json result = {
        {"migration_id", statement.text(0)},
        {"storage_id", statement.text(1)},
        {"provider_key", statement.text(2)},
        {"container_class", statement.text(3)},
        {"status", statement.text(4)},
        {"source_fingerprint", statement.text(5)},
        {"expected_revision", statement.integer(6)},
        {"source_roots", json::parse(statement.text(7))},
        {"created_ms", statement.integer(8)},
        {"updated_ms", statement.integer(9)},
    };
    if (!statement.is_null(10)) result["result_revision"] = statement.integer(10);
    if (!statement.is_null(11)) result["error"] = statement.text(11);
    return result;
}

json find_migration(PgPool* database, const std::string& migration_id) {
    Statement query(database,
        "SELECT m.migration_id,m.storage_id,c.provider_key,m.container_class,m.status,m.source_fingerprint,"
        "m.expected_revision,m.source_roots_json::text,m.created_ms,m.updated_ms,m.result_revision,m.error "
        "FROM cargo_migrations m JOIN storage_containers c ON c.storage_id=m.storage_id "
        "WHERE m.migration_id=?1 FOR UPDATE OF c,m");
    query.bind(1, migration_id);
    if (!query.row()) throw ApiError(404, "migration_not_found", "The cargo migration does not exist.");
    return migration_row(query);
}

json find_session(PgPool* database, const std::string& session_id) {
    Statement query(database,
        "SELECT s.session_id,s.storage_id,c.provider_key,s.player_id,s.status,s.expected_revision,s.cursor,s.next_cursor,"
        "s.original_root_ids_json::text,s.created_ms,s.updated_ms,s.result_revision,s.error,s.physical_source_keys_json::text,"
        "COALESCE((SELECT jsonb_agg(source_key ORDER BY source_key) FROM cargo_session_cleanup_roots r "
        "WHERE r.session_id=s.session_id AND r.cleaned=0),'[]'::jsonb)::text "
        "FROM cargo_sessions s JOIN storage_containers c ON c.storage_id=s.storage_id "
        "WHERE s.session_id=?1 FOR UPDATE OF c,s");
    query.bind(1, session_id);
    if (!query.row()) throw ApiError(404, "session_not_found", "The cargo session does not exist.");
    return session_row(query);
}

json find_operation(PgPool* database, const std::string& operation_id) {
    Statement query(database,
        "SELECT o.operation_id,o.kind,o.status,o.storage_id,o.root_item_id,o.expected_revision,o.payload_json::text,"
        "o.created_ms,o.updated_ms,o.error,o.result_revision,o.cleanup_state,o.physical_source_key,"
        "(SELECT cleanup_action FROM operation_cleanup_roots r WHERE r.operation_id=o.operation_id ORDER BY source_key LIMIT 1),"
        "COALESCE((SELECT jsonb_agg(source_key ORDER BY source_key) FROM operation_cleanup_roots r "
        "WHERE r.operation_id=o.operation_id AND r.cleaned=0),'[]'::jsonb)::text "
        "FROM operations o JOIN storage_containers c ON c.storage_id=o.storage_id "
        "WHERE o.operation_id=?1 FOR UPDATE OF c,o");
    query.bind(1, operation_id);
    if (!query.row()) throw ApiError(404, "operation_not_found", "The operation does not exist.");
    return operation_row(query);
}

json find_idempotent(PgPool* database, const std::string& key, const std::string& request_hash) {
    Statement query(database,
        "SELECT o.operation_id,o.kind,o.status,o.storage_id,o.root_item_id,o.expected_revision,o.payload_json::text,"
        "o.created_ms,o.updated_ms,o.error,o.result_revision,o.cleanup_state,o.physical_source_key,"
        "(SELECT cleanup_action FROM operation_cleanup_roots r WHERE r.operation_id=o.operation_id ORDER BY source_key LIMIT 1),"
        "COALESCE((SELECT jsonb_agg(source_key ORDER BY source_key) FROM operation_cleanup_roots r "
        "WHERE r.operation_id=o.operation_id AND r.cleaned=0),'[]'::jsonb)::text,o.request_fingerprint "
        "FROM operations o JOIN storage_containers c ON c.storage_id=o.storage_id "
        "WHERE o.idempotency_key=?1 FOR UPDATE OF c,o");
    query.bind(1, key);
    if (!query.row()) return nullptr;
    if (query.text(15) != request_hash) {
        throw ApiError(409, "idempotency_conflict", "The idempotency key was already used for another request.");
    }
    return operation_row(query);
}

std::int64_t container_revision(PgPool* database, const std::string& storage_id) {
    Statement query(database, "SELECT revision FROM storage_containers WHERE storage_id=?1");
    query.bind(1, storage_id);
    if (!query.row()) throw ApiError(404, "storage_not_found", "The storage container does not exist.");
    return query.integer(0);
}

void assert_revision(PgPool* database, const std::string& storage_id, std::int64_t expected) {
    Statement query(database, "SELECT revision FROM storage_containers WHERE storage_id=?1 FOR UPDATE");
    query.bind(1, storage_id);
    if (!query.row()) throw ApiError(404, "storage_not_found", "The storage container does not exist.");
    const auto current = query.integer(0);
    if (current != expected) {
        throw ApiError(409, "revision_conflict",
                       "Storage changed while the request was in progress. Current revision is " +
                           std::to_string(current) + ".", true);
    }
}

void assert_storage_workflow_available(PgPool* database, const std::string& storage_id,
                                       const std::string& owner_kind = "",
                                       const std::string& owner_id = "") {
    // Serialize workflows only for this storage container. Different storage rows
    // remain fully concurrent on separate PostgreSQL connections.
    Statement storage_lock(database, "SELECT storage_id FROM storage_containers WHERE storage_id=?1 FOR UPDATE");
    storage_lock.bind(1, storage_id);
    if (!storage_lock.row()) throw ApiError(404, "storage_not_found", "The storage container does not exist.");
    Statement active(database,
        "SELECT workflow_kind,workflow_id,status FROM ("
        "SELECT 'direct operation' AS workflow_kind,operation_id AS workflow_id,status "
        "FROM operations WHERE storage_id=?1 AND (status IN ('PREPARED','QUARANTINED') OR cleanup_state='PENDING') "
        "UNION ALL "
        "SELECT 'page session',session_id,status FROM cargo_sessions "
        "WHERE storage_id=?1 AND status IN ('OPEN','MATERIALIZED','COMMITTED') "
        "UNION ALL "
        "SELECT 'cargo migration',migration_id,status FROM cargo_migrations "
        "WHERE storage_id=?1 AND status IN ('PREPARED','COMMITTED')"
        ") ORDER BY workflow_kind,workflow_id");
    active.bind(1, storage_id);
    while (active.row()) {
        const auto kind = active.text(0);
        const auto id = active.text(1);
        if (kind == owner_kind && id == owner_id) continue;
        throw ApiError(409, "storage_busy",
                       "Storage is busy with an active " + kind + " (" + active.text(2) +
                           "). Finish or recover it before starting another storage workflow.",
                       true);
    }
}

json normalize_item(const json& source, std::size_t depth, std::size_t& count,
                    const HostConfig& config, const std::string& supplied_id = "") {
    if (!source.is_object()) throw ApiError(400, "invalid_item", "Every item node must be an object.");
    if (depth > config.max_item_depth) throw ApiError(400, "item_too_deep", "The item tree is too deep.");
    if (++count > config.max_item_nodes) throw ApiError(400, "too_many_items", "The item tree has too many nodes.");

    const auto class_name = required_string(source, "class_name", 256);
    const double quantity = source.value("quantity", 0.0);
    const double health = source.value("health", 1.0);
    if (!std::isfinite(quantity) || quantity < 0.0 || !std::isfinite(health) || health < 0.0) {
        throw ApiError(400, "invalid_item", "Item quantity and health must be finite, non-negative numbers.");
    }

    json result = {
        {"item_id", supplied_id.empty() ? random_hex(16) : supplied_id},
        {"class_name", class_name},
        {"quantity", quantity},
        {"health", health},
        {"location", source.value("location", json::object())},
        {"adapter", source.value("adapter", json{{"id", "dayz.state-v2"}, {"version", 1}})},
        {"state", source.value("state", json::object())},
        {"children", json::array()},
    };
    if (!result["location"].is_object() || !result["adapter"].is_object() || !result["state"].is_object()) {
        throw ApiError(400, "invalid_item", "location, adapter, and state must be objects.");
    }

    if (source.contains("children")) {
        if (!source["children"].is_array()) throw ApiError(400, "invalid_item", "children must be an array.");
        for (const auto& child : source["children"]) {
            result["children"].push_back(normalize_item(child, depth + 1, count, config));
        }
    }
    return result;
}


std::size_t collect_item_ids(const json& node, json& ids) {
    ids.push_back(node.at("item_id"));
    std::size_t count = 1;
    for (const auto& child : node.at("children")) count += collect_item_ids(child, ids);
    return count;
}

class ItemInserter {
public:
    explicit ItemInserter(PgPool* database)
        : insert_(database,
            "INSERT INTO cargo_roots(storage_id,root_item_id,class_name,quantity,health,state_json,tree_json,"
            "item_ids,node_count,created_ms) VALUES(?1,?2,?3,?4,?5,?6::jsonb,?7::jsonb,?8::jsonb,?9,?10)") {}

    std::size_t insert_tree(const std::string& storage_id, const json& item, std::int64_t now) {
        json item_ids = json::array();
        const auto node_count = collect_item_ids(item, item_ids);
        insert_.bind(1, storage_id);
        insert_.bind(2, item.at("item_id").get<std::string>());
        insert_.bind(3, item.at("class_name").get<std::string>());
        insert_.bind(4, item.at("quantity").get<double>());
        insert_.bind(5, item.at("health").get<double>());
        insert_.bind(6, item.at("state").dump());
        insert_.bind(7, item.dump());
        insert_.bind(8, item_ids.dump());
        insert_.bind(9, static_cast<std::int64_t>(node_count));
        insert_.bind(10, now);
        insert_.done();
        insert_.reset();
        return node_count;
    }

private:
    Statement insert_;
};

std::size_t insert_trees_batch(PgPool* database, const std::string& storage_id,
                               const json& items, std::int64_t now) {
    if (!items.is_array()) throw std::runtime_error("Batch cargo insert requires an item array.");
    if (items.empty()) return 0;
    json rows = json::array();
    std::size_t total_nodes = 0;
    for (const auto& item : items) {
        json item_ids = json::array();
        const auto node_count = collect_item_ids(item, item_ids);
        total_nodes += node_count;
        rows.push_back({
            {"root_item_id", item.at("item_id")},
            {"class_name", item.at("class_name")},
            {"quantity", item.at("quantity")},
            {"health", item.at("health")},
            {"state_json", item.at("state")},
            {"tree_json", item},
            {"item_ids", std::move(item_ids)},
            {"node_count", node_count}
        });
    }
    Statement insert(database, R"SQL(
INSERT INTO cargo_roots(storage_id,root_item_id,class_name,quantity,health,state_json,tree_json,item_ids,node_count,created_ms)
SELECT ?1,x.root_item_id,x.class_name,x.quantity,x.health,x.state_json,x.tree_json,x.item_ids,x.node_count,?3
FROM jsonb_to_recordset(?2::jsonb) AS x(
    root_item_id text,class_name text,quantity double precision,health double precision,
    state_json jsonb,tree_json jsonb,item_ids jsonb,node_count integer
)
)SQL");
    insert.bind(1, storage_id);
    insert.bind(2, rows.dump());
    insert.bind(3, now);
    insert.done();
    if (insert.affected_rows() != static_cast<std::int64_t>(items.size())) {
        throw std::runtime_error("PostgreSQL did not insert every cargo root in the batch.");
    }
    return total_nodes;
}

void lock_idempotency_key(PgPool* database, const std::string& key) {
    // Transaction-scoped advisory lock. Equal replay keys serialize without
    // blocking unrelated players or unrelated storage containers.
    Statement lock(database, "SELECT pg_advisory_xact_lock(hashtextextended(?1,0))");
    lock.bind(1, key);
    if (!lock.row()) throw std::runtime_error("Could not acquire PostgreSQL idempotency lock.");
}

json load_tree(PgPool* database, const std::string& storage_id, const std::string& item_id) {
    Statement root(database,
        "SELECT tree_json::text FROM cargo_roots WHERE storage_id=?1 AND root_item_id=?2");
    root.bind(1, storage_id);
    root.bind(2, item_id);
    if (root.row()) return json::parse(root.text(0));

    // Compatibility path for callers that pass a child item id. Normal hot-path
    // requests use root ids and hit the primary key above.
    Statement child(database,
        "SELECT tree_json::text FROM cargo_roots WHERE storage_id=?1 AND item_ids ? ?2 LIMIT 1");
    child.bind(1, storage_id);
    child.bind(2, item_id);
    if (!child.row()) throw ApiError(404, "item_not_found", "The virtual item does not exist.");
    return json::parse(child.text(0));
}

void audit(PgPool* database, const std::string& operation_id, const std::string& event,
           const json& detail = json::object()) {
    Statement insert(database,
        "INSERT INTO audit_events(operation_id,event_type,detail_json,created_ms) VALUES(?1,?2,?3,?4)");
    insert.bind(1, operation_id);
    insert.bind(2, event);
    insert.bind(3, detail.dump());
    insert.bind(4, now_unix_ms());
    insert.done();
}

std::size_t prune_generated_backups(const std::filesystem::path& directory,
                                    std::size_t maximum_files,
                                    const std::filesystem::path& protected_file) {
    static const std::regex generated_name(
        R"(^ClippyVirtualCargo-[0-9]+-[0-9a-f]{8}\.dump$)",
        std::regex::ECMAScript | std::regex::optimize);
    std::vector<std::filesystem::path> backups;
    std::error_code iteration_error;
    for (std::filesystem::directory_iterator iterator(directory, iteration_error), end;
         !iteration_error && iterator != end; iterator.increment(iteration_error)) {
        std::error_code type_error;
        if (!iterator->is_regular_file(type_error) || type_error) continue;
        const auto name = iterator->path().filename().string();
        if (std::regex_match(name, generated_name)) backups.push_back(iterator->path());
    }
    if (iteration_error) {
        throw std::runtime_error("Could not enumerate the PostgreSQL backup directory: " +
                                 iteration_error.message());
    }
    std::sort(backups.begin(), backups.end(), [](const auto& left, const auto& right) {
        return left.filename().string() < right.filename().string();
    });
    std::size_t removed = 0;
    while (backups.size() > maximum_files) {
        auto expired = backups.begin();
        while (expired != backups.end() && *expired == protected_file) ++expired;
        if (expired == backups.end()) break;
        std::error_code remove_error;
        if (!std::filesystem::remove(*expired, remove_error) || remove_error) {
            throw std::runtime_error("Could not remove expired PostgreSQL backup " +
                                     expired->string() + ": " + remove_error.message());
        }
        backups.erase(expired);
        ++removed;
    }
    return removed;
}

#ifdef _WIN32
std::wstring utf8_to_wide(std::string_view value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                            static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) throw std::runtime_error("Could not convert UTF-8 text for a PostgreSQL tool.");
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                            result.data(), length) != length) {
        throw std::runtime_error("Could not convert UTF-8 text for a PostgreSQL tool.");
    }
    return result;
}

std::wstring quote_windows_argument(const std::wstring& value) {
    if (value.empty()) return L"\"\"";
    if (value.find_first_of(L" \t\n\v\"") == std::wstring::npos) return value;
    std::wstring out = L"\"";
    std::size_t slashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') {
            ++slashes;
            continue;
        }
        if (ch == L'\"') {
            out.append(slashes * 2 + 1, L'\\');
            out.push_back(L'\"');
            slashes = 0;
            continue;
        }
        out.append(slashes, L'\\');
        slashes = 0;
        out.push_back(ch);
    }
    out.append(slashes * 2, L'\\');
    out.push_back(L'\"');
    return out;
}

DWORD run_postgres_tool(const std::filesystem::path& executable,
                        const std::vector<std::wstring>& arguments,
                        const std::string& password) {
    if (!std::filesystem::is_regular_file(executable)) {
        throw std::runtime_error("PostgreSQL tool is missing: " + executable.string());
    }
    std::wstring command = quote_windows_argument(executable.wstring());
    for (const auto& argument : arguments) {
        command.push_back(L' ');
        command += quote_windows_argument(argument);
    }

    const auto password_wide = utf8_to_wide(password);
    DWORD old_length = GetEnvironmentVariableW(L"PGPASSWORD", nullptr, 0);
    std::wstring old_value;
    const bool had_old = old_length != 0;
    if (had_old) {
        old_value.resize(old_length);
        const DWORD copied = GetEnvironmentVariableW(L"PGPASSWORD", old_value.data(), old_length);
        if (copied != 0 && copied < old_length) old_value.resize(copied);
    }
    if (!SetEnvironmentVariableW(L"PGPASSWORD", password_wide.c_str())) {
        throw std::runtime_error("Could not prepare PostgreSQL backup authentication.");
    }
    auto restore_environment = [&] {
        SetEnvironmentVariableW(L"PGPASSWORD", had_old ? old_value.c_str() : nullptr);
    };

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    const BOOL created = CreateProcessW(executable.c_str(), mutable_command.data(), nullptr, nullptr, FALSE,
                                        CREATE_NO_WINDOW, nullptr, executable.parent_path().c_str(),
                                        &startup, &process);
    restore_environment();
    if (!created) {
        throw std::runtime_error("Could not launch PostgreSQL tool " + executable.string() +
                                 " (Windows error " + std::to_string(GetLastError()) + ").");
    }
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 10 * 60 * 1000);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 124);
        CloseHandle(process.hProcess);
        throw std::runtime_error("PostgreSQL backup tool timed out.");
    }
    DWORD exit_code = 1;
    if (!GetExitCodeProcess(process.hProcess, &exit_code)) exit_code = 1;
    CloseHandle(process.hProcess);
    return exit_code;
}
#endif

} // namespace

StorageDatabase::StorageDatabase(HostConfig config) : config_(std::move(config)) {
    std::filesystem::create_directories(config_.backup_directory);
    pool_ = std::make_unique<PgPool>(config_);
    writer_ = pool_.get();
    writer_gate_.attach(*pool_);
    const int reader_slots = (std::max)(4, config_.postgres_pool_size);
    for (int index = 0; index < reader_slots; ++index) {
        readers_.push_back(std::make_unique<ReadSlot>(*pool_));
    }

    {
        std::lock_guard lock(writer_gate_);
        initialize_schema();
    }
    prune_terminal_history();
}

StorageDatabase::~StorageDatabase() = default;

StorageDatabase::ReadSlot& StorageDatabase::reader() {
    return *readers_[next_reader_.fetch_add(1, std::memory_order_relaxed) % readers_.size()];
}

void StorageDatabase::initialize_schema() {
    execute(writer_, "CREATE SCHEMA IF NOT EXISTS clippy AUTHORIZATION CURRENT_USER");
    execute(writer_, "SET search_path=clippy,pg_catalog");
    execute(writer_, R"SQL(
CREATE TABLE IF NOT EXISTS schema_migrations(
    version INTEGER PRIMARY KEY,
    applied_ms BIGINT NOT NULL
);
CREATE TABLE IF NOT EXISTS application_meta(
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
)SQL");

    int current_version = 0;
    {
        Statement version(writer_, "SELECT COALESCE(max(version),0) FROM schema_migrations");
        if (!version.row()) throw std::runtime_error("PostgreSQL schema metadata query returned no row.");
        current_version = static_cast<int>(version.integer(0));
    }
    if (current_version > schema_version) {
        throw std::runtime_error("The PostgreSQL schema is newer than this storage host supports.");
    }

    Transaction transaction(writer_);
    execute(writer_, R"SQL(
CREATE TABLE IF NOT EXISTS storage_containers(
    storage_id TEXT PRIMARY KEY,
    provider_id TEXT NOT NULL,
    provider_key TEXT NOT NULL,
    display_name TEXT NOT NULL,
    capacity_slots BIGINT NOT NULL CHECK(capacity_slots >= 0),
    revision BIGINT NOT NULL DEFAULT 0 CHECK(revision >= 0),
    created_ms BIGINT NOT NULL,
    updated_ms BIGINT NOT NULL,
    UNIQUE(provider_id, provider_key)
) WITH (fillfactor=80, autovacuum_vacuum_scale_factor=0.05, autovacuum_analyze_scale_factor=0.02);

CREATE TABLE IF NOT EXISTS cargo_roots(
    storage_id TEXT NOT NULL REFERENCES storage_containers(storage_id) ON DELETE CASCADE,
    root_item_id TEXT NOT NULL,
    class_name TEXT NOT NULL,
    quantity DOUBLE PRECISION NOT NULL CHECK(quantity >= 0 AND quantity <> 'NaN'::float8 AND quantity <> 'Infinity'::float8),
    health DOUBLE PRECISION NOT NULL CHECK(health >= 0 AND health <> 'NaN'::float8 AND health <> 'Infinity'::float8),
    state_json JSONB NOT NULL,
    tree_json JSONB NOT NULL,
    item_ids JSONB NOT NULL CHECK(jsonb_typeof(item_ids)='array'),
    node_count INTEGER NOT NULL CHECK(node_count > 0),
    created_ms BIGINT NOT NULL,
    PRIMARY KEY(storage_id,root_item_id)
) WITH (autovacuum_vacuum_scale_factor=0.05, autovacuum_analyze_scale_factor=0.02, toast.autovacuum_vacuum_scale_factor=0.05);
CREATE INDEX IF NOT EXISTS cargo_roots_item_ids
    ON cargo_roots USING GIN(item_ids);

CREATE TABLE IF NOT EXISTS operations(
    operation_id TEXT PRIMARY KEY,
    idempotency_key TEXT NOT NULL UNIQUE,
    request_fingerprint TEXT NOT NULL,
    kind TEXT NOT NULL CHECK(kind IN ('deposit','withdraw')),
    status TEXT NOT NULL CHECK(status IN ('PREPARED','QUARANTINED','COMMITTED','CLEANED','ABORTED')),
    cleanup_state TEXT NOT NULL DEFAULT 'NONE' CHECK(cleanup_state IN ('NONE','PENDING','CLEANED')),
    physical_source_key TEXT,
    storage_id TEXT NOT NULL REFERENCES storage_containers(storage_id),
    root_item_id TEXT NOT NULL,
    expected_revision BIGINT NOT NULL,
    payload_json JSONB,
    created_ms BIGINT NOT NULL,
    updated_ms BIGINT NOT NULL,
    error TEXT,
    result_revision BIGINT
) WITH (fillfactor=80, autovacuum_vacuum_scale_factor=0.05, autovacuum_analyze_scale_factor=0.02);
CREATE INDEX IF NOT EXISTS operations_incomplete ON operations(created_ms)
    WHERE status IN ('PREPARED','QUARANTINED') OR cleanup_state='PENDING';
CREATE INDEX IF NOT EXISTS operations_active_storage ON operations(storage_id)
    WHERE status IN ('PREPARED','QUARANTINED') OR cleanup_state='PENDING';
CREATE INDEX IF NOT EXISTS operations_terminal_retention ON operations(updated_ms,operation_id)
    WHERE cleanup_state<>'PENDING' AND status IN ('CLEANED','ABORTED');

CREATE TABLE IF NOT EXISTS operation_cleanup_roots(
    operation_id TEXT NOT NULL REFERENCES operations(operation_id) ON DELETE CASCADE,
    source_key TEXT NOT NULL,
    cleanup_action TEXT NOT NULL CHECK(cleanup_action IN ('DELETE_SOURCE','RELEASE_SOURCE','RELEASE_TARGET','DELETE_TARGET')),
    cleaned INTEGER NOT NULL DEFAULT 0 CHECK(cleaned IN (0,1)),
    PRIMARY KEY(operation_id,source_key)
);
CREATE INDEX IF NOT EXISTS operation_cleanup_pending
    ON operation_cleanup_roots(operation_id) WHERE cleaned=0;

CREATE TABLE IF NOT EXISTS cargo_sessions(
    session_id TEXT PRIMARY KEY,
    storage_id TEXT NOT NULL REFERENCES storage_containers(storage_id),
    idempotency_key TEXT NOT NULL UNIQUE,
    player_id TEXT NOT NULL,
    status TEXT NOT NULL CHECK(status IN ('OPEN','MATERIALIZED','COMMITTED','CLEANED','ABORTED')),
    expected_revision BIGINT NOT NULL,
    cursor TEXT NOT NULL,
    next_cursor TEXT NOT NULL,
    original_root_ids_json JSONB NOT NULL,
    physical_source_keys_json JSONB NOT NULL DEFAULT '[]'::jsonb,
    created_ms BIGINT NOT NULL,
    updated_ms BIGINT NOT NULL,
    result_revision BIGINT,
    error TEXT
) WITH (fillfactor=80, autovacuum_vacuum_scale_factor=0.05, autovacuum_analyze_scale_factor=0.02);
CREATE UNIQUE INDEX IF NOT EXISTS cargo_sessions_one_active_storage
    ON cargo_sessions(storage_id) WHERE status IN ('OPEN','MATERIALIZED','COMMITTED');
CREATE INDEX IF NOT EXISTS cargo_sessions_incomplete ON cargo_sessions(created_ms)
    WHERE status IN ('OPEN','MATERIALIZED','COMMITTED');
CREATE INDEX IF NOT EXISTS cargo_sessions_terminal_retention ON cargo_sessions(updated_ms,session_id)
    WHERE status IN ('CLEANED','ABORTED');

CREATE TABLE IF NOT EXISTS cargo_session_cleanup_roots(
    session_id TEXT NOT NULL REFERENCES cargo_sessions(session_id) ON DELETE CASCADE,
    source_key TEXT NOT NULL,
    cleaned INTEGER NOT NULL DEFAULT 0 CHECK(cleaned IN (0,1)),
    PRIMARY KEY(session_id,source_key)
);
CREATE INDEX IF NOT EXISTS cargo_session_cleanup_pending
    ON cargo_session_cleanup_roots(session_id) WHERE cleaned=0;

CREATE TABLE IF NOT EXISTS cargo_migrations(
    migration_id TEXT PRIMARY KEY,
    storage_id TEXT NOT NULL REFERENCES storage_containers(storage_id),
    container_class TEXT NOT NULL,
    status TEXT NOT NULL CHECK(status IN ('PREPARED','COMMITTED','CLEANED')),
    source_fingerprint TEXT NOT NULL,
    expected_revision BIGINT NOT NULL,
    source_roots_json JSONB NOT NULL,
    normalized_items_json JSONB NOT NULL,
    created_ms BIGINT NOT NULL,
    updated_ms BIGINT NOT NULL,
    result_revision BIGINT,
    error TEXT,
    UNIQUE(storage_id,source_fingerprint)
) WITH (fillfactor=80, autovacuum_vacuum_scale_factor=0.05, autovacuum_analyze_scale_factor=0.02);
CREATE UNIQUE INDEX IF NOT EXISTS cargo_migrations_one_active_storage
    ON cargo_migrations(storage_id) WHERE status IN ('PREPARED','COMMITTED');
CREATE INDEX IF NOT EXISTS cargo_migrations_incomplete ON cargo_migrations(created_ms)
    WHERE status IN ('PREPARED','COMMITTED');
CREATE INDEX IF NOT EXISTS cargo_migrations_terminal_retention ON cargo_migrations(updated_ms,migration_id)
    WHERE status='CLEANED';

CREATE TABLE IF NOT EXISTS cargo_migration_roots(
    storage_id TEXT NOT NULL REFERENCES storage_containers(storage_id),
    source_key TEXT NOT NULL,
    migration_id TEXT NOT NULL REFERENCES cargo_migrations(migration_id) ON DELETE CASCADE,
    virtual_root_id TEXT NOT NULL,
    cleaned INTEGER NOT NULL DEFAULT 0 CHECK(cleaned IN (0,1)),
    PRIMARY KEY(storage_id,source_key)
);
CREATE INDEX IF NOT EXISTS cargo_migration_roots_migration ON cargo_migration_roots(migration_id) WHERE cleaned=0;

CREATE TABLE IF NOT EXISTS cargo_migration_observations(
    provider_id TEXT NOT NULL,
    provider_key TEXT NOT NULL,
    container_class TEXT NOT NULL,
    status TEXT NOT NULL,
    physical_roots BIGINT NOT NULL,
    captured_roots BIGINT NOT NULL,
    rejected_roots BIGINT NOT NULL,
    detail TEXT NOT NULL,
    updated_ms BIGINT NOT NULL,
    PRIMARY KEY(provider_id,provider_key)
);
CREATE INDEX IF NOT EXISTS cargo_migration_observations_recent
    ON cargo_migration_observations(provider_id,updated_ms DESC);

CREATE TABLE IF NOT EXISTS audit_events(
    event_id BIGINT GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY,
    operation_id TEXT NOT NULL,
    event_type TEXT NOT NULL,
    detail_json JSONB NOT NULL,
    created_ms BIGINT NOT NULL
);
CREATE INDEX IF NOT EXISTS audit_events_retention ON audit_events(created_ms,event_id);

CREATE TABLE IF NOT EXISTS legacy_imports(
    source_fingerprint TEXT PRIMARY KEY,
    source_path TEXT NOT NULL,
    imported_ms BIGINT NOT NULL
);
)SQL");

    Statement record(writer_,
        "INSERT INTO schema_migrations(version,applied_ms) VALUES(?1,?2) ON CONFLICT(version) DO NOTHING");
    record.bind(1, static_cast<std::int64_t>(schema_version));
    record.bind(2, now_unix_ms());
    record.done();

    Statement app(writer_,
        "INSERT INTO application_meta(key,value) VALUES('application','ClippyVirtualCargo') "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    app.done();
    transaction.commit();
}

json StorageDatabase::resolve_container(const json& request) {
    const auto provider_id = required_string(request, "provider_id", 128);
    const auto provider_key = required_string(request, "provider_key", 512);
    const auto display_name = request.value("display_name", provider_key);
    const auto capacity = request.value("capacity_slots", 0LL);
    if (display_name.empty() || display_name.size() > 256 || capacity < 0 || capacity > 100000000) {
        throw ApiError(400, "invalid_request", "display_name or capacity_slots is invalid.");
    }

    std::lock_guard lock(writer_gate_);
    Transaction transaction(writer_);
    const auto now = now_unix_ms();
    Statement insert(writer_,
        "INSERT INTO storage_containers(storage_id,provider_id,provider_key,display_name,capacity_slots,created_ms,updated_ms) "
        "VALUES(?1,?2,?3,?4,?5,?6,?6) ON CONFLICT(provider_id,provider_key) DO UPDATE SET "
        "display_name=excluded.display_name,capacity_slots=excluded.capacity_slots,updated_ms=excluded.updated_ms "
        "WHERE storage_containers.display_name<>excluded.display_name "
        "OR storage_containers.capacity_slots<>excluded.capacity_slots");
    insert.bind(1, random_hex(16)); insert.bind(2, provider_id); insert.bind(3, provider_key);
    insert.bind(4, display_name); insert.bind(5, static_cast<std::int64_t>(capacity)); insert.bind(6, now); insert.done();
    Statement query(writer_,
        "SELECT storage_id,provider_id,provider_key,display_name,capacity_slots,revision,updated_ms "
        "FROM storage_containers WHERE provider_id=?1 AND provider_key=?2");
    query.bind(1, provider_id); query.bind(2, provider_key); query.row();
    auto result = container_row(query);
    transaction.commit();
    return result;
}

json StorageDatabase::snapshot(const json& request) {
    const auto storage_id = required_string(request, "storage_id", 64);
    const auto cursor = request.value("cursor", "");
    const auto limit = std::clamp(request.value("limit", 100), 1, 500);
    auto& slot = reader();
    std::lock_guard lock(slot.mutex);
    Transaction transaction(slot.connection, "BEGIN");
    const auto revision = container_revision(slot.connection, storage_id);
    json items = json::array();
    Statement query(slot.connection,
        "SELECT root_item_id,class_name,quantity,health,state_json::text FROM cargo_roots WHERE storage_id=?1 "
        "AND root_item_id>?2 ORDER BY root_item_id LIMIT ?3");
    query.bind(1, storage_id); query.bind(2, cursor); query.bind(3, static_cast<std::int64_t>(limit + 1));
    std::string next_cursor;
    while (query.row()) {
        if (items.size() == static_cast<std::size_t>(limit)) { next_cursor = items.back()["item_id"].get<std::string>(); break; }
        items.push_back({{"item_id", query.text(0)}, {"class_name", query.text(1)},
                         {"quantity", query.number(2)}, {"health", query.number(3)},
                         {"state", json::parse(query.text(4))}});
    }
    Statement count(slot.connection, "SELECT count(*) FROM cargo_roots WHERE storage_id=?1");
    count.bind(1, storage_id); count.row();
    auto result = json{{"storage_id", storage_id}, {"revision", revision}, {"total_roots", count.integer(0)},
                       {"items", items}, {"next_cursor", next_cursor}};
    transaction.commit();
    return result;
}

json StorageDatabase::item_tree(const json& request) {
    const auto storage_id = required_string(request, "storage_id", 64);
    const auto item_id = required_string(request, "item_id", 64);
    auto& slot = reader();
    std::lock_guard lock(slot.mutex);
    Transaction transaction(slot.connection, "BEGIN");
    auto result = json{{"storage_id", storage_id}, {"revision", container_revision(slot.connection, storage_id)},
                       {"item", load_tree(slot.connection, storage_id, item_id)}};
    transaction.commit();
    return result;
}

json StorageDatabase::prepare_deposit(const json& request) {
    const auto storage_id = required_string(request, "storage_id", 64);
    const auto key = required_string(request, "idempotency_key", 256);
    const auto expected = required_revision(request);
    if (!request.contains("item")) throw ApiError(400, "invalid_request", "item is required.");
    const auto request_hash = fingerprint(json{{"kind","deposit"},{"storage_id",storage_id},
                                                {"expected_revision",expected},{"item",request["item"]}}.dump());
    std::size_t count = 0;
    auto item = normalize_item(request["item"], 1, count, config_);

    std::lock_guard lock(writer_gate_);
    Transaction transaction(writer_);
    lock_idempotency_key(writer_, key);
    if (auto existing = find_idempotent(writer_, key, request_hash); !existing.is_null()) {
        transaction.commit(); return existing;
    }
    assert_revision(writer_, storage_id, expected);
    assert_storage_workflow_available(writer_, storage_id);
    const auto operation_id = random_hex(16);
    const auto root_id = item["item_id"].get<std::string>();
    const auto now = now_unix_ms();
    Statement insert(writer_,
        "INSERT INTO operations(operation_id,idempotency_key,request_fingerprint,kind,status,storage_id,root_item_id,"
        "expected_revision,payload_json,created_ms,updated_ms) VALUES(?1,?2,?3,'deposit','PREPARED',?4,?5,?6,?7,?8,?8)");
    insert.bind(1, operation_id); insert.bind(2, key); insert.bind(3, request_hash); insert.bind(4, storage_id);
    insert.bind(5, root_id); insert.bind(6, expected); insert.bind(7, item.dump()); insert.bind(8, now); insert.done();
    audit(writer_, operation_id, "deposit_prepared", {{"nodes", count}});
    auto result = find_operation(writer_, operation_id);
    transaction.commit();
    return result;
}

json StorageDatabase::prepare_withdrawal(const json& request) {
    const auto storage_id = required_string(request, "storage_id", 64);
    const auto item_id = required_string(request, "item_id", 64);
    const auto key = required_string(request, "idempotency_key", 256);
    const auto expected = required_revision(request);
    const auto request_hash = fingerprint(json{{"kind","withdraw"},{"storage_id",storage_id},
                                                {"expected_revision",expected},{"item_id",item_id}}.dump());
    std::lock_guard lock(writer_gate_);
    Transaction transaction(writer_);
    lock_idempotency_key(writer_, key);
    if (auto existing = find_idempotent(writer_, key, request_hash); !existing.is_null()) {
        transaction.commit(); return existing;
    }
    assert_revision(writer_, storage_id, expected);
    assert_storage_workflow_available(writer_, storage_id);
    auto item = load_tree(writer_, storage_id, item_id);
    const auto root_id = item["item_id"].get<std::string>();
    if (root_id != item_id) throw ApiError(400, "root_required", "Withdrawals must select a root item.");
    const auto operation_id = random_hex(16);
    const auto now = now_unix_ms();
    Statement insert(writer_,
        "INSERT INTO operations(operation_id,idempotency_key,request_fingerprint,kind,status,storage_id,root_item_id,"
        "expected_revision,payload_json,created_ms,updated_ms) VALUES(?1,?2,?3,'withdraw','PREPARED',?4,?5,?6,?7,?8,?8)");
    insert.bind(1, operation_id); insert.bind(2, key); insert.bind(3, request_hash); insert.bind(4, storage_id);
    insert.bind(5, root_id); insert.bind(6, expected); insert.bind(7, item.dump()); insert.bind(8, now); insert.done();
    audit(writer_, operation_id, "withdraw_prepared");
    auto result = find_operation(writer_, operation_id);
    transaction.commit();
    return result;
}

json StorageDatabase::mark_quarantined(const json& request) {
    const auto operation_id = required_string(request, "operation_id", 64);
    const auto physical_source_key = required_string(request, "physical_source_key", 512);
    std::lock_guard lock(writer_gate_);
    Transaction transaction(writer_);
    auto operation = find_operation(writer_, operation_id);
    const auto status = operation["status"].get<std::string>();
    if (status == "PREPARED") {
        assert_storage_workflow_available(writer_, operation["storage_id"].get<std::string>(),
                                          "direct operation", operation_id);
        Statement update(writer_,
            "UPDATE operations SET status='QUARANTINED',physical_source_key=?2,updated_ms=?3 WHERE operation_id=?1");
        update.bind(1, operation_id); update.bind(2, physical_source_key); update.bind(3, now_unix_ms()); update.done();
        audit(writer_, operation_id, "quarantined", {{"physical_source_key", physical_source_key}});
        operation = find_operation(writer_, operation_id);
    } else if (status == "QUARANTINED" && !operation.contains("physical_source_key")) {
        // A pre-v6 active operation has no durable physical identity. A
        // repeated marker may attach it before the operation proceeds.
        Statement update(writer_,
            "UPDATE operations SET physical_source_key=?2,updated_ms=?3 WHERE operation_id=?1");
        update.bind(1, operation_id); update.bind(2, physical_source_key); update.bind(3, now_unix_ms()); update.done();
        audit(writer_, operation_id, "quarantine_identity_attached",
              {{"physical_source_key", physical_source_key}});
        operation = find_operation(writer_, operation_id);
    } else if (status == "QUARANTINED" || status == "COMMITTED" || status == "CLEANED") {
        if (!operation.contains("physical_source_key") ||
            operation["physical_source_key"].get<std::string>() != physical_source_key) {
            throw ApiError(409, "quarantine_conflict",
                           "This operation was already associated with another physical source key.");
        }
    } else {
        throw ApiError(409, "invalid_operation_state", "Only a prepared operation can enter quarantine.");
    }
    transaction.commit();
    return operation;
}

json StorageDatabase::commit_deposit(const json& request) {
    const auto operation_id = required_string(request, "operation_id", 64);
    std::lock_guard lock(writer_gate_);
    Transaction transaction(writer_);
    auto operation = find_operation(writer_, operation_id);
    if (operation["kind"] != "deposit") throw ApiError(409, "wrong_operation_kind", "This is not a deposit.");
    if (operation["status"] == "COMMITTED" || operation["status"] == "CLEANED") {
        transaction.commit(); return operation;
    }
    if (operation["status"] != "QUARANTINED") throw ApiError(409, "invalid_operation_state", "Deposit must be quarantined before commit.");
    const auto storage_id = operation["storage_id"].get<std::string>();
    const auto expected = operation["expected_revision"].get<std::int64_t>();
    if (!operation.contains("physical_source_key")) {
        throw ApiError(409, "physical_identity_required",
                       "The quarantined physical source must have a durable source key before commit.");
    }
    assert_storage_workflow_available(writer_, storage_id, "direct operation", operation_id);
    assert_revision(writer_, storage_id, expected);
    {
        Statement capacity(writer_,
            "SELECT (SELECT count(*) FROM cargo_roots i WHERE i.storage_id=c.storage_id "
            "),capacity_slots FROM storage_containers c WHERE c.storage_id=?1");
        capacity.bind(1, storage_id);
        if (!capacity.row()) throw ApiError(404, "storage_not_found", "The storage container does not exist.");
        if (capacity.integer(0) >= capacity.integer(1)) {
            throw ApiError(409, "storage_capacity",
                           "The virtual container has reached its configured root-item capacity.");
        }
    }
    const auto now = now_unix_ms();
    ItemInserter inserter(writer_);
    const auto inserted_nodes = inserter.insert_tree(storage_id, operation["item"], now);
    Statement revision(writer_, "UPDATE storage_containers SET revision=revision+1,updated_ms=?2 WHERE storage_id=?1");
    revision.bind(1, storage_id); revision.bind(2, now); revision.done();
    Statement cleanup(writer_,
        "INSERT INTO operation_cleanup_roots(operation_id,source_key,cleanup_action) VALUES(?1,?2,'DELETE_SOURCE')");
    cleanup.bind(1, operation_id); cleanup.bind(2, operation["physical_source_key"].get<std::string>()); cleanup.done();
    Statement finish(writer_,
        "UPDATE operations SET status='COMMITTED',cleanup_state='PENDING',result_revision=?2,updated_ms=?3 "
        "WHERE operation_id=?1");
    finish.bind(1, operation_id); finish.bind(2, expected + 1); finish.bind(3, now); finish.done();
    audit(writer_, operation_id, "deposit_committed", {{"nodes", inserted_nodes}});
    operation = find_operation(writer_, operation_id);
    transaction.commit();
    return operation;
}

json StorageDatabase::commit_withdrawal(const json& request) {
    const auto operation_id = required_string(request, "operation_id", 64);
    std::lock_guard lock(writer_gate_);
    Transaction transaction(writer_);
    auto operation = find_operation(writer_, operation_id);
    if (operation["kind"] != "withdraw") throw ApiError(409, "wrong_operation_kind", "This is not a withdrawal.");
    if (operation["status"] == "COMMITTED" || operation["status"] == "CLEANED") {
        transaction.commit(); return operation;
    }
    if (operation["status"] != "QUARANTINED") throw ApiError(409, "invalid_operation_state", "Withdrawal must be quarantined before commit.");
    const auto storage_id = operation["storage_id"].get<std::string>();
    const auto expected = operation["expected_revision"].get<std::int64_t>();
    if (!operation.contains("physical_source_key")) {
        throw ApiError(409, "physical_identity_required",
                       "The quarantined physical target must have a durable source key before commit.");
    }
    assert_storage_workflow_available(writer_, storage_id, "direct operation", operation_id);
    assert_revision(writer_, storage_id, expected);
    Statement remove(writer_, "DELETE FROM cargo_roots WHERE storage_id=?1 AND root_item_id=?2");
    remove.bind(1, storage_id); remove.bind(2, operation["root_item_id"].get<std::string>()); remove.done();
    if (remove.affected_rows() != 1) throw ApiError(409, "item_changed", "The item is no longer available.", true);
    const auto now = now_unix_ms();
    Statement revision(writer_, "UPDATE storage_containers SET revision=revision+1,updated_ms=?2 WHERE storage_id=?1");
    revision.bind(1, storage_id); revision.bind(2, now); revision.done();
    Statement cleanup(writer_,
        "INSERT INTO operation_cleanup_roots(operation_id,source_key,cleanup_action) VALUES(?1,?2,'RELEASE_TARGET')");
    cleanup.bind(1, operation_id); cleanup.bind(2, operation["physical_source_key"].get<std::string>()); cleanup.done();
    Statement finish(writer_,
        "UPDATE operations SET status='COMMITTED',cleanup_state='PENDING',result_revision=?2,updated_ms=?3 "
        "WHERE operation_id=?1");
    finish.bind(1, operation_id); finish.bind(2, expected + 1); finish.bind(3, now); finish.done();
    audit(writer_, operation_id, "withdraw_committed");
    operation = find_operation(writer_, operation_id);
    transaction.commit();
    return operation;
}

json StorageDatabase::abort_operation(const json& request) {
    const auto operation_id = required_string(request, "operation_id", 64);
    const auto reason = request.value("reason", "aborted by DayZ server");
    std::lock_guard lock(writer_gate_);
    Transaction transaction(writer_);
    auto operation = find_operation(writer_, operation_id);
    if (operation["status"] == "COMMITTED" || operation["status"] == "CLEANED") {
        throw ApiError(409, "already_committed", "A committed operation cannot be aborted.");
    }
    if (operation["status"] == "PREPARED") {
        Statement update(writer_,
            "UPDATE operations SET status='ABORTED',cleanup_state='CLEANED',error=?2,updated_ms=?3 "
            "WHERE operation_id=?1");
        update.bind(1, operation_id); update.bind(2, reason.substr(0, 1024)); update.bind(3, now_unix_ms()); update.done();
        audit(writer_, operation_id, "aborted_before_quarantine", {{"reason", reason.substr(0, 1024)}});
        operation = find_operation(writer_, operation_id);
    } else if (operation["status"] == "QUARANTINED") {
        if (!operation.contains("physical_source_key")) {
            throw ApiError(409, "physical_identity_required",
                           "A quarantined operation needs its durable physical source key before abort recovery.");
        }
        const auto action = operation["kind"] == "deposit" ? "RELEASE_SOURCE" : "DELETE_TARGET";
        Statement cleanup(writer_,
            "INSERT INTO operation_cleanup_roots(operation_id,source_key,cleanup_action) VALUES(?1,?2,?3)");
        cleanup.bind(1, operation_id); cleanup.bind(2, operation["physical_source_key"].get<std::string>());
        cleanup.bind(3, std::string(action)); cleanup.done();
        Statement update(writer_,
            "UPDATE operations SET status='ABORTED',cleanup_state='PENDING',error=?2,updated_ms=?3 "
            "WHERE operation_id=?1");
        update.bind(1, operation_id); update.bind(2, reason.substr(0, 1024)); update.bind(3, now_unix_ms()); update.done();
        audit(writer_, operation_id, "abort_cleanup_pending",
              {{"reason", reason.substr(0, 1024)}, {"cleanup_action", action}});
        operation = find_operation(writer_, operation_id);
    }
    transaction.commit();
    return operation;
}

json StorageDatabase::acknowledge_operation_cleanup(const json& request) {
    const auto operation_id = required_string(request, "operation_id", 64);
    const auto cleaned_source_keys = required_source_keys(request, "cleaned_source_keys", 1);
    if (cleaned_source_keys.empty()) {
        throw ApiError(400, "invalid_request", "cleaned_source_keys must contain the reconciled physical key.");
    }
    std::lock_guard lock(writer_gate_);
    Transaction transaction(writer_);
    auto operation = find_operation(writer_, operation_id);
    if (operation["cleanup_state"] != "PENDING" && operation["cleanup_state"] != "CLEANED") {
        throw ApiError(409, "invalid_operation_state",
                       "Physical cleanup can be acknowledged only for a pending or completed reconciliation.");
    }
    Statement update(writer_,
        "UPDATE operation_cleanup_roots SET cleaned=1 "
        "WHERE operation_id=?1 AND source_key=?2 AND cleaned=0");
    Statement known(writer_,
        "SELECT 1 FROM operation_cleanup_roots WHERE operation_id=?1 AND source_key=?2");
    const auto key = cleaned_source_keys[0].get<std::string>();
    update.bind(1, operation_id); update.bind(2, key); update.done();
    if (update.affected_rows() == 0) {
        known.bind(1, operation_id); known.bind(2, key);
        if (!known.row()) {
            throw ApiError(400, "unknown_source_key", "The cleanup key does not belong to this operation.");
        }
    }
    if (operation["cleanup_state"] == "PENDING") {
        Statement finish(writer_,
            "UPDATE operations SET status=CASE WHEN status='COMMITTED' THEN 'CLEANED' ELSE status END,"
            "cleanup_state='CLEANED',updated_ms=?2 WHERE operation_id=?1");
        finish.bind(1, operation_id); finish.bind(2, now_unix_ms()); finish.done();
        audit(writer_, operation_id, "operation_physical_cleanup_acknowledged");
    }
    operation = find_operation(writer_, operation_id);
    transaction.commit();
    return operation;
}

json StorageDatabase::incomplete_operations(const json& request) {
    const auto provider_id = required_string(request, "provider_id", 128);
    const auto limit = std::clamp(request.value("limit", 500), 1, 5000);
    auto& slot = reader();
    std::lock_guard lock(slot.mutex);
    Transaction transaction(slot.connection, "BEGIN");
    json operations = json::array();
    Statement query(slot.connection,
        "SELECT o.operation_id,o.kind,o.status,o.storage_id,o.root_item_id,o.expected_revision,o.payload_json::text,"
        "o.created_ms,o.updated_ms,o.error,o.result_revision,o.cleanup_state,o.physical_source_key,"
        "(SELECT cleanup_action FROM operation_cleanup_roots r WHERE r.operation_id=o.operation_id LIMIT 1),"
        "COALESCE((SELECT jsonb_agg(source_key ORDER BY source_key) FROM operation_cleanup_roots r "
        "WHERE r.operation_id=o.operation_id AND r.cleaned=0),'[]'::jsonb)::text,c.provider_id,c.provider_key "
        "FROM operations o JOIN storage_containers c ON c.storage_id=o.storage_id "
        "WHERE c.provider_id=?1 AND (o.status IN ('PREPARED','QUARANTINED') OR o.cleanup_state='PENDING') "
        "ORDER BY o.created_ms LIMIT ?2");
    query.bind(1, provider_id); query.bind(2, static_cast<std::int64_t>(limit));
    while (query.row()) {
        auto operation = operation_row(query);
        operation["provider_id"] = query.text(15);
        operation["provider_key"] = query.text(16);
        operations.push_back(std::move(operation));
    }
    transaction.commit();
    return {{"provider_id", provider_id}, {"operations", operations}};
}

json StorageDatabase::open_session(const json& request) {
    const auto storage_id = required_string(request, "storage_id", 64);
    const auto player_id = required_string(request, "player_id", 128);
    const auto key = required_string(request, "idempotency_key", 256);
    const auto cursor = request.value("cursor", "");
    const auto limit = std::clamp(request.value("limit", 40), 1, 200);
    const auto expected = required_revision(request);
    if (!request.value("cursor", "").empty() && cursor.size() > 64) {
        throw ApiError(400, "invalid_request", "cursor is too long.");
    }

    std::lock_guard lock(writer_gate_);
    Transaction transaction(writer_);

    lock_idempotency_key(writer_, key);
    Statement retry(writer_,
        "SELECT session_id,storage_id,player_id FROM cargo_sessions WHERE idempotency_key=?1 FOR UPDATE");
    retry.bind(1, key);
    if (retry.row()) {
        if (retry.text(1) != storage_id || retry.text(2) != player_id) {
            throw ApiError(409, "idempotency_conflict",
                           "The idempotency key was already used for another cargo session.");
        }
        auto result = find_session(writer_, retry.text(0));
        json trees = json::array();
        if ((result["status"] == "OPEN" || result["status"] == "MATERIALIZED") &&
            !result["original_root_ids"].empty()) {
            Statement retry_page(writer_,
                "SELECT tree_json::text FROM cargo_roots WHERE storage_id=?1 AND root_item_id>?2 "
                "ORDER BY root_item_id LIMIT ?3");
            retry_page.bind(1, storage_id);
            retry_page.bind(2, result.value("cursor", ""));
            retry_page.bind(3, static_cast<std::int64_t>(result["original_root_ids"].size()));
            while (retry_page.row()) trees.push_back(json::parse(retry_page.text(0)));
            if (trees.size() != result["original_root_ids"].size()) {
                throw ApiError(409, "session_page_changed",
                               "The virtual cargo page changed while an active session existed.", true);
            }
        }
        result["items"] = std::move(trees);
        transaction.commit();
        return result;
    }

    assert_revision(writer_, storage_id, expected);
    assert_storage_workflow_available(writer_, storage_id);

    // Pick the page from indexed metadata first, then join only the selected roots
    // to their JSONB trees. This keeps a 20-root page from pulling 20 huge trees
    // across PostgreSQL only to discard most of them because of the node budget.
    json root_ids = json::array();
    json trees = json::array();
    std::size_t page_nodes = 0;
    bool has_more = false;
    Statement page(writer_, R"SQL(
WITH candidates AS (
    SELECT root_item_id,node_count,
           SUM(node_count) OVER (ORDER BY root_item_id ROWS UNBOUNDED PRECEDING) AS running_nodes,
           ROW_NUMBER() OVER (ORDER BY root_item_id) AS rn
    FROM cargo_roots
    WHERE storage_id=?1 AND root_item_id>?2
    ORDER BY root_item_id
    LIMIT ?3
), selected AS (
    SELECT root_item_id,node_count
    FROM candidates
    WHERE rn=1 OR running_nodes<=?4
    ORDER BY root_item_id
    LIMIT ?5
)
SELECT r.root_item_id,r.node_count,r.tree_json::text,
       CASE WHEN EXISTS(
           SELECT 1 FROM cargo_roots more
           WHERE more.storage_id=?1 AND more.root_item_id>r.root_item_id
       ) THEN 1 ELSE 0 END AS has_more
FROM selected s
JOIN cargo_roots r ON r.storage_id=?1 AND r.root_item_id=s.root_item_id
ORDER BY r.root_item_id
)SQL");
    page.bind(1, storage_id);
    page.bind(2, cursor);
    page.bind(3, static_cast<std::int64_t>(limit + 1));
    page.bind(4, static_cast<std::int64_t>(config_.max_page_nodes));
    page.bind(5, static_cast<std::int64_t>(limit));
    while (page.row()) {
        root_ids.push_back(page.text(0));
        const auto node_count = static_cast<std::size_t>((std::max)(std::int64_t{1}, page.integer(1)));
        page_nodes += node_count;
        trees.push_back(json::parse(page.text(2)));
        has_more = page.integer(3) != 0;
    }
    const std::string next_cursor = has_more && !root_ids.empty()
        ? root_ids.back().get<std::string>() : "";

    const auto session_id = random_hex(16);
    const auto now = now_unix_ms();
    Statement insert(writer_,
        "INSERT INTO cargo_sessions(session_id,storage_id,idempotency_key,player_id,status,expected_revision,cursor,"
        "next_cursor,original_root_ids_json,created_ms,updated_ms) "
        "VALUES(?1,?2,?3,?4,'OPEN',?5,?6,?7,?8,?9,?9)");
    insert.bind(1, session_id); insert.bind(2, storage_id); insert.bind(3, key); insert.bind(4, player_id);
    insert.bind(5, expected); insert.bind(6, cursor); insert.bind(7, next_cursor);
    insert.bind(8, root_ids.dump()); insert.bind(9, now); insert.done();
    audit(writer_, session_id, "session_opened",
          {{"player_id", player_id}, {"roots", root_ids.size()}, {"nodes", page_nodes}});

    auto result = find_session(writer_, session_id);
    result["items"] = std::move(trees);
    transaction.commit();
    return result;
}

json StorageDatabase::mark_session_materialized(const json& request) {
    const auto session_id = required_string(request, "session_id", 64);
    const auto physical_source_keys = required_source_keys(request, "physical_source_keys");
    std::lock_guard lock(writer_gate_);
    Transaction transaction(writer_);
    auto session = find_session(writer_, session_id);
    const auto status = session["status"].get<std::string>();
    if (status == "OPEN") {
        assert_storage_workflow_available(writer_, session["storage_id"].get<std::string>(),
                                          "page session", session_id);
        json materialized_ids = session["original_root_ids"];
        if (request.contains("root_ids")) {
            if (!request["root_ids"].is_array()) {
                throw ApiError(400, "invalid_request", "root_ids must be an array.");
            }
            if (request["root_ids"].size() > session["original_root_ids"].size()) {
                throw ApiError(400, "invalid_request",
                               "root_ids must be an original-order prefix of the roots offered by this session.");
            }
            materialized_ids = json::array();
            for (std::size_t index = 0; index < request["root_ids"].size(); ++index) {
                const auto& id_value = request["root_ids"][index];
                if (!id_value.is_string()) throw ApiError(400, "invalid_request", "Every root id must be a string.");
                const auto id = id_value.get<std::string>();
                if (id != session["original_root_ids"][index].get<std::string>()) {
                    throw ApiError(400, "invalid_request",
                                   "root_ids must be an original-order prefix of the roots offered by this session.");
                }
                materialized_ids.push_back(id);
            }
        }
        if (materialized_ids.size() != physical_source_keys.size()) {
            throw ApiError(400, "invalid_request",
                           "physical_source_keys must contain one key for every materialized root id.");
        }
        std::string adjusted_next = session["next_cursor"].get<std::string>();
        if (materialized_ids.size() < session["original_root_ids"].size()) {
            adjusted_next = materialized_ids.empty() ? session["cursor"].get<std::string>()
                                                     : materialized_ids.back().get<std::string>();
        }
        Statement update(writer_,
            "UPDATE cargo_sessions SET status='MATERIALIZED',original_root_ids_json=?2,next_cursor=?3,"
            "physical_source_keys_json=?4,updated_ms=?5 "
            "WHERE session_id=?1");
        update.bind(1, session_id); update.bind(2, materialized_ids.dump()); update.bind(3, adjusted_next);
        update.bind(4, physical_source_keys.dump()); update.bind(5, now_unix_ms()); update.done();
        audit(writer_, session_id, "session_materialized",
              {{"roots", materialized_ids.size()}, {"physical_source_keys", physical_source_keys.size()}});
        session = find_session(writer_, session_id);
    } else if (status == "MATERIALIZED") {
        bool roots_match = true;
        if (request.contains("root_ids")) {
            if (!request["root_ids"].is_array()) {
                throw ApiError(400, "invalid_request", "root_ids must be an array.");
            }
            json requested_roots = json::array();
            for (const auto& value : request["root_ids"]) {
                if (!value.is_string()) {
                    throw ApiError(400, "invalid_request", "Every root id must be a string.");
                }
                const auto id = value.get<std::string>();
                if (id.empty() || id.size() > 64) {
                    throw ApiError(400, "invalid_request", "root_ids contains an invalid item id.");
                }
                requested_roots.push_back(id);
            }
            roots_match = requested_roots == session["original_root_ids"];
        }
        if (!roots_match || session["physical_source_keys"] != physical_source_keys ||
            session["original_root_ids"].size() != physical_source_keys.size()) {
            throw ApiError(409, "materialization_conflict",
                           "This materialized session was already recorded with different roots or physical source keys.");
        }
    } else if (status != "COMMITTED" && status != "CLEANED") {
        throw ApiError(409, "invalid_session_state", "Only an open session can be materialized.");
    }
    transaction.commit();
    return session;
}

json StorageDatabase::commit_session(const json& request) {
    const auto session_id = required_string(request, "session_id", 64);

    std::lock_guard lock(writer_gate_);
    Transaction transaction(writer_);
    auto session = find_session(writer_, session_id);
    if (session["status"] == "COMMITTED" || session["status"] == "CLEANED") {
        transaction.commit();
        return session;
    }
    if (session["status"] != "MATERIALIZED") {
        throw ApiError(409, "invalid_session_state", "The session must be materialized before commit.");
    }
    if (!request.contains("items") || !request["items"].is_array()) {
        throw ApiError(400, "invalid_request", "items must be an array.");
    }
    const auto physical_source_keys = required_source_keys(request, "physical_source_keys");
    if (physical_source_keys.size() != request["items"].size()) {
        throw ApiError(400, "invalid_request",
                       "physical_source_keys must contain one key for every physical root being committed.");
    }

    const auto storage_id = session["storage_id"].get<std::string>();
    const auto expected = session["expected_revision"].get<std::int64_t>();
    assert_storage_workflow_available(writer_, storage_id, "page session", session_id);
    assert_revision(writer_, storage_id, expected);

    std::unordered_set<std::string> original_ids;
    for (const auto& id : session["original_root_ids"]) original_ids.insert(id.get<std::string>());
    std::unordered_set<std::string> retained_ids;
    std::vector<json> normalized;
    std::size_t total_nodes = 0;
    for (const auto& source : request["items"]) {
        std::string supplied_id;
        if (source.is_object() && source.contains("item_id") && source["item_id"].is_string()) {
            const auto candidate = source["item_id"].get<std::string>();
            if (original_ids.contains(candidate) && !retained_ids.contains(candidate)) {
                supplied_id = candidate;
                retained_ids.insert(candidate);
            }
        }
        normalized.push_back(normalize_item(source, 1, total_nodes, config_, supplied_id));
    }

    Statement counts(writer_,
        "SELECT (SELECT count(*) FROM cargo_roots i WHERE i.storage_id=c.storage_id),"
        "capacity_slots FROM storage_containers c WHERE c.storage_id=?1");
    counts.bind(1, storage_id); counts.row();
    const auto resulting_roots = counts.integer(0) - static_cast<std::int64_t>(original_ids.size()) +
                                 static_cast<std::int64_t>(normalized.size());
    if (resulting_roots > counts.integer(1)) {
        throw ApiError(409, "storage_capacity", "The virtual container has reached its configured root-item capacity.");
    }

    json original_id_json = json::array();
    for (const auto& id : original_ids) original_id_json.push_back(id);
    Statement remove(writer_,
        "DELETE FROM cargo_roots WHERE storage_id=?1 "
        "AND root_item_id IN (SELECT value FROM jsonb_array_elements_text(?2::jsonb))");
    remove.bind(1, storage_id); remove.bind(2, original_id_json.dump()); remove.done();
    if (remove.affected_rows() != static_cast<std::int64_t>(original_ids.size())) {
        throw ApiError(409, "item_changed", "A materialized root item is no longer present in storage.", true);
    }
    const auto now = now_unix_ms();
    json normalized_json = json::array();
    for (auto& item : normalized) normalized_json.push_back(std::move(item));
    insert_trees_batch(writer_, storage_id, normalized_json, now);

    Statement cleanup(writer_,
        "INSERT INTO cargo_session_cleanup_roots(session_id,source_key) "
        "SELECT ?1,value FROM jsonb_array_elements_text(?2::jsonb)");
    cleanup.bind(1, session_id); cleanup.bind(2, physical_source_keys.dump()); cleanup.done();

    Statement revision(writer_,
        "UPDATE storage_containers SET revision=revision+1,updated_ms=?2 WHERE storage_id=?1");
    revision.bind(1, storage_id); revision.bind(2, now); revision.done();
    Statement finish(writer_,
        "UPDATE cargo_sessions SET status=?2,result_revision=?3,physical_source_keys_json=?4,updated_ms=?5 "
        "WHERE session_id=?1");
    finish.bind(1, session_id);
    finish.bind(2, std::string(physical_source_keys.empty() ? "CLEANED" : "COMMITTED"));
    finish.bind(3, expected + 1); finish.bind(4, physical_source_keys.dump()); finish.bind(5, now); finish.done();
    audit(writer_, session_id, "session_committed",
          {{"removed_roots", original_ids.size()}, {"stored_roots", normalized.size()}, {"nodes", total_nodes},
           {"cleanup_roots", physical_source_keys.size()}});
    session = find_session(writer_, session_id);
    transaction.commit();
    return session;
}

json StorageDatabase::acknowledge_session_cleanup(const json& request) {
    const auto session_id = required_string(request, "session_id", 64);
    const auto cleaned_source_keys = required_source_keys(request, "cleaned_source_keys");
    std::lock_guard lock(writer_gate_);
    Transaction transaction(writer_);
    auto session = find_session(writer_, session_id);
    const auto status = session["status"].get<std::string>();
    if (status != "COMMITTED" && status != "CLEANED") {
        throw ApiError(409, "invalid_session_state",
                       "Physical roots can be acknowledged only after the session is committed.");
    }

    Statement update(writer_,
        "UPDATE cargo_session_cleanup_roots SET cleaned=1 "
        "WHERE session_id=?1 AND source_key=?2 AND cleaned=0");
    Statement known(writer_,
        "SELECT 1 FROM cargo_session_cleanup_roots WHERE session_id=?1 AND source_key=?2");
    for (const auto& source_key : cleaned_source_keys) {
        const auto key = source_key.get<std::string>();
        update.bind(1, session_id); update.bind(2, key); update.done();
        if (update.affected_rows() == 0) {
            known.bind(1, session_id); known.bind(2, key);
            if (!known.row()) {
                throw ApiError(400, "unknown_source_key",
                               "A cleanup key does not belong to this session.");
            }
            known.reset();
        }
        update.reset();
    }

    Statement remaining(writer_,
        "SELECT count(*) FROM cargo_session_cleanup_roots WHERE session_id=?1 AND cleaned=0");
    remaining.bind(1, session_id); remaining.row();
    if (remaining.integer(0) == 0 && status == "COMMITTED") {
        Statement finish(writer_,
            "UPDATE cargo_sessions SET status='CLEANED',updated_ms=?2 WHERE session_id=?1");
        finish.bind(1, session_id); finish.bind(2, now_unix_ms()); finish.done();
        audit(writer_, session_id, "session_physical_cleanup_acknowledged");
    }
    session = find_session(writer_, session_id);
    transaction.commit();
    return session;
}

json StorageDatabase::abort_session(const json& request) {
    const auto session_id = required_string(request, "session_id", 64);
    const auto reason = request.value("reason", "aborted before materialization");
    std::lock_guard lock(writer_gate_);
    Transaction transaction(writer_);
    auto session = find_session(writer_, session_id);
    if (session["status"] == "MATERIALIZED") {
        throw ApiError(409, "recovery_required",
                       "A materialized session cannot be aborted because that could duplicate items.");
    }
    if (session["status"] == "COMMITTED" || session["status"] == "CLEANED") {
        transaction.commit();
        return session;
    }
    if (session["status"] != "ABORTED") {
        Statement update(writer_,
            "UPDATE cargo_sessions SET status='ABORTED',error=?2,updated_ms=?3 WHERE session_id=?1");
        update.bind(1, session_id); update.bind(2, reason.substr(0, 1024)); update.bind(3, now_unix_ms()); update.done();
        audit(writer_, session_id, "session_aborted", {{"reason", reason.substr(0, 1024)}});
        session = find_session(writer_, session_id);
    }
    transaction.commit();
    return session;
}

json StorageDatabase::incomplete_sessions(const json& request) {
    const auto provider_id = required_string(request, "provider_id", 128);
    const auto limit = std::clamp(request.value("limit", 500), 1, 5000);
    auto& slot = reader();
    std::lock_guard lock(slot.mutex);
    Transaction transaction(slot.connection, "BEGIN");
    json sessions = json::array();
    Statement query(slot.connection,
        "SELECT session_id,s.storage_id,c.provider_key,player_id,status,expected_revision,cursor,next_cursor,"
        "original_root_ids_json::text,s.created_ms,s.updated_ms,result_revision,error,s.physical_source_keys_json::text,"
        "COALESCE((SELECT jsonb_agg(source_key ORDER BY source_key) FROM cargo_session_cleanup_roots r "
        "WHERE r.session_id=s.session_id AND r.cleaned=0),'[]'::jsonb)::text,c.provider_id "
        "FROM cargo_sessions s JOIN storage_containers c ON c.storage_id=s.storage_id "
        "WHERE c.provider_id=?1 AND status IN ('OPEN','MATERIALIZED','COMMITTED') "
        "ORDER BY s.created_ms LIMIT ?2");
    query.bind(1, provider_id); query.bind(2, static_cast<std::int64_t>(limit));
    while (query.row()) {
        auto session = session_row(query);
        session["provider_id"] = query.text(15);
        sessions.push_back(std::move(session));
    }
    transaction.commit();
    return {{"provider_id", provider_id}, {"sessions", sessions}};
}

json StorageDatabase::prepare_migration(const json& request) {
    const auto storage_id = required_string(request, "storage_id", 64);
    const auto container_class = required_string(request, "container_class", 256);
    const auto expected = required_revision(request);
    if (!request.contains("roots") || !request["roots"].is_array() || request["roots"].empty()) {
        throw ApiError(400, "invalid_request", "roots must be a non-empty array.");
    }
    if (request["roots"].size() > 200) {
        throw ApiError(400, "invalid_request", "A migration batch cannot contain more than 200 roots.");
    }

    std::lock_guard lock(writer_gate_);
    Transaction transaction(writer_);
    assert_revision(writer_, storage_id, expected);

    // A source key is the durable DayZ identity of a physical root. If a prior
    // commit was followed by a crash before deletion was persisted, return the
    // original migration so DayZ removes the resurrected physical copy without
    // inserting it a second time.
    for (const auto& root : request["roots"]) {
        if (!root.is_object()) throw ApiError(400, "invalid_request", "Every migration root must be an object.");
        const auto source_key = required_string(root, "source_key", 512);
        Statement existing(writer_,
            "SELECT migration_id FROM cargo_migration_roots WHERE storage_id=?1 AND source_key=?2");
        existing.bind(1, storage_id); existing.bind(2, source_key);
        if (existing.row()) {
            auto result = find_migration(writer_, existing.text(0));
            transaction.commit();
            return result;
        }
    }

    Statement active(writer_,
        "SELECT migration_id FROM cargo_migrations WHERE storage_id=?1 AND status IN ('PREPARED','COMMITTED') LIMIT 1");
    active.bind(1, storage_id);
    if (active.row()) {
        auto result = find_migration(writer_, active.text(0));
        transaction.commit();
        return result;
    }
    assert_storage_workflow_available(writer_, storage_id);

    json normalized_items = json::array();
    json source_roots = json::array();
    std::unordered_set<std::string> source_keys;
    std::size_t total_nodes = 0;
    for (const auto& root : request["roots"]) {
        const auto source_key = required_string(root, "source_key", 512);
        if (!source_keys.insert(source_key).second) {
            throw ApiError(400, "invalid_request", "A migration source key was supplied more than once.");
        }
        if (!root.contains("item")) throw ApiError(400, "invalid_request", "Every migration root needs an item tree.");
        auto normalized = normalize_item(root["item"], 1, total_nodes, config_);
        source_roots.push_back({{"source_key", source_key}, {"virtual_root_id", normalized["item_id"]}, {"cleaned", false}});
        normalized_items.push_back({{"source_key", source_key}, {"item", std::move(normalized)}});
    }

    Statement counts(writer_,
        "SELECT (SELECT count(*) FROM cargo_roots i WHERE i.storage_id=c.storage_id),"
        "capacity_slots FROM storage_containers c WHERE c.storage_id=?1");
    counts.bind(1, storage_id); counts.row();
    if (counts.integer(0) + static_cast<std::int64_t>(normalized_items.size()) > counts.integer(1)) {
        throw ApiError(409, "storage_capacity", "The virtual container cannot hold this migration batch.");
    }

    const auto source_fingerprint = fingerprint(normalized_items.dump());
    Statement same(writer_,
        "SELECT migration_id FROM cargo_migrations WHERE storage_id=?1 AND source_fingerprint=?2");
    same.bind(1, storage_id); same.bind(2, source_fingerprint);
    if (same.row()) {
        auto result = find_migration(writer_, same.text(0));
        transaction.commit();
        return result;
    }

    const auto migration_id = random_hex(16);
    const auto now = now_unix_ms();
    Statement insert(writer_,
        "INSERT INTO cargo_migrations(migration_id,storage_id,container_class,status,source_fingerprint,"
        "expected_revision,source_roots_json,normalized_items_json,created_ms,updated_ms) "
        "VALUES(?1,?2,?3,'PREPARED',?4,?5,?6,?7,?8,?8)");
    insert.bind(1, migration_id); insert.bind(2, storage_id); insert.bind(3, container_class);
    insert.bind(4, source_fingerprint); insert.bind(5, expected); insert.bind(6, source_roots.dump());
    insert.bind(7, normalized_items.dump()); insert.bind(8, now); insert.done();
    Statement root_insert(writer_,
        "INSERT INTO cargo_migration_roots(storage_id,source_key,migration_id,virtual_root_id) "
        "VALUES(?1,?2,?3,?4)");
    for (const auto& root : source_roots) {
        root_insert.bind(1, storage_id); root_insert.bind(2, root["source_key"].get<std::string>());
        root_insert.bind(3, migration_id); root_insert.bind(4, root["virtual_root_id"].get<std::string>());
        root_insert.done();
        root_insert.reset();
    }
    audit(writer_, migration_id, "migration_prepared",
          {{"container_class", container_class}, {"roots", source_roots.size()}, {"nodes", total_nodes}});
    auto result = find_migration(writer_, migration_id);
    transaction.commit();
    return result;
}

json StorageDatabase::commit_migration(const json& request) {
    const auto migration_id = required_string(request, "migration_id", 64);
    std::lock_guard lock(writer_gate_);
    Transaction transaction(writer_);
    auto migration = find_migration(writer_, migration_id);
    const auto status = migration["status"].get<std::string>();
    if (status == "COMMITTED" || status == "CLEANED") {
        transaction.commit();
        return migration;
    }
    if (status != "PREPARED") throw ApiError(409, "invalid_migration_state", "The migration is not prepared.");

    const auto storage_id = migration["storage_id"].get<std::string>();
    const auto expected = migration["expected_revision"].get<std::int64_t>();
    assert_storage_workflow_available(writer_, storage_id, "cargo migration", migration_id);
    assert_revision(writer_, storage_id, expected);
    Statement payload(writer_, "SELECT normalized_items_json::text FROM cargo_migrations WHERE migration_id=?1");
    payload.bind(1, migration_id); payload.row();
    const auto items = json::parse(payload.text(0));
    const auto now = now_unix_ms();
    json migration_items = json::array();
    for (const auto& entry : items) migration_items.push_back(entry.at("item"));
    insert_trees_batch(writer_, storage_id, migration_items, now);

    Statement revision(writer_, "UPDATE storage_containers SET revision=revision+1,updated_ms=?2 WHERE storage_id=?1");
    revision.bind(1, storage_id); revision.bind(2, now); revision.done();
    Statement finish(writer_,
        "UPDATE cargo_migrations SET status='COMMITTED',result_revision=?2,updated_ms=?3 WHERE migration_id=?1");
    finish.bind(1, migration_id); finish.bind(2, expected + 1); finish.bind(3, now); finish.done();
    audit(writer_, migration_id, "migration_committed", {{"roots", items.size()}});
    migration = find_migration(writer_, migration_id);
    transaction.commit();
    return migration;
}

json StorageDatabase::acknowledge_migration_cleanup(const json& request) {
    const auto migration_id = required_string(request, "migration_id", 64);
    const auto cleaned_source_keys = required_source_keys(request, "cleaned_source_keys", 200);
    std::lock_guard lock(writer_gate_);
    Transaction transaction(writer_);
    auto migration = find_migration(writer_, migration_id);
    if (migration["status"] == "PREPARED") {
        throw ApiError(409, "invalid_migration_state", "Physical roots cannot be cleaned before the migration is committed.");
    }
    Statement update(writer_,
        "UPDATE cargo_migration_roots SET cleaned=1 "
        "WHERE migration_id=?1 AND source_key=?2 AND cleaned=0");
    Statement known(writer_,
        "SELECT 1 FROM cargo_migration_roots WHERE migration_id=?1 AND source_key=?2");
    for (const auto& value : cleaned_source_keys) {
        const auto key = value.get<std::string>();
        update.bind(1, migration_id); update.bind(2, key); update.done();
        if (update.affected_rows() == 0) {
            known.bind(1, migration_id); known.bind(2, key);
            if (!known.row()) {
                throw ApiError(400, "unknown_source_key", "A cleanup key does not belong to this migration.");
            }
            known.reset();
        }
        update.reset();
    }
    Statement remaining(writer_, "SELECT count(*) FROM cargo_migration_roots WHERE migration_id=?1 AND cleaned=0");
    remaining.bind(1, migration_id); remaining.row();
    if (remaining.integer(0) == 0 && migration["status"] != "CLEANED") {
        // Keep the compact source-root identity forever so a restored DayZ hive
        // cannot import a previously migrated physical root a second time.  The
        // normalized item trees are only needed before commit, so release that
        // potentially large payload once every physical cleanup is acknowledged.
        Statement finish(writer_,
            "UPDATE cargo_migrations SET status='CLEANED',normalized_items_json='[]'::jsonb,updated_ms=?2 "
            "WHERE migration_id=?1");
        finish.bind(1, migration_id); finish.bind(2, now_unix_ms()); finish.done();
        audit(writer_, migration_id, "migration_physical_cleanup_acknowledged");
    }
    migration = find_migration(writer_, migration_id);
    transaction.commit();
    return migration;
}

json StorageDatabase::incomplete_migrations(const json& request) {
    const auto provider_id = required_string(request, "provider_id", 128);
    const auto limit = std::clamp(request.value("limit", 500), 1, 5000);
    auto& slot = reader();
    std::lock_guard lock(slot.mutex);
    Transaction transaction(slot.connection, "BEGIN");
    json migrations = json::array();
    Statement query(slot.connection,
        "SELECT m.migration_id,m.storage_id,c.provider_key,m.container_class,m.status,m.source_fingerprint,"
        "m.expected_revision,m.source_roots_json::text,m.created_ms,m.updated_ms,m.result_revision,m.error,c.provider_id "
        "FROM cargo_migrations m JOIN storage_containers c ON c.storage_id=m.storage_id "
        "WHERE c.provider_id=?1 AND m.status IN ('PREPARED','COMMITTED') ORDER BY m.created_ms LIMIT ?2");
    query.bind(1, provider_id); query.bind(2, static_cast<std::int64_t>(limit));
    while (query.row()) {
        auto migration = migration_row(query);
        migration["provider_id"] = query.text(12);
        migrations.push_back(std::move(migration));
    }
    transaction.commit();
    return {{"provider_id", provider_id}, {"migrations", migrations}};
}

json StorageDatabase::observe_migration(const json& request) {
    const auto provider_id = required_string(request, "provider_id", 128);
    const auto provider_key = required_string(request, "provider_key", 512);
    const auto container_class = required_string(request, "container_class", 256);
    const auto status = required_string(request, "status", 64);
    const auto physical_roots = std::max<std::int64_t>(0, request.value("physical_roots", 0LL));
    const auto captured_roots = std::max<std::int64_t>(0, request.value("captured_roots", 0LL));
    const auto rejected_roots = std::max<std::int64_t>(0, request.value("rejected_roots", 0LL));
    const auto detail = request.value("detail", "").substr(0, 2048);
    std::lock_guard lock(writer_gate_);
    Statement upsert(writer_,
        "INSERT INTO cargo_migration_observations(provider_id,provider_key,container_class,status,physical_roots,"
        "captured_roots,rejected_roots,detail,updated_ms) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9) "
        "ON CONFLICT(provider_id,provider_key) DO UPDATE SET container_class=excluded.container_class,status=excluded.status,"
        "physical_roots=excluded.physical_roots,captured_roots=excluded.captured_roots,"
        "rejected_roots=excluded.rejected_roots,detail=excluded.detail,updated_ms=excluded.updated_ms");
    upsert.bind(1, provider_id); upsert.bind(2, provider_key); upsert.bind(3, container_class);
    upsert.bind(4, status); upsert.bind(5, physical_roots); upsert.bind(6, captured_roots);
    upsert.bind(7, rejected_roots); upsert.bind(8, detail); upsert.bind(9, now_unix_ms()); upsert.done();
    return {{"provider_id", provider_id}, {"provider_key", provider_key}, {"status", status}};
}

json StorageDatabase::health() {
    auto& slot = reader();
    std::lock_guard lock(slot.mutex);
    Transaction transaction(slot.connection, "BEGIN READ ONLY");
    Statement query(slot.connection,
        "SELECT EXISTS(SELECT 1 FROM schema_migrations WHERE version=?1),"
        "COALESCE((SELECT value FROM application_meta WHERE key='application'),'')");
    query.bind(1, static_cast<std::int64_t>(schema_version));
    if (!query.row()) throw std::runtime_error("PostgreSQL health query returned no row.");
    const bool healthy = query.text(0) == "t" && query.text(1) == "ClippyVirtualCargo";
    transaction.commit();
    return {{"healthy", healthy}, {"schema_version", schema_version},
            {"postgres_server_version", pool_->server_version_text()},
            {"postgres_library_version", postgres_library_version()},
            {"connection_pool_size", static_cast<std::int64_t>(pool_->size())}};
}

json StorageDatabase::quick_check() {
    auto& slot = reader();
    std::lock_guard lock(slot.mutex);
    Transaction transaction(slot.connection, "BEGIN READ ONLY");
    json checks = json::array();
    bool healthy = true;
    auto check_zero = [&](const char* name, const char* sql) {
        Statement query(slot.connection, sql);
        if (!query.row()) throw std::runtime_error(std::string("Integrity check returned no row: ") + name);
        const auto count = query.integer(0);
        checks.push_back({{"check", name}, {"errors", count}});
        if (count != 0) healthy = false;
    };
    check_zero("invalid_storage_revisions",
               "SELECT count(*) FROM storage_containers WHERE revision<0 OR capacity_slots<0");
    check_zero("invalid_root_payloads",
               "SELECT count(*) FROM cargo_roots WHERE node_count<=0 OR jsonb_typeof(tree_json)<>'object' "
               "OR jsonb_typeof(state_json)<>'object' OR jsonb_typeof(item_ids)<>'array'");
    check_zero("orphan_operation_cleanup",
               "SELECT count(*) FROM operation_cleanup_roots r LEFT JOIN operations o USING(operation_id) "
               "WHERE o.operation_id IS NULL");
    check_zero("orphan_session_cleanup",
               "SELECT count(*) FROM cargo_session_cleanup_roots r LEFT JOIN cargo_sessions s USING(session_id) "
               "WHERE s.session_id IS NULL");
    check_zero("orphan_migration_roots",
               "SELECT count(*) FROM cargo_migration_roots r LEFT JOIN cargo_migrations m USING(migration_id) "
               "WHERE m.migration_id IS NULL");
    check_zero("multiple_active_workflows",
               "SELECT count(*) FROM (SELECT storage_id FROM ("
               "SELECT storage_id FROM operations WHERE status IN ('PREPARED','QUARANTINED') OR cleanup_state='PENDING' "
               "UNION ALL SELECT storage_id FROM cargo_sessions WHERE status IN ('OPEN','MATERIALIZED','COMMITTED') "
               "UNION ALL SELECT storage_id FROM cargo_migrations WHERE status IN ('PREPARED','COMMITTED')"
               ") active GROUP BY storage_id HAVING count(*)>1) conflicts");
    Statement app(slot.connection,
        "SELECT count(*) FROM application_meta WHERE key='application' AND value='ClippyVirtualCargo'");
    if (!app.row() || app.integer(0) != 1) {
        healthy = false;
        checks.push_back({{"check", "application_marker"}, {"errors", 1}});
    } else {
        checks.push_back({{"check", "application_marker"}, {"errors", 0}});
    }
    transaction.commit();
    return {{"healthy", healthy}, {"checks", std::move(checks)},
            {"note", "PostgreSQL enforces foreign keys and checks on every write; this endpoint validates Clippy logical invariants."}};
}

json StorageDatabase::backup(const json&) {
    std::lock_guard backup_lock(backup_mutex_);
    const auto created = now_unix_ms();
    const auto filename = "ClippyVirtualCargo-" + std::to_string(created) + "-" + random_hex(4) + ".dump";
    const auto path = config_.backup_directory / filename;
    const auto started = std::chrono::steady_clock::now();
    try {
#ifdef _WIN32
        if (config_.postgres_bin_directory.empty()) {
            throw std::runtime_error("postgresBinDirectory is required for online PostgreSQL backups.");
        }
        const auto bin = std::filesystem::path(config_.postgres_bin_directory);
        const auto dump = bin / "pg_dump.exe";
        std::vector<std::wstring> dump_args = {
            L"--host", utf8_to_wide(config_.postgres_host),
            L"--port", std::to_wstring(config_.postgres_port),
            L"--username", utf8_to_wide(config_.postgres_user),
            L"--dbname", utf8_to_wide(config_.postgres_database),
            L"--format=custom", L"--compress=6", L"--no-owner", L"--no-privileges",
            L"--file", path.wstring()
        };
        if (run_postgres_tool(dump, dump_args, config_.postgres_password) != 0) {
            throw std::runtime_error("pg_dump failed while creating the PostgreSQL backup.");
        }
        const auto restore = bin / "pg_restore.exe";
        if (run_postgres_tool(restore, {L"--list", path.wstring()}, config_.postgres_password) != 0) {
            throw std::runtime_error("pg_restore could not validate the PostgreSQL backup archive.");
        }
#else
        throw std::runtime_error("Online backup is currently packaged for the Windows DayZ server build.");
#endif
        if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) == 0) {
            throw std::runtime_error("PostgreSQL backup archive is empty.");
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        throw;
    }
    const auto removed = prune_generated_backups(
        config_.backup_directory, static_cast<std::size_t>(config_.max_backup_files), path);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    return {{"path", path.string()}, {"created_ms", created},
            {"bytes", static_cast<std::int64_t>(std::filesystem::file_size(path))},
            {"duration_ms", elapsed}, {"expired_backups_removed", removed},
            {"max_backup_files", config_.max_backup_files}, {"format", "pg_dump custom"}};
}

json StorageDatabase::metrics(const json& request) {
    const auto provider_id = required_string(request, "provider_id", 128);
    auto& slot = reader();
    std::lock_guard lock(slot.mutex);
    Transaction transaction(slot.connection, "BEGIN");
    auto scalar = [&](const char* sql) {
        Statement query(slot.connection, sql);
        query.bind(1, provider_id); query.row(); return query.integer(0);
    };
    auto result = json{
        {"provider_id", provider_id},
        {"containers", scalar("SELECT count(*) FROM storage_containers WHERE provider_id=?1")},
        {"item_nodes", scalar("SELECT COALESCE(sum(i.node_count),0) FROM cargo_roots i JOIN storage_containers c "
                              "ON c.storage_id=i.storage_id WHERE c.provider_id=?1")},
        {"root_items", scalar("SELECT count(*) FROM cargo_roots i JOIN storage_containers c "
                              "ON c.storage_id=i.storage_id WHERE c.provider_id=?1")},
        {"incomplete_operations", scalar("SELECT count(*) FROM operations o JOIN storage_containers c "
                                          "ON c.storage_id=o.storage_id WHERE c.provider_id=?1 "
                                          "AND (o.status IN ('PREPARED','QUARANTINED') OR o.cleanup_state='PENDING')")},
        {"active_sessions", scalar("SELECT count(*) FROM cargo_sessions s JOIN storage_containers c "
                                   "ON c.storage_id=s.storage_id WHERE c.provider_id=?1 "
                                   "AND s.status IN ('OPEN','MATERIALIZED','COMMITTED')")},
        {"incomplete_migrations", scalar("SELECT count(*) FROM cargo_migrations m JOIN storage_containers c "
                                         "ON c.storage_id=m.storage_id WHERE c.provider_id=?1 "
                                         "AND m.status IN ('PREPARED','COMMITTED')")},
        {"migrated_roots", scalar("SELECT count(*) FROM cargo_migration_roots r JOIN cargo_migrations m "
                                  "ON m.migration_id=r.migration_id JOIN storage_containers c "
                                  "ON c.storage_id=m.storage_id WHERE c.provider_id=?1")},
        {"database_bytes", [&] {
            Statement size(slot.connection, "SELECT pg_database_size(current_database())");
            if (!size.row()) return std::int64_t{0};
            return size.integer(0);
        }()},
    };
    transaction.commit();
    return result;
}

void StorageDatabase::checkpoint() {
    // PostgreSQL owns WAL checkpoint scheduling. Synchronous commits already make
    // successful Clippy transactions crash-durable, so the host does not force a
    // cluster-wide CHECKPOINT on request or shutdown.
}

void StorageDatabase::optimize() {
    // PostgreSQL autovacuum and auto-analyze maintain tables and indexes. Clippy
    // only prunes old terminal workflow rows here to keep hot indexes compact.
    prune_terminal_history();
}

void StorageDatabase::prune_terminal_history() {
    const auto cutoff = now_unix_ms() -
        static_cast<std::int64_t>(config_.terminal_retention_days) * 24 * 60 * 60 * 1000;
    const auto batch = static_cast<std::int64_t>(config_.maintenance_prune_batch_rows);
    std::lock_guard lock(writer_gate_);
    Transaction transaction(writer_);

    Statement audit_delete(writer_, R"SQL(
DELETE FROM audit_events WHERE event_id IN (
    SELECT a.event_id FROM audit_events a
    WHERE a.created_ms<?1 AND (
        EXISTS(SELECT 1 FROM operations o WHERE o.operation_id=a.operation_id
               AND o.updated_ms<?1 AND o.cleanup_state<>'PENDING'
               AND o.status IN ('CLEANED','ABORTED'))
        OR EXISTS(SELECT 1 FROM cargo_sessions s WHERE s.session_id=a.operation_id
                  AND s.updated_ms<?1 AND s.status IN ('CLEANED','ABORTED'))
        OR EXISTS(SELECT 1 FROM cargo_migrations m WHERE m.migration_id=a.operation_id
                  AND m.updated_ms<?1 AND m.status='CLEANED')
        OR (NOT EXISTS(SELECT 1 FROM operations o WHERE o.operation_id=a.operation_id)
            AND NOT EXISTS(SELECT 1 FROM cargo_sessions s WHERE s.session_id=a.operation_id)
            AND NOT EXISTS(SELECT 1 FROM cargo_migrations m WHERE m.migration_id=a.operation_id))
    ) ORDER BY a.event_id LIMIT ?2
))SQL");
    audit_delete.bind(1, cutoff); audit_delete.bind(2, batch); audit_delete.done();

    Statement operation_delete(writer_, R"SQL(
DELETE FROM operations WHERE operation_id IN (
    SELECT operation_id FROM operations
    WHERE updated_ms<?1 AND cleanup_state<>'PENDING' AND status IN ('CLEANED','ABORTED')
    ORDER BY updated_ms,operation_id LIMIT ?2
))SQL");
    operation_delete.bind(1, cutoff); operation_delete.bind(2, batch); operation_delete.done();

    Statement session_delete(writer_, R"SQL(
DELETE FROM cargo_sessions WHERE session_id IN (
    SELECT session_id FROM cargo_sessions
    WHERE updated_ms<?1 AND status IN ('CLEANED','ABORTED')
    ORDER BY updated_ms,session_id LIMIT ?2
))SQL");
    session_delete.bind(1, cutoff); session_delete.bind(2, batch); session_delete.done();

    // cargo_migrations and cargo_migration_roots are permanent dedupe
    // tombstones. Deleting them could let a rolled-back DayZ hive resurrect and
    // re-import physical roots that PostgreSQL has already committed.
    transaction.commit();
}

} // namespace clippy
