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

constexpr int schema_version = 11;

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
    Statement expired_lock(database, "DELETE FROM admin_container_locks WHERE storage_id=?1 AND expires_ms<=?2");
    expired_lock.bind(1, storage_id);
    expired_lock.bind(2, now_unix_ms());
    expired_lock.done();
    Statement maintenance(database,
        "SELECT lock_reason,expires_ms FROM admin_container_locks WHERE storage_id=?1 LIMIT 1");
    maintenance.bind(1, storage_id);
    if (maintenance.row()) {
        throw ApiError(409, "admin_maintenance",
                       "Storage is temporarily locked for local admin maintenance. Try again after the admin change is finished.",
                       true);
    }
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

void validate_telemetry_node(const json& node, std::size_t depth, std::size_t& count,
                             std::unordered_set<std::string>& item_ids, const HostConfig& config) {
    if (!node.is_object()) throw ApiError(400, "invalid_player_snapshot", "Every player inventory node must be an object.");
    if (depth > static_cast<std::size_t>(config.max_item_depth)) {
        throw ApiError(400, "player_snapshot_too_deep", "The player inventory snapshot is too deeply nested.");
    }
    if (++count > static_cast<std::size_t>(config.max_item_nodes)) {
        throw ApiError(400, "player_snapshot_too_large", "The player inventory snapshot contains too many item nodes.");
    }
    const auto item_id = required_string(node, "item_id", 192);
    required_string(node, "class_name", 256);
    if (!item_ids.insert(item_id).second) {
        throw ApiError(400, "duplicate_player_item_id", "The player inventory snapshot contains a duplicate item ID.");
    }
    const double quantity = node.value("quantity", 0.0);
    const double health = node.value("health", 0.0);
    if (!std::isfinite(quantity) || !std::isfinite(health) || quantity < 0.0 || health < 0.0) {
        throw ApiError(400, "invalid_player_snapshot", "Player item quantity and health must be finite non-negative values.");
    }
    if (node.contains("children")) {
        if (!node["children"].is_array()) {
            throw ApiError(400, "invalid_player_snapshot", "Player item children must be an array.");
        }
        for (const auto& child : node["children"]) {
            validate_telemetry_node(child, depth + 1, count, item_ids, config);
        }
    }
}

std::string bounded_optional_string(const json& request, const char* key, std::size_t maximum) {
    if (!request.contains(key) || request[key].is_null()) return {};
    if (!request[key].is_string()) throw ApiError(400, "invalid_request", std::string(key) + " must be a string.");
    const auto value = request[key].get<std::string>();
    if (value.size() > maximum || value.find('\0') != std::string::npos) {
        throw ApiError(400, "invalid_request", std::string(key) + " is too long or contains a NUL byte.");
    }
    return value;
}

double optional_coordinate(const json& request, const char* key) {
    if (!request.contains(key) || request[key].is_null()) return std::numeric_limits<double>::quiet_NaN();
    if (!request[key].is_number()) throw ApiError(400, "invalid_request", std::string(key) + " must be numeric.");
    const double value = request[key].get<double>();
    if (!std::isfinite(value) || std::abs(value) > 1000000.0) {
        throw ApiError(400, "invalid_request", std::string(key) + " is outside the supported world coordinate range.");
    }
    return value;
}

void collect_item_index_rows(const std::string& storage_id,
                             const std::string& root_item_id,
                             const json& node,
                             const std::string& parent_item_id,
                             int depth,
                             std::int64_t updated_ms,
                             json& rows) {
    if (!node.is_object()) throw std::runtime_error("Stored cargo tree contains a non-object node.");
    const auto item_id = node.value("item_id", "");
    const auto class_name = node.value("class_name", "");
    if (item_id.empty() || class_name.empty()) {
        throw std::runtime_error("Stored cargo tree is missing item_id or class_name while rebuilding the item index.");
    }
    const auto adapter_id = node.contains("adapter") && node["adapter"].is_object()
        ? node["adapter"].value("id", "") : "";
    const auto location_type = node.contains("location") && node["location"].is_object()
        ? node["location"].value("kind", "") : "";
    rows.push_back({
        {"storage_id", storage_id},
        {"root_item_id", root_item_id},
        {"item_id", item_id},
        {"parent_item_id", parent_item_id.empty() ? json(nullptr) : json(parent_item_id)},
        {"depth", depth},
        {"class_name", class_name},
        {"quantity", node.value("quantity", 0.0)},
        {"health", node.value("health", 0.0)},
        {"adapter_id", adapter_id},
        {"location_type", location_type},
        {"search_state_json", node.contains("state") && node["state"].is_object() ? node["state"] : json::object()},
        {"updated_ms", updated_ms}
    });
    if (!node.contains("children")) return;
    if (!node["children"].is_array()) {
        throw std::runtime_error("Stored cargo tree has a non-array children field while rebuilding the item index.");
    }
    for (const auto& child : node["children"]) {
        collect_item_index_rows(storage_id, root_item_id, child, item_id, depth + 1, updated_ms, rows);
    }
}

std::size_t insert_item_index_rows(PgPool* database, const json& rows) {
    if (!rows.is_array() || rows.empty()) return 0;
    Statement insert(database, R"SQL(
INSERT INTO cargo_item_index(
    storage_id,root_item_id,item_id,parent_item_id,depth,class_name,quantity,health,
    adapter_id,location_type,search_state_json,updated_ms
)
SELECT x.storage_id,x.root_item_id,x.item_id,x.parent_item_id,x.depth,x.class_name,x.quantity,x.health,
       x.adapter_id,x.location_type,x.search_state_json,x.updated_ms
FROM jsonb_to_recordset(?1::jsonb) AS x(
    storage_id text,root_item_id text,item_id text,parent_item_id text,depth integer,class_name text,
    quantity double precision,health double precision,adapter_id text,location_type text,
    search_state_json jsonb,updated_ms bigint
)
ON CONFLICT(storage_id,root_item_id,item_id) DO NOTHING
)SQL");
    insert.bind(1, rows.dump());
    insert.done();
    return static_cast<std::size_t>(insert.affected_rows());
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
    container_class TEXT NOT NULL DEFAULT '',
    world_position_x DOUBLE PRECISION,
    world_position_y DOUBLE PRECISION,
    world_position_z DOUBLE PRECISION,
    map_name TEXT NOT NULL DEFAULT '',
    first_seen_ms BIGINT,
    last_seen_ms BIGINT,
    created_ms BIGINT NOT NULL,
    updated_ms BIGINT NOT NULL,
    UNIQUE(provider_id, provider_key)
) WITH (fillfactor=80, autovacuum_vacuum_scale_factor=0.05, autovacuum_analyze_scale_factor=0.02);
ALTER TABLE storage_containers ADD COLUMN IF NOT EXISTS container_class TEXT NOT NULL DEFAULT '';
ALTER TABLE storage_containers ADD COLUMN IF NOT EXISTS world_position_x DOUBLE PRECISION;
ALTER TABLE storage_containers ADD COLUMN IF NOT EXISTS world_position_y DOUBLE PRECISION;
ALTER TABLE storage_containers ADD COLUMN IF NOT EXISTS world_position_z DOUBLE PRECISION;
ALTER TABLE storage_containers ADD COLUMN IF NOT EXISTS map_name TEXT NOT NULL DEFAULT '';
ALTER TABLE storage_containers ADD COLUMN IF NOT EXISTS first_seen_ms BIGINT;
ALTER TABLE storage_containers ADD COLUMN IF NOT EXISTS last_seen_ms BIGINT;
CREATE INDEX IF NOT EXISTS storage_containers_class_seen
    ON storage_containers((lower(container_class)) text_pattern_ops,last_seen_ms DESC,storage_id);
