#include "admin_database.hpp"

#include "util.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace clippy_admin {

using nlohmann::json;
using clippy::Statement;

namespace {

json page_result(json rows, const std::string& next_after = {}) {
    json result = {{"rows", std::move(rows)}};
    if (!next_after.empty()) result["next_after"] = next_after;
    return result;
}

bool item_index_complete(clippy::PgPool* database) {
    Statement exists(database, "SELECT to_regclass('clippy.cargo_item_index') IS NOT NULL AND to_regclass('clippy.cargo_item_index_state') IS NOT NULL");
    if (!exists.row() || exists.text(0) != "t") return false;
    Statement state(database, "SELECT complete FROM cargo_item_index_state WHERE state_id=1");
    return state.row() && state.text(0) == "t";
}

std::string escape_like_prefix(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (const char c : value) {
        if (c == '\\' || c == '%' || c == '_') escaped.push_back('\\');
        escaped.push_back(c);
    }
    return escaped;
}

const json* find_item_node(const json& node, const std::string& item_id, int depth,
                           int& found_depth, std::string& parent_id) {
    if (!node.is_object()) return nullptr;
    if (node.value("item_id", "") == item_id) {
        found_depth = depth;
        return &node;
    }
    if (!node.contains("children") || !node["children"].is_array()) return nullptr;
    const auto current_id = node.value("item_id", "");
    for (const auto& child : node["children"]) {
        std::string nested_parent;
        int nested_depth = -1;
        if (const auto* found = find_item_node(child, item_id, depth + 1, nested_depth, nested_parent)) {
            found_depth = nested_depth;
            parent_id = nested_parent.empty() ? current_id : nested_parent;
            return found;
        }
    }
    return nullptr;
}

} // namespace

AdminDatabase::AdminDatabase(const AdminConfig& config)
    : pool_(std::make_unique<clippy::PgPool>(config.postgres)), gate_(*pool_) {}

json AdminDatabase::health() {
    std::lock_guard lock(gate_);
    Statement query(pool_.get(), "SELECT current_database(),current_user,current_setting('transaction_read_only'),1");
    if (!query.row()) throw std::runtime_error("PostgreSQL health query returned no row.");
    return {
        {"database", query.text(0)},
        {"role", query.text(1)},
        {"transaction_read_only", query.text(2) == "on"},
        {"postgres_version", pool_->server_version_text()},
        {"pool_size", static_cast<std::int64_t>(pool_->size())},
        {"item_index_complete", item_index_complete(pool_.get())}
    };
}

json AdminDatabase::overview() {
    std::lock_guard lock(gate_);
    Statement query(pool_.get(), R"SQL(
SELECT
  pg_database_size(current_database()),
  COALESCE((SELECT reltuples::bigint FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname='clippy' AND c.relname='storage_containers'),0),
  COALESCE((SELECT reltuples::bigint FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname='clippy' AND c.relname='cargo_roots'),0),
  (SELECT count(*) FROM cargo_sessions WHERE status IN ('OPEN','MATERIALIZED','COMMITTED')),
  (SELECT count(*) FROM operations WHERE status IN ('PREPARED','QUARANTINED') OR cleanup_state='PENDING'),
  (SELECT count(*) FROM cargo_migrations WHERE status IN ('PREPARED','COMMITTED')),
  (SELECT count(*) FROM operation_cleanup_roots WHERE cleaned=0) +
    (SELECT count(*) FROM cargo_session_cleanup_roots WHERE cleaned=0) +
    (SELECT count(*) FROM cargo_migration_roots WHERE cleaned=0),
  COALESCE((SELECT max(version) FROM schema_migrations),0)
)SQL");
    if (!query.row()) throw std::runtime_error("Overview query returned no row.");
    return {
        {"database_size_bytes", query.integer(0)},
        {"containers_estimated", query.integer(1)},
        {"roots_estimated", query.integer(2)},
        {"active_sessions", query.integer(3)},
        {"incomplete_operations", query.integer(4)},
        {"incomplete_migrations", query.integer(5)},
        {"pending_cleanup", query.integer(6)},
        {"schema_version", query.integer(7)},
        {"item_index_complete", item_index_complete(pool_.get())}
    };
}

