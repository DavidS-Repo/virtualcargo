#include "database.hpp"

#include "sqlite3.h"
#include "util.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <functional>
#include <memory>
#include <regex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace clippy {
namespace {

using nlohmann::json;
constexpr int legacy_application_id = 1129726769;
constexpr int legacy_schema_version = 6;

struct SqliteCloser {
    void operator()(sqlite3* value) const noexcept { if (value) sqlite3_close(value); }
};
using SqlitePtr = std::unique_ptr<sqlite3, SqliteCloser>;

[[noreturn]] void sqlite_fail(sqlite3* db, const std::string& context, int code = SQLITE_ERROR) {
    const char* message = db ? sqlite3_errmsg(db) : nullptr;
    throw std::runtime_error(context + ": " + (message ? message : "unknown SQLite error") +
                             " (code " + std::to_string(code) + ")");
}

void sqlite_exec(sqlite3* db, const char* sql) {
    char* message = nullptr;
    const int result = sqlite3_exec(db, sql, nullptr, nullptr, &message);
    if (result != SQLITE_OK) {
        const std::string detail = message ? message : sqlite3_errmsg(db);
        if (message) sqlite3_free(message);
        throw std::runtime_error("Legacy SQLite migration SQL failed: " + detail);
    }
}

std::int64_t sqlite_scalar_integer(sqlite3* db, const char* sql) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &raw, nullptr) != SQLITE_OK) sqlite_fail(db, "Could not prepare legacy SQLite query");
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
    const int result = sqlite3_step(statement.get());
    if (result != SQLITE_ROW) sqlite_fail(db, "Legacy SQLite scalar query returned no row", result);
    return sqlite3_column_int64(statement.get(), 0);
}

bool sqlite_table_exists(sqlite3* db, const char* name) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_schema WHERE type='table' AND name=?1", -1, &raw, nullptr) != SQLITE_OK) {
        sqlite_fail(db, "Could not inspect legacy SQLite schema");
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
    sqlite3_bind_text(statement.get(), 1, name, -1, SQLITE_STATIC);
    return sqlite3_step(statement.get()) == SQLITE_ROW;
}