CREATE INDEX IF NOT EXISTS storage_containers_last_seen
    ON storage_containers(last_seen_ms DESC,storage_id);

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
CREATE INDEX IF NOT EXISTS cargo_roots_class_prefix
    ON cargo_roots((lower(class_name)) text_pattern_ops,storage_id,root_item_id);

CREATE TABLE IF NOT EXISTS cargo_item_index(
    storage_id TEXT NOT NULL,
    root_item_id TEXT NOT NULL,
    item_id TEXT NOT NULL,
    parent_item_id TEXT,
    depth INTEGER NOT NULL CHECK(depth >= 0),
    class_name TEXT NOT NULL,
    quantity DOUBLE PRECISION NOT NULL CHECK(quantity >= 0 AND quantity <> 'NaN'::float8 AND quantity <> 'Infinity'::float8),
    health DOUBLE PRECISION NOT NULL CHECK(health >= 0 AND health <> 'NaN'::float8 AND health <> 'Infinity'::float8),
    adapter_id TEXT NOT NULL,
    location_type TEXT NOT NULL,
    search_state_json JSONB NOT NULL,
    updated_ms BIGINT NOT NULL,
    PRIMARY KEY(storage_id,root_item_id,item_id),
    FOREIGN KEY(storage_id,root_item_id) REFERENCES cargo_roots(storage_id,root_item_id) ON DELETE CASCADE
) WITH (autovacuum_vacuum_scale_factor=0.05, autovacuum_analyze_scale_factor=0.02);
CREATE INDEX IF NOT EXISTS cargo_item_index_item_lookup
    ON cargo_item_index(item_id,storage_id,root_item_id);
CREATE INDEX IF NOT EXISTS cargo_item_index_class_prefix
    ON cargo_item_index((lower(class_name)) text_pattern_ops,storage_id,root_item_id,item_id);
CREATE INDEX IF NOT EXISTS cargo_item_index_storage
    ON cargo_item_index(storage_id,root_item_id,depth,item_id);

CREATE TABLE IF NOT EXISTS cargo_item_index_state(
    state_id SMALLINT PRIMARY KEY CHECK(state_id=1),
    complete BOOLEAN NOT NULL,
    indexed_roots BIGINT NOT NULL DEFAULT 0 CHECK(indexed_roots >= 0),
    updated_ms BIGINT NOT NULL,
    last_error TEXT
);
INSERT INTO cargo_item_index_state(state_id,complete,indexed_roots,updated_ms,last_error)
SELECT 1,NOT EXISTS(SELECT 1 FROM cargo_roots),0,0,NULL
ON CONFLICT(state_id) DO NOTHING;