json AdminDatabase::containers(const std::string& search, const std::string& after, int limit) {
    std::lock_guard lock(gate_);
    Statement query(pool_.get(), R"SQL(
SELECT c.storage_id,c.provider_id,c.provider_key,c.display_name,c.capacity_slots,c.revision,c.created_ms,c.updated_ms,
       (SELECT count(*) FROM cargo_roots r WHERE r.storage_id=c.storage_id) AS root_count,
       COALESCE((SELECT sum(node_count) FROM cargo_roots r WHERE r.storage_id=c.storage_id),0) AS node_count,
       EXISTS(SELECT 1 FROM cargo_sessions s WHERE s.storage_id=c.storage_id AND s.status IN ('OPEN','MATERIALIZED','COMMITTED')),
       EXISTS(SELECT 1 FROM operations o WHERE o.storage_id=c.storage_id AND (o.status IN ('PREPARED','QUARANTINED') OR o.cleanup_state='PENDING')),
       EXISTS(SELECT 1 FROM cargo_migrations m WHERE m.storage_id=c.storage_id AND m.status IN ('PREPARED','COMMITTED'))
FROM storage_containers c
WHERE c.storage_id>?2 AND (
  ?1='' OR c.storage_id ILIKE '%'||?1||'%' OR c.provider_key ILIKE '%'||?1||'%' OR c.display_name ILIKE '%'||?1||'%'
)
ORDER BY c.storage_id
LIMIT ?3
)SQL");
    query.bind(1, search);
    query.bind(2, after);
    query.bind(3, static_cast<std::int64_t>(limit + 1));
    json rows = json::array();
    std::string next;
    while (query.row()) {
        if (rows.size() == static_cast<std::size_t>(limit)) {
            next = rows.back()["storage_id"].get<std::string>();
            break;
        }
        rows.push_back({
            {"storage_id", query.text(0)}, {"provider_id", query.text(1)}, {"provider_key", query.text(2)},
            {"display_name", query.text(3)}, {"capacity_slots", query.integer(4)}, {"revision", query.integer(5)},
            {"created_ms", query.integer(6)}, {"updated_ms", query.integer(7)}, {"root_count", query.integer(8)},
            {"node_count", query.integer(9)}, {"active_session", query.text(10) == "t"},
            {"active_operation", query.text(11) == "t"}, {"active_migration", query.text(12) == "t"}
        });
    }
    return page_result(std::move(rows), next);
}

json AdminDatabase::container(const std::string& storage_id) {
    std::lock_guard lock(gate_);
    Statement query(pool_.get(), R"SQL(
SELECT c.storage_id,c.provider_id,c.provider_key,c.display_name,c.capacity_slots,c.revision,c.created_ms,c.updated_ms,
       (SELECT count(*) FROM cargo_roots r WHERE r.storage_id=c.storage_id),
       COALESCE((SELECT sum(node_count) FROM cargo_roots r WHERE r.storage_id=c.storage_id),0),
       COALESCE((SELECT jsonb_agg(jsonb_build_object('session_id',s.session_id,'status',s.status,'player_id',s.player_id,'updated_ms',s.updated_ms) ORDER BY s.updated_ms DESC)
                 FROM cargo_sessions s WHERE s.storage_id=c.storage_id AND s.status IN ('OPEN','MATERIALIZED','COMMITTED')),'[]'::jsonb)::text,
       COALESCE((SELECT jsonb_agg(jsonb_build_object('operation_id',o.operation_id,'kind',o.kind,'status',o.status,'cleanup_state',o.cleanup_state,'updated_ms',o.updated_ms) ORDER BY o.updated_ms DESC)
                 FROM operations o WHERE o.storage_id=c.storage_id AND (o.status IN ('PREPARED','QUARANTINED') OR o.cleanup_state='PENDING')),'[]'::jsonb)::text,
       COALESCE((SELECT jsonb_agg(jsonb_build_object('migration_id',m.migration_id,'status',m.status,'container_class',m.container_class,'updated_ms',m.updated_ms) ORDER BY m.updated_ms DESC)
                 FROM cargo_migrations m WHERE m.storage_id=c.storage_id AND m.status IN ('PREPARED','COMMITTED')),'[]'::jsonb)::text
FROM storage_containers c WHERE c.storage_id=?1
)SQL");
    query.bind(1, storage_id);
    if (!query.row()) throw clippy::ApiError(404, "storage_not_found", "The storage container does not exist.");
    return {
        {"storage_id", query.text(0)}, {"provider_id", query.text(1)}, {"provider_key", query.text(2)},
        {"display_name", query.text(3)}, {"capacity_slots", query.integer(4)}, {"revision", query.integer(5)},
        {"created_ms", query.integer(6)}, {"updated_ms", query.integer(7)}, {"root_count", query.integer(8)},
        {"node_count", query.integer(9)}, {"active_sessions", json::parse(query.text(10))},
        {"active_operations", json::parse(query.text(11))}, {"active_migrations", json::parse(query.text(12))}
    };
}