void upgrade_legacy_schema(sqlite3* db) {
    const auto current_version = sqlite_scalar_integer(db, "PRAGMA user_version");
    const auto current_application = sqlite_scalar_integer(db, "PRAGMA application_id");
    if (current_version <= 0 || current_version > legacy_schema_version) {
        throw std::runtime_error("Legacy SQLite schema version is not supported: " + std::to_string(current_version));
    }
    if (current_application != 0 && current_application != legacy_application_id) {
        throw std::runtime_error("The detected SQLite database belongs to another application.");
    }
    if (!sqlite_table_exists(db, "storage_containers") || !sqlite_table_exists(db, "items")) {
        throw std::runtime_error("The detected SQLite file is not a Clippy Virtual Cargo database.");
    }

    sqlite_exec(db, "BEGIN IMMEDIATE");
    try {
        if (current_version < legacy_schema_version) {
            sqlite_exec(db, R"SQL(
DROP INDEX IF EXISTS items_root;
DROP INDEX IF EXISTS operations_incomplete;
DROP INDEX IF EXISTS operations_active_storage;
DROP INDEX IF EXISTS cargo_sessions_one_active_storage;
DROP INDEX IF EXISTS cargo_sessions_incomplete;
DROP INDEX IF EXISTS cargo_migrations_incomplete;
DROP INDEX IF EXISTS cargo_migration_observations_recent;
)SQL");
        }
        if (current_version < 5 && sqlite_table_exists(db, "cargo_sessions")) {
            sqlite_exec(db, R"SQL(
DROP TABLE IF EXISTS cargo_session_cleanup_roots;
ALTER TABLE cargo_sessions RENAME TO cargo_sessions_v4;
CREATE TABLE cargo_sessions(
    session_id TEXT PRIMARY KEY,
    storage_id TEXT NOT NULL REFERENCES storage_containers(storage_id),
    idempotency_key TEXT NOT NULL UNIQUE,
    player_id TEXT NOT NULL,
    status TEXT NOT NULL CHECK(status IN ('OPEN','MATERIALIZED','COMMITTED','CLEANED','ABORTED')),
    expected_revision INTEGER NOT NULL,
    cursor TEXT NOT NULL,
    next_cursor TEXT NOT NULL,
    original_root_ids_json TEXT NOT NULL CHECK(json_valid(original_root_ids_json)),
    physical_source_keys_json TEXT NOT NULL DEFAULT '[]' CHECK(json_valid(physical_source_keys_json)),
    created_ms INTEGER NOT NULL,
    updated_ms INTEGER NOT NULL,
    result_revision INTEGER,
    error TEXT
) STRICT;
INSERT INTO cargo_sessions(session_id,storage_id,idempotency_key,player_id,status,expected_revision,cursor,next_cursor,
    original_root_ids_json,physical_source_keys_json,created_ms,updated_ms,result_revision,error)
SELECT session_id,storage_id,idempotency_key,player_id,
    CASE WHEN status='COMMITTED' THEN 'CLEANED' ELSE status END,
    expected_revision,cursor,next_cursor,original_root_ids_json,'[]',created_ms,updated_ms,result_revision,error
FROM cargo_sessions_v4;
DROP TABLE cargo_sessions_v4;
)SQL");
        }
        if (current_version < 6 && sqlite_table_exists(db, "operations")) {
            sqlite_exec(db, R"SQL(
DROP TABLE IF EXISTS operation_cleanup_roots;
ALTER TABLE operations RENAME TO operations_v5;
CREATE TABLE operations(
    operation_id TEXT PRIMARY KEY,
    idempotency_key TEXT NOT NULL UNIQUE,
    request_fingerprint TEXT NOT NULL,
    kind TEXT NOT NULL CHECK(kind IN ('deposit','withdraw')),
    status TEXT NOT NULL CHECK(status IN ('PREPARED','QUARANTINED','COMMITTED','CLEANED','ABORTED')),
    cleanup_state TEXT NOT NULL CHECK(cleanup_state IN ('NONE','PENDING','CLEANED')),
    physical_source_key TEXT,
    storage_id TEXT NOT NULL REFERENCES storage_containers(storage_id),
    root_item_id TEXT NOT NULL,
    expected_revision INTEGER NOT NULL,
    payload_json TEXT CHECK(payload_json IS NULL OR json_valid(payload_json)),
    created_ms INTEGER NOT NULL,
    updated_ms INTEGER NOT NULL,
    error TEXT,
    result_revision INTEGER
) STRICT;
INSERT INTO operations(operation_id,idempotency_key,request_fingerprint,kind,status,cleanup_state,
    physical_source_key,storage_id,root_item_id,expected_revision,payload_json,created_ms,updated_ms,error,result_revision)
SELECT operation_id,idempotency_key,request_fingerprint,kind,
    CASE WHEN status='COMMITTED' THEN 'CLEANED' ELSE status END,
    CASE WHEN status IN ('COMMITTED','ABORTED') THEN 'CLEANED' ELSE 'NONE' END,
    NULL,storage_id,root_item_id,expected_revision,payload_json,created_ms,updated_ms,error,
    CASE WHEN status='COMMITTED' THEN expected_revision+1 ELSE NULL END
FROM operations_v5;
DROP TABLE operations_v5;
)SQL");
        }
        if (current_version < 5 && sqlite_table_exists(db, "cargo_migration_observations")) {
            sqlite_exec(db, "DROP TABLE cargo_migration_observations");
        }

        sqlite_exec(db, R"SQL(
CREATE TABLE IF NOT EXISTS schema_migrations(version INTEGER PRIMARY KEY, applied_ms INTEGER NOT NULL) STRICT;
CREATE TABLE IF NOT EXISTS operation_cleanup_roots(
    operation_id TEXT NOT NULL REFERENCES operations(operation_id) ON DELETE CASCADE,
    source_key TEXT NOT NULL,
    cleanup_action TEXT NOT NULL CHECK(cleanup_action IN ('DELETE_SOURCE','RELEASE_SOURCE','RELEASE_TARGET','DELETE_TARGET')),
    cleaned INTEGER NOT NULL DEFAULT 0 CHECK(cleaned IN (0,1)),
    PRIMARY KEY(operation_id,source_key)
) STRICT;
CREATE INDEX IF NOT EXISTS operation_cleanup_pending ON operation_cleanup_roots(operation_id,cleaned);
CREATE TABLE IF NOT EXISTS cargo_session_cleanup_roots(
    session_id TEXT NOT NULL REFERENCES cargo_sessions(session_id) ON DELETE CASCADE,
    source_key TEXT NOT NULL,
    cleaned INTEGER NOT NULL DEFAULT 0 CHECK(cleaned IN (0,1)),
    PRIMARY KEY(session_id,source_key)
) STRICT;
CREATE INDEX IF NOT EXISTS cargo_session_cleanup_pending ON cargo_session_cleanup_roots(session_id,cleaned);
CREATE INDEX IF NOT EXISTS items_storage_roots ON items(storage_id,parent_item_id,item_id);
CREATE INDEX IF NOT EXISTS items_root ON items(storage_id,root_item_id,ordinal,item_id);
CREATE INDEX IF NOT EXISTS operations_incomplete ON operations(created_ms)
    WHERE status IN ('PREPARED','QUARANTINED') OR cleanup_state='PENDING';
CREATE INDEX IF NOT EXISTS operations_active_storage ON operations(storage_id)
    WHERE status IN ('PREPARED','QUARANTINED') OR cleanup_state='PENDING';
CREATE INDEX IF NOT EXISTS operations_terminal_retention ON operations(updated_ms,operation_id)
    WHERE cleanup_state<>'PENDING' AND status IN ('CLEANED','ABORTED');
CREATE UNIQUE INDEX IF NOT EXISTS cargo_sessions_one_active_storage
    ON cargo_sessions(storage_id) WHERE status IN ('OPEN','MATERIALIZED','COMMITTED');
CREATE INDEX IF NOT EXISTS cargo_sessions_incomplete ON cargo_sessions(created_ms)
    WHERE status IN ('OPEN','MATERIALIZED','COMMITTED');
CREATE INDEX IF NOT EXISTS cargo_sessions_terminal_retention ON cargo_sessions(updated_ms,session_id)
    WHERE status IN ('CLEANED','ABORTED');
CREATE UNIQUE INDEX IF NOT EXISTS cargo_migrations_one_active_storage
    ON cargo_migrations(storage_id) WHERE status IN ('PREPARED','COMMITTED');
CREATE INDEX IF NOT EXISTS cargo_migrations_incomplete ON cargo_migrations(created_ms)
    WHERE status IN ('PREPARED','COMMITTED');
CREATE INDEX IF NOT EXISTS cargo_migrations_terminal_retention ON cargo_migrations(updated_ms,migration_id)
    WHERE status='CLEANED';
CREATE INDEX IF NOT EXISTS cargo_migration_roots_migration ON cargo_migration_roots(migration_id,cleaned);
CREATE INDEX IF NOT EXISTS cargo_migration_roots_lookup ON cargo_migration_roots(migration_id,source_key);
CREATE TABLE IF NOT EXISTS cargo_migration_observations(
    provider_id TEXT NOT NULL,
    provider_key TEXT NOT NULL,
    container_class TEXT NOT NULL,
    status TEXT NOT NULL,
    physical_roots INTEGER NOT NULL,
    captured_roots INTEGER NOT NULL,
    rejected_roots INTEGER NOT NULL,
    detail TEXT NOT NULL,
    updated_ms INTEGER NOT NULL,
    PRIMARY KEY(provider_id,provider_key)
) STRICT;
CREATE INDEX IF NOT EXISTS cargo_migration_observations_recent
    ON cargo_migration_observations(provider_id,updated_ms DESC);
CREATE INDEX IF NOT EXISTS audit_events_retention ON audit_events(created_ms,event_id);
)SQL");
        sqlite_exec(db, "INSERT OR IGNORE INTO schema_migrations(version,applied_ms) VALUES(6,0)");
        sqlite_exec(db, "PRAGMA application_id=1129726769");
        sqlite_exec(db, "PRAGMA user_version=6");
        sqlite_exec(db, "COMMIT");
    } catch (...) {
        try { sqlite_exec(db, "ROLLBACK"); } catch (...) {}
        throw;
    }
}