CREATE OR REPLACE FUNCTION clippy.refresh_cargo_item_index()
RETURNS trigger
LANGUAGE plpgsql
AS $$
BEGIN
    DELETE FROM cargo_item_index
    WHERE storage_id=NEW.storage_id AND root_item_id=NEW.root_item_id;

    INSERT INTO cargo_item_index(
        storage_id,root_item_id,item_id,parent_item_id,depth,class_name,quantity,health,
        adapter_id,location_type,search_state_json,updated_ms
    )
    WITH RECURSIVE nodes(node,parent_item_id,depth) AS (
        SELECT NEW.tree_json,NULL::text,0
        UNION ALL
        SELECT child.value,nodes.node->>'item_id',nodes.depth+1
        FROM nodes
        CROSS JOIN LATERAL jsonb_array_elements(
            CASE
                WHEN jsonb_typeof(nodes.node->'children')='array' THEN nodes.node->'children'
                ELSE '[]'::jsonb
            END
        ) AS child(value)
    )
    SELECT
        NEW.storage_id,
        NEW.root_item_id,
        node->>'item_id',
        parent_item_id,
        depth,
        node->>'class_name',
        COALESCE((node->>'quantity')::double precision,0),
        COALESCE((node->>'health')::double precision,0),
        COALESCE(node#>>'{adapter,id}',''),
        COALESCE(node#>>'{location,kind}',''),
        COALESCE(node->'state','{}'::jsonb),
        NEW.created_ms
    FROM nodes
    WHERE jsonb_typeof(node)='object' AND node ? 'item_id' AND node ? 'class_name';

    RETURN NEW;
END
$$;

DROP TRIGGER IF EXISTS cargo_roots_refresh_item_index ON cargo_roots;
CREATE TRIGGER cargo_roots_refresh_item_index
AFTER INSERT OR UPDATE OF tree_json ON cargo_roots
FOR EACH ROW EXECUTE FUNCTION clippy.refresh_cargo_item_index();

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

CREATE TABLE IF NOT EXISTS admin_container_locks(
    storage_id TEXT PRIMARY KEY REFERENCES storage_containers(storage_id) ON DELETE CASCADE,
    lock_id TEXT NOT NULL UNIQUE,
    admin_session_id TEXT NOT NULL,
    lock_reason TEXT NOT NULL,
    created_ms BIGINT NOT NULL,
    expires_ms BIGINT NOT NULL CHECK(expires_ms > created_ms)
);
CREATE INDEX IF NOT EXISTS admin_container_locks_expiry ON admin_container_locks(expires_ms);

CREATE TABLE IF NOT EXISTS admin_change_sets(
    change_id TEXT PRIMARY KEY,
    admin_session_id TEXT NOT NULL,
    windows_identity TEXT NOT NULL DEFAULT '',
    action_type TEXT NOT NULL,
    storage_id TEXT NOT NULL REFERENCES storage_containers(storage_id),
    target_storage_id TEXT REFERENCES storage_containers(storage_id),
    item_id TEXT,
    before_revision BIGINT NOT NULL CHECK(before_revision >= 0),
    after_revision BIGINT NOT NULL CHECK(after_revision >= before_revision),
    target_before_revision BIGINT,
    target_after_revision BIGINT,
    reason TEXT NOT NULL DEFAULT '',
    request_id TEXT NOT NULL,
    status TEXT NOT NULL CHECK(status IN ('APPLIED','UNDONE')),
    created_ms BIGINT NOT NULL,
    undone_ms BIGINT,
    undo_change_id TEXT
) WITH (fillfactor=90);
CREATE INDEX IF NOT EXISTS admin_change_sets_storage_history ON admin_change_sets(storage_id,created_ms DESC,change_id DESC);
CREATE INDEX IF NOT EXISTS admin_change_sets_created ON admin_change_sets(created_ms DESC,change_id DESC);

CREATE TABLE IF NOT EXISTS admin_change_entries(
    entry_id BIGINT GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY,
    change_id TEXT NOT NULL REFERENCES admin_change_sets(change_id),
    storage_id TEXT NOT NULL REFERENCES storage_containers(storage_id),
    root_item_id TEXT NOT NULL,
    item_id TEXT,
    entry_kind TEXT NOT NULL CHECK(entry_kind IN ('ROOT','ITEM')),
    before_state JSONB,
    after_state JSONB
);
CREATE INDEX IF NOT EXISTS admin_change_entries_change ON admin_change_entries(change_id,entry_id);
CREATE INDEX IF NOT EXISTS admin_change_entries_item ON admin_change_entries(item_id,change_id);

CREATE TABLE IF NOT EXISTS admin_quarantine(
    quarantine_id TEXT PRIMARY KEY,
    change_id TEXT NOT NULL REFERENCES admin_change_sets(change_id),
    storage_id TEXT NOT NULL REFERENCES storage_containers(storage_id),
    root_item_id TEXT NOT NULL,
    item_id TEXT NOT NULL,
    parent_item_id TEXT,
    parent_index INTEGER CHECK(parent_index IS NULL OR parent_index >= 0),
    tree_json JSONB NOT NULL,
    reason TEXT NOT NULL DEFAULT '',
    created_ms BIGINT NOT NULL,
    restored_change_id TEXT,
    restored_ms BIGINT
);
CREATE INDEX IF NOT EXISTS admin_quarantine_active ON admin_quarantine(created_ms DESC,quarantine_id) WHERE restored_ms IS NULL;

CREATE TABLE IF NOT EXISTS admin_storage_snapshots(
    snapshot_id TEXT PRIMARY KEY,
    storage_id TEXT NOT NULL REFERENCES storage_containers(storage_id),
    revision BIGINT NOT NULL CHECK(revision >= 0),
    root_count INTEGER NOT NULL CHECK(root_count >= 0),
    node_count BIGINT NOT NULL CHECK(node_count >= 0),
    reason TEXT NOT NULL DEFAULT '',
    admin_session_id TEXT NOT NULL,
    windows_identity TEXT NOT NULL DEFAULT '',
    created_ms BIGINT NOT NULL
);
CREATE INDEX IF NOT EXISTS admin_storage_snapshots_history ON admin_storage_snapshots(storage_id,created_ms DESC,snapshot_id DESC);
CREATE TABLE IF NOT EXISTS admin_snapshot_roots(
    snapshot_id TEXT NOT NULL REFERENCES admin_storage_snapshots(snapshot_id) ON DELETE CASCADE,
    root_item_id TEXT NOT NULL,
    tree_json JSONB NOT NULL,
    PRIMARY KEY(snapshot_id,root_item_id)
);

CREATE TABLE IF NOT EXISTS admin_audit_events(
    event_id BIGINT GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY,
    admin_session_id TEXT NOT NULL,
    windows_identity TEXT NOT NULL DEFAULT '',
    action TEXT NOT NULL,
    target_type TEXT NOT NULL,
    target_id TEXT NOT NULL,
    result TEXT NOT NULL CHECK(result IN ('SUCCESS','FAILURE')),
    reason TEXT NOT NULL DEFAULT '',
    error TEXT,
    request_id TEXT NOT NULL,
    change_id TEXT,
    detail_json JSONB NOT NULL DEFAULT '{}'::jsonb,
    created_ms BIGINT NOT NULL
);
CREATE INDEX IF NOT EXISTS admin_audit_events_created ON admin_audit_events(created_ms DESC,event_id DESC);
CREATE INDEX IF NOT EXISTS admin_audit_events_target ON admin_audit_events(target_type,target_id,created_ms DESC);

CREATE TABLE IF NOT EXISTS players(
    player_id TEXT PRIMARY KEY,
    display_name TEXT NOT NULL,
    plain_name TEXT NOT NULL DEFAULT '',
    full_name TEXT NOT NULL DEFAULT '',
    last_session_player_id INTEGER,
    last_ping_ms INTEGER CHECK(last_ping_ms IS NULL OR last_ping_ms >= 0),
    last_bandwidth_kbps INTEGER CHECK(last_bandwidth_kbps IS NULL OR last_bandwidth_kbps >= 0),
    last_output_throttle DOUBLE PRECISION CHECK(last_output_throttle IS NULL OR (last_output_throttle >= 0 AND last_output_throttle <= 1)),
    last_map_name TEXT NOT NULL DEFAULT '',
    last_position_x DOUBLE PRECISION,
    last_position_y DOUBLE PRECISION,
    last_position_z DOUBLE PRECISION,
    first_seen_ms BIGINT NOT NULL,
    last_seen_ms BIGINT NOT NULL,
    last_snapshot_ms BIGINT,
    last_inventory_count INTEGER NOT NULL DEFAULT 0 CHECK(last_inventory_count >= 0)
) WITH (fillfactor=90);
ALTER TABLE players ADD COLUMN IF NOT EXISTS plain_name TEXT NOT NULL DEFAULT '';
ALTER TABLE players ADD COLUMN IF NOT EXISTS full_name TEXT NOT NULL DEFAULT '';
ALTER TABLE players ADD COLUMN IF NOT EXISTS last_session_player_id INTEGER;
ALTER TABLE players ADD COLUMN IF NOT EXISTS last_ping_ms INTEGER;
ALTER TABLE players ADD COLUMN IF NOT EXISTS last_bandwidth_kbps INTEGER;
ALTER TABLE players ADD COLUMN IF NOT EXISTS last_output_throttle DOUBLE PRECISION;
ALTER TABLE players ADD COLUMN IF NOT EXISTS last_map_name TEXT NOT NULL DEFAULT '';
ALTER TABLE players ADD COLUMN IF NOT EXISTS last_position_x DOUBLE PRECISION;
ALTER TABLE players ADD COLUMN IF NOT EXISTS last_position_y DOUBLE PRECISION;
ALTER TABLE players ADD COLUMN IF NOT EXISTS last_position_z DOUBLE PRECISION;
CREATE INDEX IF NOT EXISTS players_last_seen ON players(last_seen_ms DESC,player_id);
CREATE INDEX IF NOT EXISTS players_name_prefix ON players((lower(display_name)) text_pattern_ops,player_id);

CREATE TABLE IF NOT EXISTS player_aliases(
    player_id TEXT NOT NULL REFERENCES players(player_id) ON DELETE CASCADE,
    display_name TEXT NOT NULL,
    first_seen_ms BIGINT NOT NULL,
    last_seen_ms BIGINT NOT NULL,
    PRIMARY KEY(player_id,display_name)
);
CREATE INDEX IF NOT EXISTS player_aliases_name_prefix ON player_aliases((lower(display_name)) text_pattern_ops,player_id);

CREATE TABLE IF NOT EXISTS player_inventory_snapshots(
    snapshot_id TEXT PRIMARY KEY,
    player_id TEXT NOT NULL REFERENCES players(player_id) ON DELETE CASCADE,
    captured_ms BIGINT NOT NULL,
    item_count INTEGER NOT NULL CHECK(item_count >= 0),
    profile_json JSONB NOT NULL DEFAULT '{}'::jsonb,
    network_json JSONB NOT NULL DEFAULT '{}'::jsonb,
    position_json JSONB NOT NULL DEFAULT '{}'::jsonb,
    equipment_json JSONB NOT NULL DEFAULT '{}'::jsonb,
    inventory_json JSONB NOT NULL CHECK(jsonb_typeof(inventory_json)='array')
) WITH (autovacuum_vacuum_scale_factor=0.05, toast.autovacuum_vacuum_scale_factor=0.05);
ALTER TABLE player_inventory_snapshots ADD COLUMN IF NOT EXISTS profile_json JSONB NOT NULL DEFAULT '{}'::jsonb;
ALTER TABLE player_inventory_snapshots ADD COLUMN IF NOT EXISTS network_json JSONB NOT NULL DEFAULT '{}'::jsonb;
ALTER TABLE player_inventory_snapshots ADD COLUMN IF NOT EXISTS position_json JSONB NOT NULL DEFAULT '{}'::jsonb;
CREATE INDEX IF NOT EXISTS player_inventory_snapshots_player
    ON player_inventory_snapshots(player_id,captured_ms DESC,snapshot_id DESC);

CREATE TABLE IF NOT EXISTS player_item_index(
    snapshot_id TEXT NOT NULL REFERENCES player_inventory_snapshots(snapshot_id) ON DELETE CASCADE,
    player_id TEXT NOT NULL REFERENCES players(player_id) ON DELETE CASCADE,
    item_id TEXT NOT NULL,
    parent_item_id TEXT,
    depth INTEGER NOT NULL CHECK(depth >= 0),
    class_name TEXT NOT NULL,
    quantity DOUBLE PRECISION NOT NULL,
    health DOUBLE PRECISION NOT NULL,
    adapter_id TEXT NOT NULL,
    location_type TEXT NOT NULL,
    PRIMARY KEY(snapshot_id,item_id)
);
CREATE INDEX IF NOT EXISTS player_item_index_player_class
    ON player_item_index(player_id,(lower(class_name)) text_pattern_ops,snapshot_id,item_id);
CREATE INDEX IF NOT EXISTS player_item_index_item
    ON player_item_index(item_id,player_id,snapshot_id);
CREATE INDEX IF NOT EXISTS player_item_index_class
    ON player_item_index((lower(class_name)) text_pattern_ops,player_id,snapshot_id,item_id);

CREATE TABLE IF NOT EXISTS player_events(
    event_id BIGINT GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY,
    player_id TEXT NOT NULL REFERENCES players(player_id) ON DELETE CASCADE,
    event_type TEXT NOT NULL,
    detail_json JSONB NOT NULL DEFAULT '{}'::jsonb,
    created_ms BIGINT NOT NULL
);
CREATE INDEX IF NOT EXISTS player_events_player ON player_events(player_id,created_ms DESC,event_id DESC);
CREATE INDEX IF NOT EXISTS player_events_created ON player_events(created_ms DESC,event_id DESC);

CREATE TABLE IF NOT EXISTS admin_player_commands(
    command_id TEXT PRIMARY KEY,
    idempotency_key TEXT NOT NULL UNIQUE,
    player_id TEXT NOT NULL REFERENCES players(player_id),
    action TEXT NOT NULL CHECK(action IN ('REQUEST_SNAPSHOT','REMOVE_ITEM','GIVE_ITEM','REPAIR_ITEM','MOVE_ITEM','QUARANTINE_ITEM','RESTORE_QUARANTINE')),
    payload_json JSONB NOT NULL DEFAULT '{}'::jsonb,
    status TEXT NOT NULL CHECK(status IN ('PENDING','CLAIMED','SUCCEEDED','FAILED','EXPIRED')),
    admin_session_id TEXT NOT NULL,
    windows_identity TEXT NOT NULL DEFAULT '',
    request_id TEXT NOT NULL,
    reason TEXT NOT NULL DEFAULT '',
    created_ms BIGINT NOT NULL,
    expires_ms BIGINT NOT NULL,
    claimed_ms BIGINT,
    completed_ms BIGINT,
    result_json JSONB,
    error TEXT
) WITH (fillfactor=90);
CREATE INDEX IF NOT EXISTS admin_player_commands_pending
    ON admin_player_commands(player_id,created_ms,command_id) WHERE status='PENDING';
CREATE INDEX IF NOT EXISTS admin_player_commands_history
    ON admin_player_commands(created_ms DESC,command_id DESC);

CREATE TABLE IF NOT EXISTS player_quarantine(
    quarantine_id TEXT PRIMARY KEY,
    command_id TEXT NOT NULL REFERENCES admin_player_commands(command_id),
    player_id TEXT NOT NULL REFERENCES players(player_id),
    item_id TEXT NOT NULL,
    tree_json JSONB NOT NULL,
    created_ms BIGINT NOT NULL,
    restored_command_id TEXT,
    restored_ms BIGINT
);
CREATE INDEX IF NOT EXISTS player_quarantine_active
    ON player_quarantine(created_ms DESC,quarantine_id) WHERE restored_ms IS NULL;

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
    const auto container_class = bounded_optional_string(request, "container_class", 256);
    const auto map_name = bounded_optional_string(request, "map_name", 128);
    const double position_x = optional_coordinate(request, "world_position_x");
    const double position_y = optional_coordinate(request, "world_position_y");
    const double position_z = optional_coordinate(request, "world_position_z");
    if (display_name.empty() || display_name.size() > 256 || capacity < 0 || capacity > 100000000) {
        throw ApiError(400, "invalid_request", "display_name or capacity_slots is invalid.");
    }

    std::lock_guard lock(writer_gate_);
    Transaction transaction(writer_);
    const auto now = now_unix_ms();
    Statement insert(writer_, R"SQL(
INSERT INTO storage_containers(
    storage_id,provider_id,provider_key,display_name,capacity_slots,revision,
    container_class,world_position_x,world_position_y,world_position_z,map_name,
    first_seen_ms,last_seen_ms,created_ms,updated_ms
)
VALUES(?1,?2,?3,?4,?5,0,?6,?7,?8,?9,?10,?11,?11,?11,?11)
ON CONFLICT(provider_id,provider_key) DO UPDATE SET
    display_name=excluded.display_name,
    capacity_slots=excluded.capacity_slots,
    container_class=CASE WHEN excluded.container_class<>'' THEN excluded.container_class ELSE storage_containers.container_class END,
    world_position_x=COALESCE(excluded.world_position_x,storage_containers.world_position_x),
    world_position_y=COALESCE(excluded.world_position_y,storage_containers.world_position_y),
    world_position_z=COALESCE(excluded.world_position_z,storage_containers.world_position_z),
    map_name=CASE WHEN excluded.map_name<>'' THEN excluded.map_name ELSE storage_containers.map_name END,
    first_seen_ms=COALESCE(storage_containers.first_seen_ms,excluded.first_seen_ms),
    last_seen_ms=excluded.last_seen_ms,
    updated_ms=excluded.updated_ms
)SQL");
    insert.bind(1, random_hex(16));
    insert.bind(2, provider_id);
    insert.bind(3, provider_key);
    insert.bind(4, display_name);
    insert.bind(5, static_cast<std::int64_t>(capacity));
    insert.bind(6, container_class);
    if (std::isnan(position_x)) insert.bind_null(7); else insert.bind(7, position_x);
    if (std::isnan(position_y)) insert.bind_null(8); else insert.bind(8, position_y);
    if (std::isnan(position_z)) insert.bind_null(9); else insert.bind(9, position_z);
    insert.bind(10, map_name);
    insert.bind(11, now);
    insert.done();

    Statement query(writer_,
        "SELECT storage_id,provider_id,provider_key,display_name,capacity_slots,revision,updated_ms,"
        "container_class,world_position_x,world_position_y,world_position_z,map_name,first_seen_ms,last_seen_ms "
        "FROM storage_containers WHERE provider_id=?1 AND provider_key=?2");
    query.bind(1, provider_id);
    query.bind(2, provider_key);
    if (!query.row()) throw std::runtime_error("Resolved storage container disappeared.");
    auto result = container_row(query);
    result["container_class"] = query.text(7);
    if (!query.is_null(8)) result["world_position_x"] = query.number(8);
    if (!query.is_null(9)) result["world_position_y"] = query.number(9);
    if (!query.is_null(10)) result["world_position_z"] = query.number(10);
    result["map_name"] = query.text(11);
    if (!query.is_null(12)) result["first_seen_ms"] = query.integer(12);
    if (!query.is_null(13)) result["last_seen_ms"] = query.integer(13);
    transaction.commit();
    return result;
}

json StorageDatabase::observe_container(const json& request) {
    const auto storage_id = required_string(request, "storage_id", 64);
    const auto container_class = bounded_optional_string(request, "container_class", 256);
    const auto map_name = bounded_optional_string(request, "map_name", 128);
    const double position_x = optional_coordinate(request, "world_position_x");
    const double position_y = optional_coordinate(request, "world_position_y");
    const double position_z = optional_coordinate(request, "world_position_z");
    const auto display_name = bounded_optional_string(request, "display_name", 256);
    const auto now = now_unix_ms();

    std::lock_guard lock(writer_gate_);
    Transaction transaction(writer_);
    Statement update(writer_, R"SQL(
UPDATE storage_containers SET
    display_name=CASE WHEN ?2<>'' THEN ?2 ELSE display_name END,
    container_class=CASE WHEN ?3<>'' THEN ?3 ELSE container_class END,
    world_position_x=COALESCE(?4,world_position_x),
    world_position_y=COALESCE(?5,world_position_y),
    world_position_z=COALESCE(?6,world_position_z),
    map_name=CASE WHEN ?7<>'' THEN ?7 ELSE map_name END,
    first_seen_ms=COALESCE(first_seen_ms,?8),
    last_seen_ms=?8
WHERE storage_id=?1
RETURNING revision
)SQL");
    update.bind(1, storage_id);
    update.bind(2, display_name);
    update.bind(3, container_class);
    if (std::isnan(position_x)) update.bind_null(4); else update.bind(4, position_x);
    if (std::isnan(position_y)) update.bind_null(5); else update.bind(5, position_y);
    if (std::isnan(position_z)) update.bind_null(6); else update.bind(6, position_z);
    update.bind(7, map_name);
    update.bind(8, now);
    if (!update.row()) throw ApiError(404, "storage_not_found", "The storage container is not registered.");
    const auto revision = update.integer(0);
    transaction.commit();
    return {{"storage_id", storage_id}, {"revision", revision}, {"last_seen_ms", now}};
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

json StorageDatabase::item_index_status() {
    auto& slot = reader();
    std::lock_guard lock(slot.mutex);
    Transaction transaction(slot.connection, "BEGIN READ ONLY");
    Statement state(slot.connection,
        "SELECT complete,indexed_roots,updated_ms,last_error,"
        "COALESCE((SELECT GREATEST(reltuples,0)::bigint FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace "
        "WHERE n.nspname='clippy' AND c.relname='cargo_roots'),0),"
        "COALESCE((SELECT GREATEST(reltuples,0)::bigint FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace "
        "WHERE n.nspname='clippy' AND c.relname='cargo_item_index'),0) "
        "FROM cargo_item_index_state WHERE state_id=1");
    if (!state.row()) throw std::runtime_error("Cargo item index state row is missing.");
    json result = {
        {"available", true},
        {"complete", state.text(0) == "t"},
        {"indexed_roots", state.integer(1)},
        {"updated_ms", state.integer(2)},
        {"root_count_estimated", state.integer(4)},
        {"indexed_nodes_estimated", state.integer(5)}
    };
    if (!state.is_null(3)) result["last_error"] = state.text(3);
    transaction.commit();
    return result;
}

json StorageDatabase::rebuild_item_index_batch(const json& request) {
    int root_limit = 4;
    if (request.contains("root_limit")) {
        if (!request["root_limit"].is_number_integer()) {
            throw ApiError(400, "invalid_request", "root_limit must be an integer.");
        }
        root_limit = request["root_limit"].get<int>();
    }
    if (root_limit < 1 || root_limit > 8) {
        throw ApiError(400, "invalid_request", "root_limit must be between 1 and 8.");
    }

    std::lock_guard lock(writer_gate_);
    Transaction transaction(writer_);
    Statement roots(writer_, R"SQL(
SELECT r.storage_id,r.root_item_id,r.tree_json::text,r.created_ms
FROM cargo_roots r
WHERE NOT EXISTS(
    SELECT 1 FROM cargo_item_index i
    WHERE i.storage_id=r.storage_id AND i.root_item_id=r.root_item_id
)
ORDER BY r.storage_id,r.root_item_id
LIMIT ?1
FOR UPDATE OF r SKIP LOCKED
)SQL");
    roots.bind(1, static_cast<std::int64_t>(root_limit));

    struct PendingRoot {
        std::string storage_id;
        std::string root_item_id;
        json tree;
        std::int64_t updated_ms = 0;
    };
    std::vector<PendingRoot> pending;
    while (roots.row()) {
        pending.push_back({
            roots.text(0),
            roots.text(1),
            json::parse(roots.text(2)),
            roots.integer(3)
        });
    }

    std::size_t nodes_indexed = 0;
    for (const auto& root : pending) {
        json rows = json::array();
        collect_item_index_rows(root.storage_id, root.root_item_id, root.tree, "", 0, root.updated_ms, rows);
        const auto inserted = insert_item_index_rows(writer_, rows);
        if (inserted != rows.size()) {
            throw std::runtime_error("Cargo item index backfill did not insert every node for a locked root.");
        }
        nodes_indexed += inserted;
    }

    Statement remaining(writer_, R"SQL(
SELECT EXISTS(
    SELECT 1
    FROM cargo_roots r
    WHERE NOT EXISTS(
        SELECT 1 FROM cargo_item_index i
        WHERE i.storage_id=r.storage_id AND i.root_item_id=r.root_item_id
    )
    LIMIT 1
)
)SQL");
    if (!remaining.row()) throw std::runtime_error("Cargo item index completeness query returned no row.");
    const bool complete = remaining.text(0) != "t";
    Statement update(writer_,
        "UPDATE cargo_item_index_state "
        "SET complete=?1,indexed_roots=indexed_roots+?2,updated_ms=?3,last_error=NULL "
        "WHERE state_id=1");
    update.bind(1, std::string(complete ? "true" : "false"));
    update.bind(2, static_cast<std::int64_t>(pending.size()));
    update.bind(3, now_unix_ms());
    update.done();
    transaction.commit();

    return {
        {"roots_indexed", static_cast<std::int64_t>(pending.size())},
        {"nodes_indexed", static_cast<std::int64_t>(nodes_indexed)},
        {"complete", complete}
    };
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
    Statement index_state(slot.connection, "SELECT complete FROM cargo_item_index_state WHERE state_id=1");
    if (!index_state.row()) throw std::runtime_error("Cargo item index state row is missing.");
    const bool item_index_complete = index_state.text(0) == "t";
    if (item_index_complete) {
        check_zero("cargo_item_index_node_counts",
                   "SELECT count(*) FROM cargo_roots r LEFT JOIN ("
                   "SELECT storage_id,root_item_id,count(*) AS indexed_nodes FROM cargo_item_index GROUP BY storage_id,root_item_id"
                   ") i USING(storage_id,root_item_id) WHERE COALESCE(i.indexed_nodes,0)<>r.node_count");
    }
    check_zero("player_snapshot_index_counts",
               "SELECT count(*) FROM ("
               "SELECT DISTINCT ON (player_id) snapshot_id,item_count FROM player_inventory_snapshots "
               "ORDER BY player_id,captured_ms DESC,snapshot_id DESC"
               ") s LEFT JOIN ("
               "SELECT snapshot_id,count(*) AS indexed_nodes FROM player_item_index GROUP BY snapshot_id"
               ") i USING(snapshot_id) WHERE COALESCE(i.indexed_nodes,0)<>s.item_count");
    check_zero("invalid_player_telemetry",
               "SELECT count(*) FROM players WHERE "
               "(last_ping_ms IS NOT NULL AND last_ping_ms<0) OR "
               "(last_bandwidth_kbps IS NOT NULL AND last_bandwidth_kbps<0) OR "
               "(last_output_throttle IS NOT NULL AND (last_output_throttle<0 OR last_output_throttle>1)) OR "
               "(last_position_x IS NOT NULL AND (last_position_y IS NULL OR last_position_z IS NULL)) OR "
               "(last_position_y IS NOT NULL AND (last_position_x IS NULL OR last_position_z IS NULL)) OR "
               "(last_position_z IS NOT NULL AND (last_position_x IS NULL OR last_position_y IS NULL))");
    check_zero("invalid_player_snapshot_payloads",
               "SELECT count(*) FROM player_inventory_snapshots WHERE "
               "jsonb_typeof(profile_json)<>'object' OR jsonb_typeof(network_json)<>'object' OR "
               "jsonb_typeof(position_json)<>'object' OR jsonb_typeof(equipment_json)<>'object' OR "
               "jsonb_typeof(inventory_json)<>'array'");
    check_zero("invalid_player_quarantine",
               "SELECT count(*) FROM player_quarantine WHERE jsonb_typeof(tree_json)<>'object'");
    Statement app(slot.connection,
        "SELECT count(*) FROM application_meta WHERE key='application' AND value='ClippyVirtualCargo'");
    if (!app.row() || app.integer(0) != 1) {
        healthy = false;
        checks.push_back({{"check", "application_marker"}, {"errors", 1}});
    } else {
        checks.push_back({{"check", "application_marker"}, {"errors", 0}});
    }
    transaction.commit();
    return {{"healthy", healthy}, {"checks", std::move(checks)}, {"item_index_complete", item_index_complete},
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

json StorageDatabase::verify_backup(const json& request) {
    const auto filename = required_string(request, "filename", 128);
    static const std::regex generated_name(R"(^ClippyVirtualCargo-[0-9]{10,20}-[0-9a-fA-F]{8}\.dump$)");
    if (!std::regex_match(filename, generated_name)) {
        throw ApiError(400, "invalid_backup", "Only Clippy-generated PostgreSQL backup filenames can be verified.");
    }
    const auto path = config_.backup_directory / filename;
    if (!std::filesystem::is_regular_file(path)) {
        throw ApiError(404, "backup_not_found", "The PostgreSQL backup file does not exist.");
    }
    const auto started = std::chrono::steady_clock::now();
#ifdef _WIN32
    if (config_.postgres_bin_directory.empty()) {
        throw std::runtime_error("postgresBinDirectory is required for PostgreSQL backup verification.");
    }
    const auto restore = std::filesystem::path(config_.postgres_bin_directory) / "pg_restore.exe";
    if (run_postgres_tool(restore, {L"--list", path.wstring()}, config_.postgres_password) != 0) {
        throw ApiError(409, "backup_invalid", "pg_restore could not validate this PostgreSQL backup archive.");
    }
#else
    throw std::runtime_error("PostgreSQL backup verification is currently packaged for the Windows DayZ server build.");
#endif
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    return {{"filename", filename}, {"path", path.string()}, {"bytes", static_cast<std::int64_t>(std::filesystem::file_size(path))},
            {"verified", true}, {"duration_ms", elapsed}, {"format", "pg_dump custom"}};
}


json StorageDatabase::player_snapshot(const json& request) {
    const auto player_id = required_string(request, "player_id", 128);
    const auto display_name = required_string(request, "display_name", 128);
    if (!request.contains("inventory") || !request["inventory"].is_array()) {
        throw ApiError(400, "invalid_player_snapshot", "inventory must be an array.");
    }
    json equipment = request.value("equipment", json::object());
    json profile = request.value("profile", json::object());
    json network = request.value("network", json::object());
    json position = request.value("position", json::object());
    if (!equipment.is_object() || !profile.is_object() || !network.is_object() || !position.is_object()) {
        throw ApiError(400, "invalid_player_snapshot", "equipment, profile, network, and position must be objects.");
    }
    const auto plain_name = bounded_optional_string(profile, "plain_name", 128);
    const auto full_name = bounded_optional_string(profile, "full_name", 256);
    int session_player_id = -1;
    if (profile.contains("session_player_id")) {
        if (!profile["session_player_id"].is_number_integer()) {
            throw ApiError(400, "invalid_player_snapshot", "session_player_id must be an integer.");
        }
        session_player_id = profile["session_player_id"].get<int>();
    }
    if (session_player_id < -1 || session_player_id > 1000000000) {
        throw ApiError(400, "invalid_player_snapshot", "session_player_id is outside the supported range.");
    }
    const bool network_available = network.value("available", false);
    if (network.contains("available") && !network["available"].is_boolean()) {
        throw ApiError(400, "invalid_player_snapshot", "network.available must be boolean.");
    }
    auto network_int = [&](const char* key, int maximum) {
        if (!network.contains(key)) return 0;
        if (!network[key].is_number_integer()) throw ApiError(400, "invalid_player_snapshot", std::string("network.") + key + " must be an integer.");
        const auto value = network[key].get<int>();
        if (value < 0 || value > maximum) throw ApiError(400, "invalid_player_snapshot", std::string("network.") + key + " is outside the supported range.");
        return value;
    };
    const int ping_act_ms = network_int("ping_act_ms", 600000);
    const int ping_min_ms = network_int("ping_min_ms", 600000);
    const int ping_max_ms = network_int("ping_max_ms", 600000);
    const int ping_avg_ms = network_int("ping_avg_ms", 600000);
    const int bandwidth_min_kbps = network_int("bandwidth_min_kbps", 1000000000);
    const int bandwidth_max_kbps = network_int("bandwidth_max_kbps", 1000000000);
    const int bandwidth_avg_kbps = network_int("bandwidth_avg_kbps", 1000000000);
    (void)ping_act_ms; (void)ping_min_ms; (void)ping_max_ms;
    (void)bandwidth_min_kbps; (void)bandwidth_max_kbps;
    double output_throttle = network.value("output_throttle", 0.0);
    if (!std::isfinite(output_throttle) || output_throttle < 0.0 || output_throttle > 1.0) {
        throw ApiError(400, "invalid_player_snapshot", "network.output_throttle must be between 0 and 1.");
    }
    const bool position_available = position.value("available", false);
    if (position.contains("available") && !position["available"].is_boolean()) {
        throw ApiError(400, "invalid_player_snapshot", "position.available must be boolean.");
    }
    const auto player_map_name = bounded_optional_string(position, "map_name", 128);
    if (position_available && (!position.contains("world_position_x") || !position.contains("world_position_y") || !position.contains("world_position_z"))) {
        throw ApiError(400, "invalid_player_snapshot", "position coordinates are required when position telemetry is available.");
    }
    const double player_x = position_available ? optional_coordinate(position, "world_position_x") : std::numeric_limits<double>::quiet_NaN();
    const double player_y = position_available ? optional_coordinate(position, "world_position_y") : std::numeric_limits<double>::quiet_NaN();
    const double player_z = position_available ? optional_coordinate(position, "world_position_z") : std::numeric_limits<double>::quiet_NaN();
    if (position_available && (std::isnan(player_x) || std::isnan(player_y) || std::isnan(player_z))) {
        throw ApiError(400, "invalid_player_snapshot", "position coordinates must be finite numbers.");
    }

    std::size_t item_count = 0;
    std::unordered_set<std::string> item_ids;
    for (const auto& root : request["inventory"]) {
        validate_telemetry_node(root, 0, item_count, item_ids, config_);
    }
    const auto now = now_unix_ms();
    const auto snapshot_id = random_hex(16);

    std::lock_guard lock(writer_gate_);
    Transaction transaction(writer_);
    Statement player(writer_, R"SQL(
INSERT INTO players(
    player_id,display_name,plain_name,full_name,last_session_player_id,last_ping_ms,last_bandwidth_kbps,
    last_output_throttle,last_map_name,last_position_x,last_position_y,last_position_z,
    first_seen_ms,last_seen_ms,last_snapshot_ms,last_inventory_count
)
VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?13,?13,?14)
ON CONFLICT(player_id) DO UPDATE SET
    display_name=excluded.display_name,
    plain_name=excluded.plain_name,
    full_name=excluded.full_name,
    last_session_player_id=excluded.last_session_player_id,
    last_ping_ms=excluded.last_ping_ms,
    last_bandwidth_kbps=excluded.last_bandwidth_kbps,
    last_output_throttle=excluded.last_output_throttle,
    last_map_name=excluded.last_map_name,
    last_position_x=excluded.last_position_x,
    last_position_y=excluded.last_position_y,
    last_position_z=excluded.last_position_z,
    last_seen_ms=excluded.last_seen_ms,
    last_snapshot_ms=excluded.last_snapshot_ms,
    last_inventory_count=excluded.last_inventory_count
)SQL");
    player.bind(1, player_id);
    player.bind(2, display_name);
    player.bind(3, plain_name);
    player.bind(4, full_name);
    if (session_player_id >= 0) player.bind(5, session_player_id); else player.bind_null(5);
    if (network_available) player.bind(6, ping_avg_ms); else player.bind_null(6);
    if (network_available) player.bind(7, bandwidth_avg_kbps); else player.bind_null(7);
    if (network_available) player.bind(8, output_throttle); else player.bind_null(8);
    player.bind(9, position_available ? player_map_name : std::string{});
    if (position_available) player.bind(10, player_x); else player.bind_null(10);
    if (position_available) player.bind(11, player_y); else player.bind_null(11);
    if (position_available) player.bind(12, player_z); else player.bind_null(12);
    player.bind(13, now);
    player.bind(14, static_cast<std::int64_t>(item_count));
    player.done();

    Statement alias(writer_, R"SQL(
INSERT INTO player_aliases(player_id,display_name,first_seen_ms,last_seen_ms)
VALUES(?1,?2,?3,?3)
ON CONFLICT(player_id,display_name) DO UPDATE SET last_seen_ms=excluded.last_seen_ms
)SQL");
    alias.bind(1, player_id);
    alias.bind(2, display_name);
    alias.bind(3, now);
    alias.done();

    Statement snapshot(writer_, R"SQL(
INSERT INTO player_inventory_snapshots(
    snapshot_id,player_id,captured_ms,item_count,profile_json,network_json,position_json,equipment_json,inventory_json
)
VALUES(?1,?2,?3,?4,?5::jsonb,?6::jsonb,?7::jsonb,?8::jsonb,?9::jsonb)
)SQL");
    snapshot.bind(1, snapshot_id);
    snapshot.bind(2, player_id);
    snapshot.bind(3, now);
    snapshot.bind(4, static_cast<std::int64_t>(item_count));
    snapshot.bind(5, profile.dump());
    snapshot.bind(6, network.dump());
    snapshot.bind(7, position.dump());
    snapshot.bind(8, equipment.dump());
    snapshot.bind(9, request["inventory"].dump());
    snapshot.done();

    Statement index(writer_, R"SQL(
INSERT INTO player_item_index(
    snapshot_id,player_id,item_id,parent_item_id,depth,class_name,quantity,health,adapter_id,location_type
)
WITH RECURSIVE roots(node) AS (
    SELECT value FROM jsonb_array_elements(?3::jsonb)
),
nodes(node,parent_item_id,depth) AS (
    SELECT node,NULL::text,0 FROM roots
    UNION ALL
    SELECT child.value,nodes.node->>'item_id',nodes.depth+1
    FROM nodes
    CROSS JOIN LATERAL jsonb_array_elements(
        CASE WHEN jsonb_typeof(nodes.node->'children')='array' THEN nodes.node->'children' ELSE '[]'::jsonb END
    ) child(value)
)
SELECT ?1,?2,node->>'item_id',parent_item_id,depth,node->>'class_name',
       COALESCE((node->>'quantity')::double precision,0),
       COALESCE((node->>'health')::double precision,0),
       COALESCE(node#>>'{adapter,id}',''),
       COALESCE(node#>>'{location,kind}','')
FROM nodes
WHERE jsonb_typeof(node)='object' AND node ? 'item_id' AND node ? 'class_name'
)SQL");
    index.bind(1, snapshot_id);
    index.bind(2, player_id);
    index.bind(3, request["inventory"].dump());
    index.done();

    // Keep historical snapshot JSON for compare/export, but keep the derived
    // searchable index only for this player's latest snapshot.
    Statement prune_old_index(writer_,
        "DELETE FROM player_item_index WHERE player_id=?1 AND snapshot_id<>?2");
    prune_old_index.bind(1, player_id);
    prune_old_index.bind(2, snapshot_id);
    prune_old_index.done();

    Statement prune_snapshot_history(writer_, R"SQL(
DELETE FROM player_inventory_snapshots WHERE snapshot_id IN (
    SELECT snapshot_id FROM player_inventory_snapshots
    WHERE player_id=?1
    ORDER BY captured_ms DESC,snapshot_id DESC
    OFFSET ?2 LIMIT ?3
)
)SQL");
    prune_snapshot_history.bind(1, player_id);
    prune_snapshot_history.bind(2, static_cast<std::int64_t>(config_.player_snapshot_history_limit));
    prune_snapshot_history.bind(3, static_cast<std::int64_t>(config_.maintenance_prune_batch_rows));
    prune_snapshot_history.done();

    Statement event(writer_,
        "INSERT INTO player_events(player_id,event_type,detail_json,created_ms) VALUES(?1,'SNAPSHOT',?2::jsonb,?3)");
    event.bind(1, player_id);
    event.bind(2, json{{"snapshot_id",snapshot_id},{"item_count",item_count},
                       {"ping_avg_ms",network_available ? json(ping_avg_ms) : json(nullptr)},
                       {"bandwidth_avg_kbps",network_available ? json(bandwidth_avg_kbps) : json(nullptr)},
                       {"map_name",position_available ? json(player_map_name) : json(nullptr)}}.dump());
    event.bind(3, now);
    event.done();

    transaction.commit();
    return {{"snapshot_id", snapshot_id}, {"player_id", player_id},
            {"captured_ms", now}, {"item_count", static_cast<std::int64_t>(item_count)}};
}

json StorageDatabase::poll_player_commands(const json& request) {
    const auto player_id = required_string(request, "player_id", 128);
    const int limit = std::clamp(request.value("limit", 4), 1, 10);
    const auto now = now_unix_ms();

    std::lock_guard lock(writer_gate_);
    Transaction transaction(writer_);
    Statement expire_pending(writer_,
        "UPDATE admin_player_commands SET status='EXPIRED',completed_ms=?1,error='Command expired before DayZ claimed it.' "
        "WHERE status='PENDING' AND expires_ms<=?1");
    expire_pending.bind(1, now);
    expire_pending.done();
    Statement expire_claimed(writer_,
        "UPDATE admin_player_commands SET status='EXPIRED',completed_ms=?1,error='Command result was not returned before the claim timeout.' "
        "WHERE status='CLAIMED' AND expires_ms+120000<=?1");
    expire_claimed.bind(1, now);
    expire_claimed.done();

    Statement query(writer_, R"SQL(
SELECT command_id,action,payload_json::text,expires_ms
FROM admin_player_commands
WHERE player_id=?1 AND status='PENDING' AND expires_ms>?2
ORDER BY created_ms,command_id
FOR UPDATE SKIP LOCKED
LIMIT ?3
)SQL");
    query.bind(1, player_id);
    query.bind(2, now);
    query.bind(3, static_cast<std::int64_t>(limit));
    json commands = json::array();
    std::vector<std::string> claimed;
    while (query.row()) {
        commands.push_back({
            {"command_id", query.text(0)},
            {"action", query.text(1)},
            {"payload_json", query.text(2)},
            {"expires_ms", query.integer(3)}
        });
        claimed.push_back(query.text(0));
    }
    Statement claim(writer_,
        "UPDATE admin_player_commands SET status='CLAIMED',claimed_ms=?2 WHERE command_id=?1 AND status='PENDING'");
    for (const auto& command_id : claimed) {
        claim.bind(1, command_id);
        claim.bind(2, now);
        claim.done();
        claim.reset();
    }
    if (!claimed.empty()) {
        Statement seen(writer_, "UPDATE players SET last_seen_ms=?2 WHERE player_id=?1");
        seen.bind(1, player_id);
        seen.bind(2, now);
        seen.done();
    }
    transaction.commit();
    return {{"player_id", player_id}, {"commands", std::move(commands)}, {"server_ms", now}};
}

json StorageDatabase::complete_player_command(const json& request) {
    const auto command_id = required_string(request, "command_id", 64);
    const auto player_id = required_string(request, "player_id", 128);
    const auto status = required_string(request, "status", 16);
    if (status != "SUCCEEDED" && status != "FAILED") {
        throw ApiError(400, "invalid_command_status", "Player command status must be SUCCEEDED or FAILED.");
    }
    const auto error = bounded_optional_string(request, "error", 1024);
    const auto result_text = bounded_optional_string(request, "result_json", 512 * 1024);
    json result = json::object();
    if (!result_text.empty()) {
        try {
            result = json::parse(result_text);
        } catch (...) {
            throw ApiError(400, "invalid_command_result", "result_json must contain valid JSON.");
        }
    }
    const auto now = now_unix_ms();

    std::lock_guard lock(writer_gate_);
    Transaction transaction(writer_);
    Statement current(writer_, R"SQL(
SELECT action,status,payload_json::text,admin_session_id,windows_identity,request_id,reason
FROM admin_player_commands
WHERE command_id=?1 AND player_id=?2
FOR UPDATE
)SQL");
    current.bind(1, command_id);
    current.bind(2, player_id);
    if (!current.row()) throw ApiError(404, "command_not_found", "The player command does not exist.");
    const auto action = current.text(0);
    const auto current_status = current.text(1);
    const auto payload = json::parse(current.text(2));
    const auto admin_session_id = current.text(3);
    const auto windows_identity = current.text(4);
    const auto request_id = current.text(5);
    const auto reason = current.text(6);
    if (current_status == "SUCCEEDED" || current_status == "FAILED") {
        transaction.commit();
        return {{"command_id",command_id},{"status",current_status},{"already_completed",true}};
    }
    if (current_status != "CLAIMED") {
        throw ApiError(409, "command_not_claimed", "This player command is not in a claimable completion state.", true);
    }

    Statement update(writer_, R"SQL(
UPDATE admin_player_commands
SET status=?2,completed_ms=?3,result_json=?4::jsonb,error=?5
WHERE command_id=?1
)SQL");
    update.bind(1, command_id);
    update.bind(2, status);
    update.bind(3, now);
    update.bind(4, result.dump());
    if (error.empty()) update.bind_null(5); else update.bind(5, error);
    update.done();

    if (status == "SUCCEEDED" && action == "QUARANTINE_ITEM" &&
        result.contains("item_tree") && result["item_tree"].is_object()) {
        const auto item_id = result["item_tree"].value("item_id", "");
        if (!item_id.empty()) {
            Statement quarantine(writer_, R"SQL(
INSERT INTO player_quarantine(quarantine_id,command_id,player_id,item_id,tree_json,created_ms)
VALUES(?1,?1,?2,?3,?4::jsonb,?5)
ON CONFLICT(quarantine_id) DO NOTHING
)SQL");
            quarantine.bind(1, command_id);
            quarantine.bind(2, player_id);
            quarantine.bind(3, item_id);
            quarantine.bind(4, result["item_tree"].dump());
            quarantine.bind(5, now);
            quarantine.done();
        }
    }
    if (status == "SUCCEEDED" && action == "RESTORE_QUARANTINE") {
        const auto quarantine_id = payload.value("quarantine_id", "");
        if (!quarantine_id.empty()) {
            Statement restored(writer_,
                "UPDATE player_quarantine SET restored_command_id=?2,restored_ms=?3 "
                "WHERE quarantine_id=?1 AND restored_ms IS NULL");
            restored.bind(1, quarantine_id);
            restored.bind(2, command_id);
            restored.bind(3, now);
            restored.done();
        }
    }

    Statement event(writer_,
        "INSERT INTO player_events(player_id,event_type,detail_json,created_ms) VALUES(?1,?2,?3::jsonb,?4)");
    event.bind(1, player_id);
    event.bind(2, std::string(status == "SUCCEEDED" ? "COMMAND_SUCCEEDED" : "COMMAND_FAILED"));
    event.bind(3, json{{"command_id",command_id},{"action",action},{"error",error}}.dump());
    event.bind(4, now);
    event.done();

    Statement audit(writer_, R"SQL(
INSERT INTO admin_audit_events(
    admin_session_id,windows_identity,action,target_type,target_id,result,reason,error,
    request_id,change_id,detail_json,created_ms
) VALUES(?1,?2,'player_command_result','player',?3,?4,?5,?6,?7,NULL,?8::jsonb,?9)
)SQL");
    audit.bind(1, admin_session_id);
    audit.bind(2, windows_identity);
    audit.bind(3, player_id);
    audit.bind(4, std::string(status == "SUCCEEDED" ? "SUCCESS" : "FAILURE"));
    audit.bind(5, reason);
    if (error.empty()) audit.bind_null(6); else audit.bind(6, error);
    audit.bind(7, request_id);
    audit.bind(8, json{{"command_id",command_id},{"command_action",action},{"command_status",status}}.dump());
    audit.bind(9, now);
    audit.done();

    transaction.commit();
    return {{"command_id", command_id}, {"status", status}, {"completed_ms", now}};
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
    const auto now = now_unix_ms();
    const auto cutoff = now -
        static_cast<std::int64_t>(config_.terminal_retention_days) * 24 * 60 * 60 * 1000;
    const auto player_cutoff = now -
        static_cast<std::int64_t>(config_.player_telemetry_retention_days) * 24 * 60 * 60 * 1000;
    const auto admin_cutoff = now -
        static_cast<std::int64_t>(config_.admin_audit_retention_days) * 24 * 60 * 60 * 1000;
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

    Statement player_event_delete(writer_, R"SQL(
DELETE FROM player_events WHERE event_id IN (
    SELECT event_id FROM player_events WHERE created_ms<?1 ORDER BY event_id LIMIT ?2
))SQL");
    player_event_delete.bind(1, player_cutoff); player_event_delete.bind(2, batch); player_event_delete.done();

    Statement player_snapshot_delete(writer_, R"SQL(
DELETE FROM player_inventory_snapshots WHERE snapshot_id IN (
    SELECT snapshot_id FROM player_inventory_snapshots WHERE captured_ms<?1
    ORDER BY captured_ms,snapshot_id LIMIT ?2
))SQL");
    player_snapshot_delete.bind(1, player_cutoff); player_snapshot_delete.bind(2, batch); player_snapshot_delete.done();

    Statement alias_delete(writer_, R"SQL(
DELETE FROM player_aliases WHERE (player_id,display_name) IN (
    SELECT a.player_id,a.display_name FROM player_aliases a
    JOIN players p ON p.player_id=a.player_id
    WHERE a.last_seen_ms<?1 AND a.display_name<>p.display_name
    ORDER BY a.last_seen_ms,a.player_id,a.display_name LIMIT ?2
)
)SQL");
    alias_delete.bind(1, player_cutoff); alias_delete.bind(2, batch); alias_delete.done();

    Statement admin_audit_delete(writer_, R"SQL(
DELETE FROM admin_audit_events WHERE event_id IN (
    SELECT event_id FROM admin_audit_events WHERE created_ms<?1
    ORDER BY created_ms,event_id LIMIT ?2
)
)SQL");
    admin_audit_delete.bind(1, admin_cutoff); admin_audit_delete.bind(2, batch); admin_audit_delete.done();

    Statement command_delete(writer_, R"SQL(
DELETE FROM admin_player_commands WHERE command_id IN (
    SELECT c.command_id FROM admin_player_commands c
    WHERE c.created_ms<?1 AND c.status IN ('SUCCEEDED','FAILED','EXPIRED')
      AND NOT EXISTS(SELECT 1 FROM player_quarantine q WHERE q.command_id=c.command_id OR q.restored_command_id=c.command_id)
    ORDER BY c.created_ms,c.command_id LIMIT ?2
)
)SQL");
    command_delete.bind(1, admin_cutoff); command_delete.bind(2, batch); command_delete.done();

    Statement command_expire(writer_, R"SQL(
UPDATE admin_player_commands
SET status='EXPIRED',completed_ms=?1,error=COALESCE(error,'Command expired during maintenance.')
WHERE status IN ('PENDING','CLAIMED') AND expires_ms<?1
)SQL");
    command_expire.bind(1, now); command_expire.done();

    // cargo_migrations and cargo_migration_roots are permanent dedupe
    // tombstones. Deleting them could let a rolled-back DayZ hive resurrect and
    // re-import physical roots that PostgreSQL has already committed.
    transaction.commit();
}

} // namespace clippy