json AdminDatabase::roots(const std::string& storage_id, const std::string& after, int limit) {
    std::lock_guard lock(gate_);
    Statement query(pool_.get(), R"SQL(
SELECT root_item_id,class_name,quantity,health,state_json::text,node_count,created_ms
FROM cargo_roots WHERE storage_id=?1 AND root_item_id>?2 ORDER BY root_item_id LIMIT ?3
)SQL");
    query.bind(1, storage_id);
    query.bind(2, after);
    query.bind(3, static_cast<std::int64_t>(limit + 1));
    json rows = json::array();
    std::string next;
    while (query.row()) {
        if (rows.size() == static_cast<std::size_t>(limit)) {
            next = rows.back()["root_item_id"].get<std::string>();
            break;
        }
        rows.push_back({
            {"root_item_id", query.text(0)}, {"class_name", query.text(1)}, {"quantity", query.number(2)},
            {"health", query.number(3)}, {"state", json::parse(query.text(4))}, {"node_count", query.integer(5)},
            {"created_ms", query.integer(6)}
        });
    }
    return page_result(std::move(rows), next);
}

json AdminDatabase::tree(const std::string& storage_id, const std::string& root_item_id) {
    std::lock_guard lock(gate_);
    Statement query(pool_.get(), "SELECT tree_json::text FROM cargo_roots WHERE storage_id=?1 AND root_item_id=?2");
    query.bind(1, storage_id);
    query.bind(2, root_item_id);
    if (!query.row()) throw clippy::ApiError(404, "item_not_found", "The virtual root item does not exist.");
    return json{{"tree", json::parse(query.text(0))}};
}