void validate_legacy(sqlite3* db) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA quick_check(100)", -1, &raw, nullptr) != SQLITE_OK) {
        sqlite_fail(db, "Could not prepare legacy SQLite quick_check");
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> check(raw, sqlite3_finalize);
    int rows = 0;
    while (sqlite3_step(check.get()) == SQLITE_ROW) {
        ++rows;
        const char* text = reinterpret_cast<const char*>(sqlite3_column_text(check.get(), 0));
        if (!text || std::string(text) != "ok") throw std::runtime_error("Legacy SQLite quick_check failed.");
    }
    if (rows != 1) throw std::runtime_error("Legacy SQLite quick_check did not return exactly one ok row.");

    raw = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA foreign_key_check", -1, &raw, nullptr) != SQLITE_OK) {
        sqlite_fail(db, "Could not prepare legacy SQLite foreign_key_check");
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> fk(raw, sqlite3_finalize);
    if (sqlite3_step(fk.get()) == SQLITE_ROW) throw std::runtime_error("Legacy SQLite database has foreign-key errors.");
}


void create_consistent_sqlite_snapshot(const std::filesystem::path& source_path,
                                       const std::filesystem::path& destination_path) {
    sqlite3* source_raw = nullptr;
    const auto source_utf8 = source_path.u8string();
    const int source_result = sqlite3_open_v2(reinterpret_cast<const char*>(source_utf8.c_str()), &source_raw,
                                               SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
    SqlitePtr source(source_raw);
    if (source_result != SQLITE_OK || !source)
        sqlite_fail(source_raw, "Could not open legacy SQLite source for a consistent snapshot", source_result);
    sqlite3_busy_timeout(source.get(), 30000);

    sqlite3* destination_raw = nullptr;
    const auto destination_utf8 = destination_path.u8string();
    const int destination_result = sqlite3_open_v2(reinterpret_cast<const char*>(destination_utf8.c_str()), &destination_raw,
                                                    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    SqlitePtr destination(destination_raw);
    if (destination_result != SQLITE_OK || !destination)
        sqlite_fail(destination_raw, "Could not create legacy SQLite safety snapshot", destination_result);

    sqlite3_backup* backup = sqlite3_backup_init(destination.get(), "main", source.get(), "main");
    if (!backup) sqlite_fail(destination.get(), "Could not initialize legacy SQLite online backup");
    int result = SQLITE_OK;
    do {
        result = sqlite3_backup_step(backup, 1024);
        if (result == SQLITE_BUSY || result == SQLITE_LOCKED) sqlite3_sleep(25);
    } while (result == SQLITE_OK || result == SQLITE_BUSY || result == SQLITE_LOCKED);
    const int finish_result = sqlite3_backup_finish(backup);
    if (result != SQLITE_DONE || finish_result != SQLITE_OK)
        sqlite_fail(destination.get(), "Could not finish legacy SQLite safety snapshot",
                    finish_result != SQLITE_OK ? finish_result : result);
}

void bind_sqlite_value(Statement& destination, int parameter, sqlite3_stmt* source, int column) {
    switch (sqlite3_column_type(source, column)) {
        case SQLITE_NULL: destination.bind_null(parameter); break;
        case SQLITE_INTEGER: destination.bind(parameter, static_cast<std::int64_t>(sqlite3_column_int64(source, column))); break;
        case SQLITE_FLOAT: {
            const double value = sqlite3_column_double(source, column);
            if (!std::isfinite(value)) throw std::runtime_error("Legacy SQLite contains a non-finite numeric value.");
            destination.bind(parameter, value);
            break;
        }
        case SQLITE_TEXT: {
            const auto* bytes = reinterpret_cast<const char*>(sqlite3_column_text(source, column));
            const int length = sqlite3_column_bytes(source, column);
            destination.bind(parameter, std::string(bytes ? bytes : "", static_cast<std::size_t>((std::max)(0, length))));
            break;
        }
        default: throw std::runtime_error("Legacy SQLite contains an unsupported BLOB value.");
    }
}

std::int64_t copy_rows(sqlite3* source, PgPool* destination,
                       const char* select_sql, const char* insert_sql) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(source, select_sql, -1, &raw, nullptr) != SQLITE_OK) {
        sqlite_fail(source, "Could not prepare legacy SQLite copy query");
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> query(raw, sqlite3_finalize);
    const int columns = sqlite3_column_count(query.get());
    Statement insert(destination, insert_sql);
    std::int64_t copied = 0;
    for (;;) {
        const int step = sqlite3_step(query.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) sqlite_fail(source, "Could not read legacy SQLite row", step);
        for (int column = 0; column < columns; ++column) {
            bind_sqlite_value(insert, column + 1, query.get(), column);
        }
        insert.done();
        insert.reset();
        ++copied;
    }
    return copied;
}

json legacy_tree(sqlite3* source, const std::string& storage_id, const std::string& root_id,
                 json& item_ids, std::int64_t& created_ms) {
    sqlite3_stmt* raw = nullptr;
    const char* sql =
        "SELECT item_id,parent_item_id,ordinal,class_name,quantity,health,location_json,adapter_id,"
        "adapter_version,state_json,created_ms FROM items WHERE storage_id=?1 AND root_item_id=?2 "
        "ORDER BY ordinal,item_id";
    if (sqlite3_prepare_v2(source, sql, -1, &raw, nullptr) != SQLITE_OK) {
        sqlite_fail(source, "Could not prepare legacy item-tree query");
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> query(raw, sqlite3_finalize);
    sqlite3_bind_text(query.get(), 1, storage_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(query.get(), 2, root_id.c_str(), -1, SQLITE_TRANSIENT);

    std::unordered_map<std::string, json> nodes;
    std::unordered_map<std::string, std::vector<std::pair<std::int64_t,std::string>>> children;
    created_ms = 0;
    for (;;) {
        const int step = sqlite3_step(query.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) sqlite_fail(source, "Could not read legacy item tree", step);
        const auto text_at = [&](int column) {
            const auto* value = reinterpret_cast<const char*>(sqlite3_column_text(query.get(), column));
            return std::string(value ? value : "");
        };
        const auto id = text_at(0);
        const bool parent_null = sqlite3_column_type(query.get(), 1) == SQLITE_NULL;
        const auto parent = parent_null ? std::string{} : text_at(1);
        const auto ordinal = sqlite3_column_int64(query.get(), 2);
        const auto quantity = sqlite3_column_double(query.get(), 4);
        const auto health = sqlite3_column_double(query.get(), 5);
        if (!std::isfinite(quantity) || !std::isfinite(health)) {
            throw std::runtime_error("Legacy item tree contains a non-finite quantity or health value.");
        }
        json node = {
            {"item_id", id}, {"class_name", text_at(3)}, {"quantity", quantity}, {"health", health},
            {"location", json::parse(text_at(6))},
            {"adapter", {{"id", text_at(7)}, {"version", sqlite3_column_int64(query.get(), 8)}}},
            {"state", json::parse(text_at(9))}, {"children", json::array()}
        };
        if (!nodes.emplace(id, std::move(node)).second) throw std::runtime_error("Legacy item tree contains duplicate item ids.");
        item_ids.push_back(id);
        const auto node_created = sqlite3_column_int64(query.get(), 10);
        if (id == root_id) created_ms = node_created;
        if (!parent.empty()) children[parent].push_back({ordinal,id});
    }
    if (!nodes.contains(root_id)) throw std::runtime_error("Legacy item tree root is missing.");
    for (auto& [_, list] : children) {
        std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
            return a.first != b.first ? a.first < b.first : a.second < b.second;
        });
    }
    std::unordered_set<std::string> visiting;
    std::unordered_set<std::string> built;
    std::function<json(const std::string&)> build = [&](const std::string& id) -> json {
        auto found = nodes.find(id);
        if (found == nodes.end()) throw std::runtime_error("Legacy item tree references a missing child.");
        if (!visiting.insert(id).second) throw std::runtime_error("Legacy item tree contains a cycle.");
        if (!built.insert(id).second) throw std::runtime_error("Legacy item tree contains a repeated child.");
        json result = found->second;
        if (auto list = children.find(id); list != children.end()) {
            for (const auto& [_, child] : list->second) result["children"].push_back(build(child));
        }
        visiting.erase(id);
        return result;
    };
    auto result = build(root_id);
    if (built.size() != nodes.size()) throw std::runtime_error("Legacy item tree contains disconnected nodes.");
    return result;
}

std::int64_t copy_item_roots(sqlite3* source, PgPool* destination, std::int64_t& node_count) {
    sqlite3_stmt* raw = nullptr;
    const char* sql = "SELECT storage_id,item_id FROM items WHERE parent_item_id IS NULL ORDER BY storage_id,item_id";
    if (sqlite3_prepare_v2(source, sql, -1, &raw, nullptr) != SQLITE_OK) {
        sqlite_fail(source, "Could not enumerate legacy root items");
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> roots(raw, sqlite3_finalize);
    Statement insert(destination,
        "INSERT INTO cargo_roots(storage_id,root_item_id,class_name,quantity,health,state_json,tree_json,item_ids,node_count,created_ms) "
        "VALUES(?1,?2,?3,?4,?5,?6::jsonb,?7::jsonb,?8::jsonb,?9,?10)");
    std::int64_t root_count = 0;
    node_count = 0;
    for (;;) {
        const int step = sqlite3_step(roots.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) sqlite_fail(source, "Could not read legacy root item", step);
        const auto* storage_bytes = reinterpret_cast<const char*>(sqlite3_column_text(roots.get(), 0));
        const auto* root_bytes = reinterpret_cast<const char*>(sqlite3_column_text(roots.get(), 1));
        const std::string storage_id = storage_bytes ? storage_bytes : "";
        const std::string root_id = root_bytes ? root_bytes : "";
        json ids = json::array();
        std::int64_t created = 0;
        auto tree = legacy_tree(source, storage_id, root_id, ids, created);
        const auto nodes = static_cast<std::int64_t>(ids.size());
        insert.bind(1, storage_id); insert.bind(2, root_id); insert.bind(3, tree.at("class_name").get<std::string>());
        insert.bind(4, tree.at("quantity").get<double>()); insert.bind(5, tree.at("health").get<double>());
        insert.bind(6, tree.at("state").dump()); insert.bind(7, tree.dump()); insert.bind(8, ids.dump());
        insert.bind(9, nodes); insert.bind(10, created); insert.done(); insert.reset();
        ++root_count;
        node_count += nodes;
    }
    return root_count;
}

} // namespace

nlohmann::json StorageDatabase::migrate_legacy_sqlite(const std::filesystem::path& legacy_database,
                                                       const std::string& expected_fingerprint) {
    static const std::regex sha256("^[0-9a-fA-F]{64}$");
    if (!expected_fingerprint.empty() && !std::regex_match(expected_fingerprint, sha256)) {
        throw std::runtime_error("Expected legacy source fingerprint must be a 64-character SHA-256 hex value.");
    }
    const auto source_path = std::filesystem::absolute(legacy_database).lexically_normal();
    if (!std::filesystem::is_regular_file(source_path)) {
        throw std::runtime_error("Legacy SQLite database does not exist: " + source_path.string());
    }

    std::lock_guard connection(writer_gate_);
    std::filesystem::create_directories(config_.backup_directory);
    const auto imported_ms = now_unix_ms();
    auto safety_path = config_.backup_directory /
        ("LegacySQLite-before-PostgreSQL-" + std::to_string(imported_ms) + ".db");
    create_consistent_sqlite_snapshot(source_path, safety_path);
    const auto source_fingerprint = fingerprint_file(safety_path);

    if (!expected_fingerprint.empty()) {
        std::string normalized = expected_fingerprint;
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                       [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        if (normalized != source_fingerprint) {
            std::error_code remove_error;
            std::filesystem::remove(safety_path, remove_error);
            throw std::runtime_error("The consistent SQLite snapshot does not match the expected SHA-256 fingerprint.");
        }
    }

    {
        Statement prior(writer_, "SELECT source_path,imported_ms FROM legacy_imports WHERE source_fingerprint=?1");
        prior.bind(1, source_fingerprint);
        if (prior.row()) {
            std::error_code remove_error;
            std::filesystem::remove(safety_path, remove_error);
            return {{"status", "already_imported"}, {"source_fingerprint", source_fingerprint},
                    {"source_path", prior.text(0)}, {"imported_ms", prior.integer(1)}};
        }
    }

    Statement destination_rows(writer_,
        "SELECT (SELECT count(*) FROM storage_containers)+(SELECT count(*) FROM cargo_roots)+"
        "(SELECT count(*) FROM operations)+(SELECT count(*) FROM cargo_sessions)+(SELECT count(*) FROM cargo_migrations)");
    if (!destination_rows.row() || destination_rows.integer(0) != 0) {
        std::error_code remove_error;
        std::filesystem::remove(safety_path, remove_error);
        throw std::runtime_error("PostgreSQL Clippy database is not empty and this SQLite snapshot was not imported before. Automatic merge is blocked to prevent duplicate cargo.");
    }

    const auto final_safety_path = config_.backup_directory /
        ("LegacySQLite-before-PostgreSQL-" + std::to_string(imported_ms) + "-" + source_fingerprint.substr(0, 8) + ".db");
    if (final_safety_path != safety_path) {
        std::error_code rename_error;
        std::filesystem::rename(safety_path, final_safety_path, rename_error);
        if (rename_error) throw std::runtime_error("Could not finalize the legacy SQLite safety snapshot: " + rename_error.message());
        safety_path = final_safety_path;
    }

    sqlite3* raw = nullptr;
    const auto utf8 = safety_path.u8string();
    const int open_result = sqlite3_open_v2(reinterpret_cast<const char*>(utf8.c_str()), &raw,
                                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr);
    SqlitePtr source(raw);
    if (open_result != SQLITE_OK || !source) sqlite_fail(raw, "Could not open the legacy SQLite safety copy", open_result);
    sqlite3_extended_result_codes(source.get(), 1);
    sqlite3_busy_timeout(source.get(), 30000);
    sqlite_exec(source.get(), "PRAGMA foreign_keys=ON; PRAGMA trusted_schema=OFF;");
    upgrade_legacy_schema(source.get());
    validate_legacy(source.get());

    sqlite_exec(source.get(), "BEGIN");
    Transaction transaction(writer_);
    try {
        const auto containers = copy_rows(source.get(), writer_,
            "SELECT storage_id,provider_id,provider_key,display_name,capacity_slots,revision,created_ms,updated_ms FROM storage_containers ORDER BY storage_id",
            "INSERT INTO storage_containers(storage_id,provider_id,provider_key,display_name,capacity_slots,revision,created_ms,updated_ms) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8)");

        std::int64_t item_nodes = 0;
        const auto roots = copy_item_roots(source.get(), writer_, item_nodes);

        const auto operations = copy_rows(source.get(), writer_,
            "SELECT operation_id,idempotency_key,request_fingerprint,kind,status,cleanup_state,physical_source_key,storage_id,root_item_id,expected_revision,payload_json,created_ms,updated_ms,error,result_revision FROM operations ORDER BY operation_id",
            "INSERT INTO operations(operation_id,idempotency_key,request_fingerprint,kind,status,cleanup_state,physical_source_key,storage_id,root_item_id,expected_revision,payload_json,created_ms,updated_ms,error,result_revision) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15)");
        const auto operation_cleanup = copy_rows(source.get(), writer_,
            "SELECT operation_id,source_key,cleanup_action,cleaned FROM operation_cleanup_roots ORDER BY operation_id,source_key",
            "INSERT INTO operation_cleanup_roots(operation_id,source_key,cleanup_action,cleaned) VALUES(?1,?2,?3,?4)");
        const auto sessions = copy_rows(source.get(), writer_,
            "SELECT session_id,storage_id,idempotency_key,player_id,status,expected_revision,cursor,next_cursor,original_root_ids_json,physical_source_keys_json,created_ms,updated_ms,result_revision,error FROM cargo_sessions ORDER BY session_id",
            "INSERT INTO cargo_sessions(session_id,storage_id,idempotency_key,player_id,status,expected_revision,cursor,next_cursor,original_root_ids_json,physical_source_keys_json,created_ms,updated_ms,result_revision,error) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14)");
        const auto session_cleanup = copy_rows(source.get(), writer_,
            "SELECT session_id,source_key,cleaned FROM cargo_session_cleanup_roots ORDER BY session_id,source_key",
            "INSERT INTO cargo_session_cleanup_roots(session_id,source_key,cleaned) VALUES(?1,?2,?3)");
        const auto migrations = copy_rows(source.get(), writer_,
            "SELECT migration_id,storage_id,container_class,status,source_fingerprint,expected_revision,source_roots_json,normalized_items_json,created_ms,updated_ms,result_revision,error FROM cargo_migrations ORDER BY migration_id",
            "INSERT INTO cargo_migrations(migration_id,storage_id,container_class,status,source_fingerprint,expected_revision,source_roots_json,normalized_items_json,created_ms,updated_ms,result_revision,error) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12)");
        const auto migration_roots = copy_rows(source.get(), writer_,
            "SELECT storage_id,source_key,migration_id,virtual_root_id,cleaned FROM cargo_migration_roots ORDER BY storage_id,source_key",
            "INSERT INTO cargo_migration_roots(storage_id,source_key,migration_id,virtual_root_id,cleaned) VALUES(?1,?2,?3,?4,?5)");
        const auto observations = copy_rows(source.get(), writer_,
            "SELECT provider_id,provider_key,container_class,status,physical_roots,captured_roots,rejected_roots,detail,updated_ms FROM cargo_migration_observations ORDER BY provider_id,provider_key",
            "INSERT INTO cargo_migration_observations(provider_id,provider_key,container_class,status,physical_roots,captured_roots,rejected_roots,detail,updated_ms) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)");
        const auto audit = copy_rows(source.get(), writer_,
            "SELECT operation_id,event_type,detail_json,created_ms FROM audit_events ORDER BY event_id",
            "INSERT INTO audit_events(operation_id,event_type,detail_json,created_ms) VALUES(?1,?2,?3,?4)");

        Statement mark(writer_, "INSERT INTO legacy_imports(source_fingerprint,source_path,imported_ms) VALUES(?1,?2,?3)");
        mark.bind(1, source_fingerprint); mark.bind(2, source_path.string()); mark.bind(3, imported_ms); mark.done();
        transaction.commit();
        sqlite_exec(source.get(), "COMMIT");
        return {{"status", "imported"}, {"source_fingerprint", source_fingerprint},
                {"source_path", source_path.string()}, {"safety_copy", safety_path.string()},
                {"imported_ms", imported_ms}, {"containers", containers}, {"root_items", roots},
                {"item_nodes", item_nodes}, {"operations", operations},
                {"operation_cleanup_roots", operation_cleanup}, {"sessions", sessions},
                {"session_cleanup_roots", session_cleanup}, {"migrations", migrations},
                {"migration_roots", migration_roots}, {"observations", observations}, {"audit_events", audit}};
    } catch (...) {
        try { sqlite_exec(source.get(), "ROLLBACK"); } catch (...) {}
        throw;
    }
}

} // namespace clippy