json AdminDatabase::search_items(const std::string& raw_query,
                                 const std::string& after_class,
                                 const std::string& after_storage,
                                 const std::string& after_root,
                                 const std::string& after_item,
                                 double min_quantity,
                                 double max_quantity,
                                 double min_health,
                                 double max_health,
                                 int limit) {
    std::lock_guard lock(gate_);
    std::string query_text = raw_query;
    bool exact_item_id = false;
    if (query_text.rfind("id:", 0) == 0) {
        exact_item_id = true;
        query_text.erase(0, 3);
    } else if (query_text.rfind("item:", 0) == 0) {
        query_text.erase(0, 5);
    }
    if (query_text.empty()) {
        return json{{"rows", json::array()}, {"nested_class_search_available", item_index_complete(pool_.get())}};
    }

    const bool index_ready = item_index_complete(pool_.get());
    json rows = json::array();
    std::string next_class;
    std::string next_storage;
    std::string next_root;
    std::string next_item;

    if (index_ready && exact_item_id) {
        Statement query(pool_.get(), R"SQL(
SELECT storage_id,root_item_id,item_id,COALESCE(parent_item_id,''),depth,class_name,quantity,health,
       adapter_id,location_type,updated_ms
FROM cargo_item_index
WHERE item_id=?1
  AND quantity>=?2 AND quantity<=?3
  AND health>=?4 AND health<=?5
  AND (storage_id,root_item_id,item_id)>(?6,?7,?8)
ORDER BY storage_id,root_item_id,item_id
LIMIT ?9
)SQL");
        query.bind(1, query_text);
        query.bind(2, min_quantity);
        query.bind(3, max_quantity);
        query.bind(4, min_health);
        query.bind(5, max_health);
        query.bind(6, after_storage);
        query.bind(7, after_root);
        query.bind(8, after_item);
        query.bind(9, static_cast<std::int64_t>(limit + 1));
        while (query.row()) {
            if (rows.size() == static_cast<std::size_t>(limit)) {
                next_storage = rows.back()["storage_id"].get<std::string>();
                next_root = rows.back()["root_item_id"].get<std::string>();
                next_item = rows.back()["item_id"].get<std::string>();
                break;
            }
            rows.push_back({
                {"storage_id", query.text(0)}, {"root_item_id", query.text(1)}, {"item_id", query.text(2)},
                {"parent_item_id", query.text(3)}, {"depth", query.integer(4)}, {"class_name", query.text(5)},
                {"quantity", query.number(6)}, {"health", query.number(7)}, {"adapter_id", query.text(8)},
                {"location_type", query.text(9)}, {"updated_ms", query.integer(10)}, {"scope", "item_index"}
            });
        }
    } else if (index_ready) {
        const auto prefix = escape_like_prefix(query_text);
        Statement query(pool_.get(), R"SQL(
SELECT storage_id,root_item_id,item_id,COALESCE(parent_item_id,''),depth,class_name,quantity,health,
       adapter_id,location_type,updated_ms
FROM cargo_item_index
WHERE lower(class_name) LIKE lower(?1)||'%' ESCAPE E'\\'
  AND quantity>=?2 AND quantity<=?3
  AND health>=?4 AND health<=?5
  AND (lower(class_name),storage_id,root_item_id,item_id)>(lower(?6),?7,?8,?9)
ORDER BY lower(class_name),storage_id,root_item_id,item_id
LIMIT ?10
)SQL");
        query.bind(1, prefix);
        query.bind(2, min_quantity);
        query.bind(3, max_quantity);
        query.bind(4, min_health);
        query.bind(5, max_health);
        query.bind(6, after_class);
        query.bind(7, after_storage);
        query.bind(8, after_root);
        query.bind(9, after_item);
        query.bind(10, static_cast<std::int64_t>(limit + 1));
        while (query.row()) {
            if (rows.size() == static_cast<std::size_t>(limit)) {
                next_class = rows.back()["class_name"].get<std::string>();
                next_storage = rows.back()["storage_id"].get<std::string>();
                next_root = rows.back()["root_item_id"].get<std::string>();
                next_item = rows.back()["item_id"].get<std::string>();
                break;
            }
            rows.push_back({
                {"storage_id", query.text(0)}, {"root_item_id", query.text(1)}, {"item_id", query.text(2)},
                {"parent_item_id", query.text(3)}, {"depth", query.integer(4)}, {"class_name", query.text(5)},
                {"quantity", query.number(6)}, {"health", query.number(7)}, {"adapter_id", query.text(8)},
                {"location_type", query.text(9)}, {"updated_ms", query.integer(10)}, {"scope", "item_index"}
            });
        }
    } else if (exact_item_id) {
        Statement query(pool_.get(), R"SQL(
SELECT storage_id,root_item_id,class_name,quantity,health,node_count,created_ms,tree_json::text
FROM cargo_roots
WHERE item_ids ? ?1 AND (storage_id,root_item_id)>(?2,?3)
ORDER BY storage_id,root_item_id LIMIT ?4
)SQL");
        query.bind(1, query_text);
        query.bind(2, after_storage);
        query.bind(3, after_root);
        query.bind(4, static_cast<std::int64_t>(limit + 1));
        while (query.row()) {
            if (rows.size() == static_cast<std::size_t>(limit)) {
                next_storage = rows.back()["storage_id"].get<std::string>();
                next_root = rows.back()["root_item_id"].get<std::string>();
                next_item = rows.back()["item_id"].get<std::string>();
                break;
            }
            auto tree_json = json::parse(query.text(7));
            int depth = -1;
            std::string parent;
            const auto* node = find_item_node(tree_json, query_text, 0, depth, parent);
            if (!node) continue;
            const double quantity = node->value("quantity", 0.0);
            const double health = node->value("health", 0.0);
            if (quantity < min_quantity || quantity > max_quantity || health < min_health || health > max_health) continue;
            const auto adapter = node->contains("adapter") && (*node)["adapter"].is_object()
                ? (*node)["adapter"].value("id", "") : "";
            const auto location = node->contains("location") && (*node)["location"].is_object()
                ? (*node)["location"].value("kind", "") : "";
            rows.push_back({
                {"storage_id", query.text(0)}, {"root_item_id", query.text(1)}, {"item_id", query_text},
                {"parent_item_id", parent}, {"depth", depth}, {"class_name", node->value("class_name", "")},
                {"quantity", quantity}, {"health", health}, {"adapter_id", adapter}, {"location_type", location},
                {"updated_ms", query.integer(6)}, {"scope", "exact_item_id_fallback"}
            });
        }
    } else {
        Statement query(pool_.get(), R"SQL(
SELECT storage_id,root_item_id,class_name,quantity,health,node_count,created_ms
FROM cargo_roots
WHERE lower(class_name) LIKE lower(?1)||'%' ESCAPE E'\\'
  AND quantity>=?2 AND quantity<=?3
  AND health>=?4 AND health<=?5
  AND (storage_id,root_item_id)>(?6,?7)
ORDER BY storage_id,root_item_id LIMIT ?8
)SQL");
        query.bind(1, escape_like_prefix(query_text));
        query.bind(2, min_quantity);
        query.bind(3, max_quantity);
        query.bind(4, min_health);
        query.bind(5, max_health);
        query.bind(6, after_storage);
        query.bind(7, after_root);
        query.bind(8, static_cast<std::int64_t>(limit + 1));
        while (query.row()) {
            if (rows.size() == static_cast<std::size_t>(limit)) {
                next_storage = rows.back()["storage_id"].get<std::string>();
                next_root = rows.back()["root_item_id"].get<std::string>();
                next_item = rows.back()["item_id"].get<std::string>();
                break;
            }
            rows.push_back({
                {"storage_id", query.text(0)}, {"root_item_id", query.text(1)}, {"item_id", query.text(1)},
                {"parent_item_id", ""}, {"depth", 0}, {"class_name", query.text(2)},
                {"quantity", query.number(3)}, {"health", query.number(4)}, {"updated_ms", query.integer(6)},
                {"adapter_id", ""}, {"location_type", ""}, {"scope", "root_class_fallback"}
            });
        }
    }

    json result = {
        {"rows", std::move(rows)},
        {"nested_class_search_available", index_ready},
        {"item_index_complete", index_ready}
    };
    if (!next_storage.empty()) {
        if (!next_class.empty()) result["next_after_class"] = next_class;
        result["next_after_storage"] = next_storage;
        result["next_after_root"] = next_root;
        result["next_after_item"] = next_item;
    }
    return result;
}

json AdminDatabase::sessions(std::int64_t before_ms, const std::string& before_id, int limit) {
    std::lock_guard lock(gate_);
    if (before_ms <= 0) before_ms = (std::numeric_limits<std::int64_t>::max)();
    const std::string cursor_id = before_id.empty() ? std::string(256, '~') : before_id;
    Statement query(pool_.get(), R"SQL(
SELECT s.session_id,s.storage_id,c.display_name,s.player_id,s.status,s.expected_revision,s.cursor,s.next_cursor,
       s.created_ms,s.updated_ms,s.result_revision,s.error,
       COALESCE((SELECT count(*) FROM cargo_session_cleanup_roots r WHERE r.session_id=s.session_id AND r.cleaned=0),0)
FROM cargo_sessions s JOIN storage_containers c ON c.storage_id=s.storage_id
WHERE (s.updated_ms,s.session_id)<(?1,?2)
ORDER BY s.updated_ms DESC,s.session_id DESC LIMIT ?3
)SQL");
    query.bind(1, before_ms);
    query.bind(2, cursor_id);
    query.bind(3, static_cast<std::int64_t>(limit + 1));
    json rows = json::array();
    std::int64_t next_ms = 0;
    std::string next_id;
    while (query.row()) {
        if (rows.size() == static_cast<std::size_t>(limit)) {
            next_ms = rows.back()["updated_ms"].get<std::int64_t>();
            next_id = rows.back()["session_id"].get<std::string>();
            break;
        }
        json row = {
            {"session_id", query.text(0)}, {"storage_id", query.text(1)}, {"container", query.text(2)},
            {"player_id", query.text(3)}, {"status", query.text(4)}, {"expected_revision", query.integer(5)},
            {"cursor", query.text(6)}, {"next_cursor", query.text(7)}, {"created_ms", query.integer(8)},
            {"updated_ms", query.integer(9)}, {"pending_cleanup", query.integer(12)}
        };
        if (!query.is_null(10)) row["result_revision"] = query.integer(10);
        if (!query.is_null(11)) row["error"] = query.text(11);
        rows.push_back(std::move(row));
    }
    json result = {{"rows", std::move(rows)}};
    if (!next_id.empty()) {
        result["next_before_ms"] = next_ms;
        result["next_before_id"] = next_id;
    }
    return result;
}

json AdminDatabase::recovery() {
    std::lock_guard lock(gate_);
    json result;

    Statement operations(pool_.get(), R"SQL(
SELECT operation_id,kind,status,cleanup_state,storage_id,root_item_id,expected_revision,created_ms,updated_ms,error
FROM operations WHERE status IN ('PREPARED','QUARANTINED') OR cleanup_state='PENDING'
ORDER BY created_ms LIMIT 100
)SQL");
    result["operations"] = json::array();
    while (operations.row()) {
        json row = {
            {"operation_id", operations.text(0)}, {"kind", operations.text(1)}, {"status", operations.text(2)},
            {"cleanup_state", operations.text(3)}, {"storage_id", operations.text(4)}, {"root_item_id", operations.text(5)},
            {"expected_revision", operations.integer(6)}, {"created_ms", operations.integer(7)}, {"updated_ms", operations.integer(8)}
        };
        if (!operations.is_null(9)) row["error"] = operations.text(9);
        result["operations"].push_back(std::move(row));
    }

    Statement sessions(pool_.get(), R"SQL(
SELECT session_id,storage_id,player_id,status,expected_revision,created_ms,updated_ms,error
FROM cargo_sessions WHERE status IN ('OPEN','MATERIALIZED','COMMITTED') ORDER BY created_ms LIMIT 100
)SQL");
    result["sessions"] = json::array();
    while (sessions.row()) {
        json row = {
            {"session_id", sessions.text(0)}, {"storage_id", sessions.text(1)}, {"player_id", sessions.text(2)},
            {"status", sessions.text(3)}, {"expected_revision", sessions.integer(4)},
            {"created_ms", sessions.integer(5)}, {"updated_ms", sessions.integer(6)}
        };
        if (!sessions.is_null(7)) row["error"] = sessions.text(7);
        result["sessions"].push_back(std::move(row));
    }

    Statement migrations(pool_.get(), R"SQL(
SELECT migration_id,storage_id,container_class,status,expected_revision,created_ms,updated_ms,error
FROM cargo_migrations WHERE status IN ('PREPARED','COMMITTED') ORDER BY created_ms LIMIT 100
)SQL");
    result["migrations"] = json::array();
    while (migrations.row()) {
        json row = {
            {"migration_id", migrations.text(0)}, {"storage_id", migrations.text(1)}, {"container_class", migrations.text(2)},
            {"status", migrations.text(3)}, {"expected_revision", migrations.integer(4)},
            {"created_ms", migrations.integer(5)}, {"updated_ms", migrations.integer(6)}
        };
        if (!migrations.is_null(7)) row["error"] = migrations.text(7);
        result["migrations"].push_back(std::move(row));
    }

    Statement cleanup(pool_.get(), R"SQL(
SELECT
 (SELECT count(*) FROM operation_cleanup_roots WHERE cleaned=0),
 (SELECT count(*) FROM cargo_session_cleanup_roots WHERE cleaned=0),
 (SELECT count(*) FROM cargo_migration_roots WHERE cleaned=0)
)SQL");
    cleanup.row();
    result["pending_cleanup"] = {
        {"operations", cleanup.integer(0)}, {"sessions", cleanup.integer(1)}, {"migrations", cleanup.integer(2)}
    };
    result["truncated"] = result["operations"].size() >= 100 || result["sessions"].size() >= 100 || result["migrations"].size() >= 100;
    return result;
}

json AdminDatabase::database_info() {
    std::lock_guard lock(gate_);
    json result;
    Statement info(pool_.get(), R"SQL(
SELECT current_database(),current_user,pg_database_size(current_database()),
       COALESCE((SELECT max(version) FROM schema_migrations),0),
       current_setting('transaction_read_only'),
       (SELECT numbackends FROM pg_stat_database WHERE datname=current_database())
)SQL");
    if (!info.row()) throw std::runtime_error("Database information query returned no row.");
    result["database"] = info.text(0);
    result["role"] = info.text(1);
    result["size_bytes"] = info.integer(2);
    result["schema_version"] = info.integer(3);
    result["transaction_read_only"] = info.text(4) == "on";
    result["connections"] = info.integer(5);
    result["postgres_version"] = pool_->server_version_text();
    result["item_index_complete"] = item_index_complete(pool_.get());

    Statement tables(pool_.get(), R"SQL(
SELECT c.relname,COALESCE(s.n_live_tup,0),pg_total_relation_size(c.oid),pg_relation_size(c.oid),
       pg_indexes_size(c.oid)
FROM pg_class c
JOIN pg_namespace n ON n.oid=c.relnamespace
LEFT JOIN pg_stat_user_tables s ON s.relid=c.oid
WHERE n.nspname='clippy' AND c.relkind='r'
ORDER BY c.relname
LIMIT 100
)SQL");
    result["tables"] = json::array();
    while (tables.row()) {
        result["tables"].push_back({
            {"name", tables.text(0)}, {"estimated_rows", tables.integer(1)}, {"total_bytes", tables.integer(2)},
            {"table_bytes", tables.integer(3)}, {"index_bytes", tables.integer(4)}
        });
    }
    return result;
}

} // namespace clippy_admin
