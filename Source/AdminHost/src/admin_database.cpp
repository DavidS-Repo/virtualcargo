#include "admin_database.hpp"

#include "util.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
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
    : pool_(std::make_unique<clippy::PgPool>(config.postgres)), gate_(*pool_),
      editing_enabled_(config.editing_enabled), maintenance_lock_seconds_(config.maintenance_lock_seconds) {
    if (editing_enabled_) {
        writer_pool_ = std::make_unique<clippy::PgPool>(config.postgres_write);
        writer_gate_ = std::make_unique<clippy::ConnectionGate>(*writer_pool_);
    }
}

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
        {"item_index_complete", item_index_complete(pool_.get())},
        {"editing_enabled", editing_enabled_}
    };
}

json AdminDatabase::overview() {
    std::lock_guard lock(gate_);
    Statement query(pool_.get(), R"SQL(
SELECT
  pg_database_size(current_database()),
  COALESCE((SELECT reltuples::bigint FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname='clippy' AND c.relname='storage_containers'),0),
  COALESCE((SELECT reltuples::bigint FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname='clippy' AND c.relname='cargo_roots'),0),
  COALESCE((SELECT reltuples::bigint FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname='clippy' AND c.relname='cargo_item_index'),0),
  (SELECT count(*) FROM cargo_sessions WHERE status IN ('OPEN','MATERIALIZED','COMMITTED')),
  (SELECT count(*) FROM operations WHERE status IN ('PREPARED','QUARANTINED') OR cleanup_state='PENDING'),
  (SELECT count(*) FROM cargo_migrations WHERE status IN ('PREPARED','COMMITTED')),
  (SELECT count(*) FROM operation_cleanup_roots WHERE cleaned=0) +
    (SELECT count(*) FROM cargo_session_cleanup_roots WHERE cleaned=0) +
    (SELECT count(*) FROM cargo_migration_roots WHERE cleaned=0),
  COALESCE((SELECT max(version) FROM schema_migrations),0)
)SQL");
    if (!query.row()) throw std::runtime_error("Overview query returned no row.");
    Statement admin_state(pool_.get(), R"SQL(
SELECT
  COALESCE((SELECT count(*) FROM admin_container_locks WHERE expires_ms>?1),0),
  COALESCE((SELECT count(*) FROM admin_quarantine WHERE restored_ms IS NULL),0),
  COALESCE((SELECT count(*) FROM admin_change_sets WHERE status='APPLIED'),0),
  COALESCE((SELECT count(*) FROM players),0),
  COALESCE((SELECT count(*) FROM players WHERE last_seen_ms>?2),0),
  COALESCE((SELECT count(*) FROM admin_player_commands WHERE status IN ('PENDING','CLAIMED')),0),
  COALESCE((SELECT count(*) FROM player_quarantine WHERE restored_ms IS NULL),0)
)SQL");
    const auto overview_now = clippy::now_unix_ms();
    admin_state.bind(1, overview_now);
    admin_state.bind(2, overview_now - 300000);
    admin_state.row();
    return {
        {"database_size_bytes", query.integer(0)},
        {"containers_estimated", query.integer(1)},
        {"roots_estimated", query.integer(2)},
        {"item_nodes_estimated", query.integer(3)},
        {"active_sessions", query.integer(4)},
        {"incomplete_operations", query.integer(5)},
        {"incomplete_migrations", query.integer(6)},
        {"pending_cleanup", query.integer(7)},
        {"schema_version", query.integer(8)},
        {"item_index_complete", item_index_complete(pool_.get())},
        {"editing_enabled", editing_enabled_},
        {"admin_locks", admin_state.integer(0)},
        {"quarantine_items", admin_state.integer(1)},
        {"applied_admin_changes", admin_state.integer(2)},
        {"known_players", admin_state.integer(3)},
        {"recent_players", admin_state.integer(4)},
        {"pending_player_commands", admin_state.integer(5)},
        {"player_quarantine_items", admin_state.integer(6)}
    };
}

json AdminDatabase::containers(const std::string& search, const std::string& after,
                               const std::string& contains_class, const std::string& status,
                               std::int64_t min_nodes, int stale_days, int limit) {
    static const std::set<std::string> statuses = {"", "session", "recovery", "locked", "idle"};
    if (!statuses.count(status)) throw clippy::ApiError(400, "invalid_query", "status must be session, recovery, locked, idle, or empty.");
    if (min_nodes < 0 || min_nodes > 1000000000LL) throw clippy::ApiError(400, "invalid_query", "min_nodes is outside the allowed range.");
    std::lock_guard lock(gate_);
    if (!contains_class.empty() && !item_index_complete(pool_.get())) {
        throw clippy::ApiError(409, "item_index_not_ready", "Container item-class filtering requires the completed cargo item index.", true);
    }
    const auto contains_prefix = escape_like_prefix(contains_class);
    const auto stale_before_ms = stale_days > 0 ? clippy::now_unix_ms() - static_cast<std::int64_t>(stale_days) * 86400000LL : 0;
    Statement query(pool_.get(), R"SQL(
WITH candidate AS (
  SELECT c.storage_id,c.provider_id,c.provider_key,c.display_name,c.capacity_slots,c.revision,c.created_ms,c.updated_ms,
         c.container_class,c.world_position_x,c.world_position_y,c.world_position_z,c.map_name,c.first_seen_ms,c.last_seen_ms,
         (SELECT count(*) FROM cargo_roots r WHERE r.storage_id=c.storage_id) AS root_count,
         COALESCE((SELECT sum(node_count) FROM cargo_roots r WHERE r.storage_id=c.storage_id),0) AS node_count,
         EXISTS(SELECT 1 FROM cargo_sessions s WHERE s.storage_id=c.storage_id AND s.status IN ('OPEN','MATERIALIZED','COMMITTED')) AS active_session,
         EXISTS(SELECT 1 FROM operations o WHERE o.storage_id=c.storage_id AND (o.status IN ('PREPARED','QUARANTINED') OR o.cleanup_state='PENDING')) AS active_operation,
         EXISTS(SELECT 1 FROM cargo_migrations m WHERE m.storage_id=c.storage_id AND m.status IN ('PREPARED','COMMITTED')) AS active_migration,
         EXISTS(SELECT 1 FROM admin_container_locks l WHERE l.storage_id=c.storage_id AND l.expires_ms>CAST(EXTRACT(EPOCH FROM clock_timestamp())*1000 AS BIGINT)) AS admin_locked
  FROM storage_containers c
  WHERE c.storage_id>?2 AND (
    ?1='' OR c.storage_id ILIKE '%'||?1||'%' OR c.provider_key ILIKE '%'||?1||'%' OR c.display_name ILIKE '%'||?1||'%'
    OR c.container_class ILIKE '%'||?1||'%' OR c.map_name ILIKE '%'||?1||'%'
  )
)
SELECT d.storage_id,d.provider_id,d.provider_key,d.display_name,d.capacity_slots,d.revision,d.created_ms,d.updated_ms,
       d.container_class,d.world_position_x,d.world_position_y,d.world_position_z,d.map_name,d.first_seen_ms,d.last_seen_ms,
       d.root_count,d.node_count,d.active_session,d.active_operation,d.active_migration,d.admin_locked
FROM candidate d
WHERE d.node_count>=?4
  AND (?3='' OR EXISTS(
        SELECT 1 FROM cargo_item_index i WHERE i.storage_id=d.storage_id
        AND lower(i.class_name) LIKE lower(?3)||'%' ESCAPE E'\\'
  ))
  AND (?5='' OR (?5='session' AND d.active_session)
       OR (?5='recovery' AND (d.active_operation OR d.active_migration))
       OR (?5='locked' AND d.admin_locked)
       OR (?5='idle' AND NOT d.active_session AND NOT d.active_operation AND NOT d.active_migration AND NOT d.admin_locked))
  AND (?6<=0 OR COALESCE(d.last_seen_ms,d.updated_ms)<?6)
ORDER BY d.storage_id
LIMIT ?7
)SQL");
    query.bind(1, search);
    query.bind(2, after);
    query.bind(3, contains_prefix);
    query.bind(4, min_nodes);
    query.bind(5, status);
    query.bind(6, static_cast<std::int64_t>(stale_before_ms));
    query.bind(7, static_cast<std::int64_t>(limit + 1));
    json rows = json::array();
    std::string next;
    while (query.row()) {
        if (rows.size() == static_cast<std::size_t>(limit)) {
            next = rows.back()["storage_id"].get<std::string>();
            break;
        }
        json row = {
            {"storage_id", query.text(0)}, {"provider_id", query.text(1)}, {"provider_key", query.text(2)},
            {"display_name", query.text(3)}, {"capacity_slots", query.integer(4)}, {"revision", query.integer(5)},
            {"created_ms", query.integer(6)}, {"updated_ms", query.integer(7)}, {"container_class", query.text(8)},
            {"map_name", query.text(12)}, {"root_count", query.integer(15)}, {"node_count", query.integer(16)},
            {"active_session", query.text(17) == "t"}, {"active_operation", query.text(18) == "t"},
            {"active_migration", query.text(19) == "t"}, {"admin_locked", query.text(20) == "t"}
        };
        if (!query.is_null(9)) row["world_position_x"] = query.number(9);
        if (!query.is_null(10)) row["world_position_y"] = query.number(10);
        if (!query.is_null(11)) row["world_position_z"] = query.number(11);
        if (!query.is_null(13)) row["first_seen_ms"] = query.integer(13);
        if (!query.is_null(14)) row["last_seen_ms"] = query.integer(14);
        rows.push_back(std::move(row));
    }
    auto result = page_result(std::move(rows), next);
    result["item_index_complete"] = item_index_complete(pool_.get());
    return result;
}

json AdminDatabase::container(const std::string& storage_id, const std::string& admin_session_id) {
    std::lock_guard lock(gate_);
    Statement query(pool_.get(), R"SQL(
SELECT c.storage_id,c.provider_id,c.provider_key,c.display_name,c.capacity_slots,c.revision,c.created_ms,c.updated_ms,
       c.container_class,c.world_position_x,c.world_position_y,c.world_position_z,c.map_name,c.first_seen_ms,c.last_seen_ms,
       (SELECT count(*) FROM cargo_roots r WHERE r.storage_id=c.storage_id),
       COALESCE((SELECT sum(node_count) FROM cargo_roots r WHERE r.storage_id=c.storage_id),0),
       COALESCE((SELECT jsonb_agg(jsonb_build_object('session_id',s.session_id,'status',s.status,'player_id',s.player_id,'updated_ms',s.updated_ms) ORDER BY s.updated_ms DESC)
                 FROM cargo_sessions s WHERE s.storage_id=c.storage_id AND s.status IN ('OPEN','MATERIALIZED','COMMITTED')),'[]'::jsonb)::text,
       COALESCE((SELECT jsonb_agg(jsonb_build_object('operation_id',o.operation_id,'kind',o.kind,'status',o.status,'cleanup_state',o.cleanup_state,'updated_ms',o.updated_ms) ORDER BY o.updated_ms DESC)
                 FROM operations o WHERE o.storage_id=c.storage_id AND (o.status IN ('PREPARED','QUARANTINED') OR o.cleanup_state='PENDING')),'[]'::jsonb)::text,
       COALESCE((SELECT jsonb_agg(jsonb_build_object('migration_id',m.migration_id,'status',m.status,'container_class',m.container_class,'updated_ms',m.updated_ms) ORDER BY m.updated_ms DESC)
                 FROM cargo_migrations m WHERE m.storage_id=c.storage_id AND m.status IN ('PREPARED','COMMITTED')),'[]'::jsonb)::text,
       COALESCE((SELECT jsonb_build_object('lock_id',l.lock_id,'reason',l.lock_reason,'created_ms',l.created_ms,'expires_ms',l.expires_ms,
                                           'owned_by_current_session',(l.admin_session_id=?2))
                 FROM admin_container_locks l WHERE l.storage_id=c.storage_id AND l.expires_ms>CAST(EXTRACT(EPOCH FROM clock_timestamp())*1000 AS BIGINT)),'null'::jsonb)::text
FROM storage_containers c WHERE c.storage_id=?1
)SQL");
    query.bind(1, storage_id);
    query.bind(2, admin_session_id);
    if (!query.row()) throw clippy::ApiError(404, "storage_not_found", "The storage container does not exist.");
    json result = {
        {"storage_id", query.text(0)}, {"provider_id", query.text(1)}, {"provider_key", query.text(2)},
        {"display_name", query.text(3)}, {"capacity_slots", query.integer(4)}, {"revision", query.integer(5)},
        {"created_ms", query.integer(6)}, {"updated_ms", query.integer(7)}, {"container_class", query.text(8)},
        {"map_name", query.text(12)}, {"root_count", query.integer(15)}, {"node_count", query.integer(16)},
        {"active_sessions", json::parse(query.text(17))}, {"active_operations", json::parse(query.text(18))},
        {"active_migrations", json::parse(query.text(19))}, {"admin_lock", json::parse(query.text(20))},
        {"editing_enabled", editing_enabled_}
    };
    if (!query.is_null(9)) result["world_position_x"] = query.number(9);
    if (!query.is_null(10)) result["world_position_y"] = query.number(10);
    if (!query.is_null(11)) result["world_position_z"] = query.number(11);
    if (!query.is_null(13)) result["first_seen_ms"] = query.integer(13);
    if (!query.is_null(14)) result["last_seen_ms"] = query.integer(14);
    return result;
}

json AdminDatabase::locks(std::int64_t before_expiry_ms, const std::string& before_storage_id, int limit) {
    std::lock_guard lock(gate_);
    const auto now = clippy::now_unix_ms();
    if (before_expiry_ms <= 0) before_expiry_ms = (std::numeric_limits<std::int64_t>::max)();
    const auto cursor = before_storage_id.empty() ? std::string(128, '~') : before_storage_id;
    Statement query(pool_.get(), R"SQL(
SELECT l.storage_id,c.display_name,l.lock_reason,l.created_ms,l.expires_ms
FROM admin_container_locks l JOIN storage_containers c ON c.storage_id=l.storage_id
WHERE l.expires_ms>?1 AND (l.expires_ms,l.storage_id)<(?2,?3)
ORDER BY l.expires_ms DESC,l.storage_id DESC LIMIT ?4
)SQL");
    query.bind(1, now);
    query.bind(2, before_expiry_ms);
    query.bind(3, cursor);
    query.bind(4, static_cast<std::int64_t>(limit + 1));
    json rows = json::array();
    std::int64_t next_expiry = 0;
    std::string next_storage;
    while (query.row()) {
        if (rows.size() == static_cast<std::size_t>(limit)) {
            next_expiry = rows.back()["expires_ms"].get<std::int64_t>();
            next_storage = rows.back()["storage_id"].get<std::string>();
            break;
        }
        rows.push_back({{"storage_id",query.text(0)},{"display_name",query.text(1)},{"reason",query.text(2)},
                        {"created_ms",query.integer(3)},{"expires_ms",query.integer(4)}});
    }
    json result={{"rows",std::move(rows)}};
    if (!next_storage.empty()) { result["next_before_expiry_ms"]=next_expiry; result["next_before_storage_id"]=next_storage; }
    return result;
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

json AdminDatabase::export_container(const std::string& storage_id, const std::filesystem::path& export_directory) {
    constexpr std::int64_t batch_size = 50;
    constexpr std::int64_t maximum_roots = 250000;
    constexpr std::uintmax_t maximum_bytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;

    std::error_code error;
    std::filesystem::create_directories(export_directory, error);
    if (error) throw clippy::ApiError(500, "export_directory_failed", "The admin export directory could not be created.");

    std::lock_guard lock(gate_);
    Statement container_query(pool_.get(), "SELECT display_name,revision FROM storage_containers WHERE storage_id=?1");
    container_query.bind(1, storage_id);
    if (!container_query.row()) throw clippy::ApiError(404, "storage_not_found", "The storage container does not exist.");
    const auto display_name = container_query.text(0);
    const auto revision = container_query.integer(1);

    const auto created_ms = clippy::now_unix_ms();
    const auto base = std::string("ClippyContainer-") + std::to_string(created_ms) + "-" + clippy::random_hex(4);
    const auto final_path = export_directory / (base + ".jsonl");
    const auto partial_path = export_directory / (base + ".partial");
    std::ofstream output(partial_path, std::ios::binary | std::ios::trunc);
    if (!output) throw clippy::ApiError(500, "export_open_failed", "The container export file could not be created.");

    auto cleanup_partial = [&] {
        output.close();
        std::error_code ignored;
        std::filesystem::remove(partial_path, ignored);
    };

    try {
        output << json{{"type","clippy_virtual_cargo_container_export"},{"format_version",1},{"storage_id",storage_id},
                       {"display_name",display_name},{"revision",revision},{"created_ms",created_ms}}.dump() << '\n';
        std::string after;
        std::int64_t roots = 0;
        std::int64_t nodes = 0;
        for (;;) {
            Statement batch(pool_.get(), R"SQL(
SELECT root_item_id,tree_json::text,node_count
FROM cargo_roots WHERE storage_id=?1 AND root_item_id>?2
ORDER BY root_item_id LIMIT ?3
)SQL");
            batch.bind(1, storage_id);
            batch.bind(2, after);
            batch.bind(3, batch_size);
            std::int64_t fetched = 0;
            std::string last;
            while (batch.row()) {
                ++fetched; ++roots;
                if (roots > maximum_roots) throw clippy::ApiError(413, "export_too_large", "The container has too many virtual roots for one admin export.");
                last = batch.text(0);
                nodes += batch.integer(2);
                output << "{\"type\":\"root\",\"root_item_id\":" << json(last).dump()
                       << ",\"tree\":" << batch.text(1) << "}\n";
                if (!output) throw clippy::ApiError(500, "export_write_failed", "The container export could not be written.");
                const auto position = output.tellp();
                if (position < 0 || static_cast<std::uintmax_t>(position) > maximum_bytes) {
                    throw clippy::ApiError(413, "export_too_large", "The container export exceeded the 2 GB safety limit.");
                }
            }
            if (fetched == 0) break;
            after = std::move(last);
            if (fetched < batch_size) break;
        }
        output.flush();
        if (!output) throw clippy::ApiError(500, "export_write_failed", "The container export could not be finalized.");
        output.close();
        std::filesystem::rename(partial_path, final_path, error);
        if (error) throw clippy::ApiError(500, "export_finalize_failed", "The container export could not be finalized.");
        return {{"path",final_path.string()},{"file",final_path.filename().string()},{"storage_id",storage_id},
                {"revision",revision},{"roots",roots},{"nodes",nodes},
                {"bytes",static_cast<std::int64_t>(std::filesystem::file_size(final_path))},{"format","jsonl"}};
    } catch (...) {
        cleanup_partial();
        throw;
    }
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
                                 const std::string& adapter_filter,
                                 const std::string& location_filter,
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
    const bool index_ready = item_index_complete(pool_.get());
    json rows = json::array();
    std::string next_class;
    std::string next_storage;
    std::string next_root;
    std::string next_item;

    if (index_ready && exact_item_id) {
        Statement query(pool_.get(), R"SQL(
SELECT i.storage_id,i.root_item_id,i.item_id,COALESCE(i.parent_item_id,''),i.depth,i.class_name,i.quantity,i.health,
       i.adapter_id,i.location_type,i.updated_ms,c.revision
FROM cargo_item_index i JOIN storage_containers c ON c.storage_id=i.storage_id
WHERE i.item_id=?1
  AND i.quantity>=?2 AND i.quantity<=?3
  AND i.health>=?4 AND i.health<=?5
  AND (?6='' OR i.adapter_id=?6)
  AND (?7='' OR i.location_type=?7)
  AND (i.storage_id,i.root_item_id,i.item_id)>(?8,?9,?10)
ORDER BY i.storage_id,i.root_item_id,i.item_id
LIMIT ?11
)SQL");
        query.bind(1, query_text);
        query.bind(2, min_quantity);
        query.bind(3, max_quantity);
        query.bind(4, min_health);
        query.bind(5, max_health);
        query.bind(6, adapter_filter);
        query.bind(7, location_filter);
        query.bind(8, after_storage);
        query.bind(9, after_root);
        query.bind(10, after_item);
        query.bind(11, static_cast<std::int64_t>(limit + 1));
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
                {"location_type", query.text(9)}, {"updated_ms", query.integer(10)}, {"revision", query.integer(11)}, {"scope", "item_index"}
            });
        }
    } else if (index_ready) {
        const auto prefix = escape_like_prefix(query_text);
        Statement query(pool_.get(), R"SQL(
SELECT i.storage_id,i.root_item_id,i.item_id,COALESCE(i.parent_item_id,''),i.depth,i.class_name,i.quantity,i.health,
       i.adapter_id,i.location_type,i.updated_ms,c.revision
FROM cargo_item_index i JOIN storage_containers c ON c.storage_id=i.storage_id
WHERE lower(i.class_name) LIKE lower(?1)||'%' ESCAPE E'\\'
  AND i.quantity>=?2 AND i.quantity<=?3
  AND i.health>=?4 AND i.health<=?5
  AND (?6='' OR i.adapter_id=?6)
  AND (?7='' OR i.location_type=?7)
  AND (lower(i.class_name),i.storage_id,i.root_item_id,i.item_id)>(lower(?8),?9,?10,?11)
ORDER BY lower(i.class_name),i.storage_id,i.root_item_id,i.item_id
LIMIT ?12
)SQL");
        query.bind(1, prefix);
        query.bind(2, min_quantity);
        query.bind(3, max_quantity);
        query.bind(4, min_health);
        query.bind(5, max_health);
        query.bind(6, adapter_filter);
        query.bind(7, location_filter);
        query.bind(8, after_class);
        query.bind(9, after_storage);
        query.bind(10, after_root);
        query.bind(11, after_item);
        query.bind(12, static_cast<std::int64_t>(limit + 1));
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
                {"location_type", query.text(9)}, {"updated_ms", query.integer(10)}, {"revision", query.integer(11)}, {"scope", "item_index"}
            });
        }
    } else if (exact_item_id) {
        Statement query(pool_.get(), R"SQL(
SELECT r.storage_id,r.root_item_id,r.class_name,r.quantity,r.health,r.node_count,r.created_ms,r.tree_json::text,c.revision
FROM cargo_roots r JOIN storage_containers c ON c.storage_id=r.storage_id
WHERE r.item_ids ? ?1 AND (r.storage_id,r.root_item_id)>(?2,?3)
ORDER BY r.storage_id,r.root_item_id LIMIT ?4
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
            if (!adapter_filter.empty() && adapter != adapter_filter) continue;
            if (!location_filter.empty() && location != location_filter) continue;
            rows.push_back({
                {"storage_id", query.text(0)}, {"root_item_id", query.text(1)}, {"item_id", query_text},
                {"parent_item_id", parent}, {"depth", depth}, {"class_name", node->value("class_name", "")},
                {"quantity", quantity}, {"health", health}, {"adapter_id", adapter}, {"location_type", location},
                {"updated_ms", query.integer(6)}, {"revision", query.integer(8)}, {"scope", "exact_item_id_fallback"}
            });
        }
    } else {
        if (!adapter_filter.empty() || !location_filter.empty()) {
            return json{{"rows", json::array()}, {"nested_class_search_available", false},
                        {"item_index_complete", false}, {"filter_requires_index", true}};
        }
        Statement query(pool_.get(), R"SQL(
SELECT r.storage_id,r.root_item_id,r.class_name,r.quantity,r.health,r.node_count,r.created_ms,c.revision
FROM cargo_roots r JOIN storage_containers c ON c.storage_id=r.storage_id
WHERE lower(r.class_name) LIKE lower(?1)||'%' ESCAPE E'\\'
  AND r.quantity>=?2 AND r.quantity<=?3
  AND r.health>=?4 AND r.health<=?5
  AND (r.storage_id,r.root_item_id)>(?6,?7)
ORDER BY r.storage_id,r.root_item_id LIMIT ?8
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
                {"adapter_id", ""}, {"location_type", ""}, {"revision", query.integer(7)}, {"scope", "root_class_fallback"}
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

    const auto stale_cutoff = clippy::now_unix_ms() - 30LL * 60LL * 1000LL;
    Statement stale(pool_.get(), R"SQL(
SELECT session_id,storage_id,player_id,status,updated_ms,error
FROM cargo_sessions
WHERE status IN ('OPEN','MATERIALIZED','COMMITTED') AND updated_ms<?1
ORDER BY updated_ms LIMIT 50
)SQL");
    stale.bind(1, static_cast<std::int64_t>(stale_cutoff));
    result["stale_sessions"] = json::array();
    while (stale.row()) {
        json row={{"session_id",stale.text(0)},{"storage_id",stale.text(1)},{"player_id",stale.text(2)},
                  {"status",stale.text(3)},{"updated_ms",stale.integer(4)}};
        if(!stale.is_null(5)) row["error"]=stale.text(5);
        result["stale_sessions"].push_back(std::move(row));
    }
    result["stale_session_cutoff_ms"] = stale_cutoff;

    Statement failed_migrations(pool_.get(), R"SQL(
SELECT migration_id,storage_id,container_class,status,updated_ms,error
FROM cargo_migrations
WHERE status IN ('PREPARED','COMMITTED') AND error IS NOT NULL AND error<>''
ORDER BY updated_ms DESC LIMIT 50
)SQL");
    result["failed_migrations"] = json::array();
    while(failed_migrations.row()) {
        result["failed_migrations"].push_back({{"migration_id",failed_migrations.text(0)},{"storage_id",failed_migrations.text(1)},
                                                {"container_class",failed_migrations.text(2)},{"status",failed_migrations.text(3)},
                                                {"updated_ms",failed_migrations.integer(4)},{"error",failed_migrations.text(5)}});
    }

    Statement diagnostic_counts(pool_.get(), R"SQL(
SELECT
  (SELECT count(*) FROM cargo_roots r LEFT JOIN storage_containers c ON c.storage_id=r.storage_id WHERE c.storage_id IS NULL),
  (SELECT count(DISTINCT storage_id) FROM (
      SELECT storage_id FROM operations WHERE status IN ('PREPARED','QUARANTINED') OR cleanup_state='PENDING'
      UNION ALL SELECT storage_id FROM cargo_sessions WHERE status IN ('OPEN','MATERIALIZED','COMMITTED')
      UNION ALL SELECT storage_id FROM cargo_migrations WHERE status IN ('PREPARED','COMMITTED')
   ) blocked),
  (SELECT count(*) FROM admin_audit_events WHERE result='FAILURE' AND lower(error) LIKE '%revision%')
)SQL");
    diagnostic_counts.row();
    result["orphaned_virtual_roots"] = diagnostic_counts.integer(0);
    result["blocked_containers"] = diagnostic_counts.integer(1);
    result["revision_conflict_failures"] = diagnostic_counts.integer(2);

    Statement last_integrity(pool_.get(), R"SQL(
SELECT detail_json::text,created_ms FROM admin_audit_events
WHERE action='run_integrity_check' AND result='SUCCESS' ORDER BY created_ms DESC,event_id DESC LIMIT 1
)SQL");
    if(last_integrity.row()) {
        result["last_integrity"]={{"detail",json::parse(last_integrity.text(0))},{"created_ms",last_integrity.integer(1)}};
    }

    result["truncated"] = result["operations"].size() >= 100 || result["sessions"].size() >= 100 || result["migrations"].size() >= 100;
    return result;
}


json AdminDatabase::players(const std::string& search, std::int64_t before_ms,
                            const std::string& before_id, int limit, std::int64_t online_window_ms) {
    std::lock_guard lock(gate_);
    if (before_ms <= 0) before_ms = (std::numeric_limits<std::int64_t>::max)();
    const auto cursor_id = before_id.empty() ? std::string(256, '~') : before_id;
    const auto now = clippy::now_unix_ms();
    const auto prefix = escape_like_prefix(search);
    Statement query(pool_.get(), R"SQL(
SELECT p.player_id,p.display_name,p.first_seen_ms,p.last_seen_ms,p.last_snapshot_ms,p.last_inventory_count,
       (p.last_seen_ms>=?5) AS online,
       COALESCE((SELECT count(*) FROM player_aliases a WHERE a.player_id=p.player_id),0),
       p.last_ping_ms,p.last_bandwidth_kbps,p.last_output_throttle,p.last_map_name,
       p.last_position_x,p.last_position_y,p.last_position_z
FROM players p
WHERE (p.last_seen_ms,p.player_id)<(?2,?3)
  AND (?1='' OR lower(p.display_name) LIKE lower(?1)||'%' ESCAPE E'\\'
       OR p.player_id=?1
       OR EXISTS(SELECT 1 FROM player_aliases a WHERE a.player_id=p.player_id
                 AND lower(a.display_name) LIKE lower(?1)||'%' ESCAPE E'\\'))
ORDER BY p.last_seen_ms DESC,p.player_id DESC
LIMIT ?4
)SQL");
    query.bind(1, prefix);
    query.bind(2, before_ms);
    query.bind(3, cursor_id);
    query.bind(4, static_cast<std::int64_t>(limit + 1));
    query.bind(5, now - online_window_ms);
    json rows = json::array();
    std::int64_t next_ms = 0;
    std::string next_id;
    while (query.row()) {
        if (rows.size() == static_cast<std::size_t>(limit)) {
            next_ms = rows.back()["last_seen_ms"].get<std::int64_t>();
            next_id = rows.back()["player_id"].get<std::string>();
            break;
        }
        json row = {
            {"player_id",query.text(0)},{"display_name",query.text(1)},{"first_seen_ms",query.integer(2)},
            {"last_seen_ms",query.integer(3)},{"last_inventory_count",query.integer(5)},
            {"online",query.text(6)=="t"},{"alias_count",query.integer(7)}
        };
        if (!query.is_null(4)) row["last_snapshot_ms"] = query.integer(4);
        if (!query.is_null(8)) row["last_ping_ms"] = query.integer(8);
        if (!query.is_null(9)) row["last_bandwidth_kbps"] = query.integer(9);
        if (!query.is_null(10)) row["last_output_throttle"] = query.number(10);
        row["last_map_name"] = query.text(11);
        if (!query.is_null(12)) row["last_position_x"] = query.number(12);
        if (!query.is_null(13)) row["last_position_y"] = query.number(13);
        if (!query.is_null(14)) row["last_position_z"] = query.number(14);
        rows.push_back(std::move(row));
    }
    json result={{"rows",std::move(rows)},{"online_cutoff_ms",now-online_window_ms}};
    if (!next_id.empty()) { result["next_before_ms"]=next_ms; result["next_before_id"]=next_id; }
    return result;
}

json AdminDatabase::player_detail(const std::string& player_id, std::int64_t online_window_ms) {
    std::lock_guard lock(gate_);
    const auto now = clippy::now_unix_ms();
    Statement player(pool_.get(), R"SQL(
SELECT player_id,display_name,plain_name,full_name,last_session_player_id,
       first_seen_ms,last_seen_ms,last_snapshot_ms,last_inventory_count,last_seen_ms>=?2 AS online,
       last_ping_ms,last_bandwidth_kbps,last_output_throttle,last_map_name,
       last_position_x,last_position_y,last_position_z
FROM players WHERE player_id=?1
)SQL");
    player.bind(1,player_id); player.bind(2,now-online_window_ms);
    if (!player.row()) throw clippy::ApiError(404,"player_not_found","The player is not present in the telemetry registry.");
    json result={{"player_id",player.text(0)},{"display_name",player.text(1)},
                 {"plain_name",player.text(2)},{"full_name",player.text(3)},
                 {"first_seen_ms",player.integer(5)},{"last_seen_ms",player.integer(6)},
                 {"last_inventory_count",player.integer(8)},{"online",player.text(9)=="t"},
                 {"last_map_name",player.text(13)}};
    if(!player.is_null(4)) result["last_session_player_id"]=player.integer(4);
    if(!player.is_null(7)) result["last_snapshot_ms"]=player.integer(7);
    if(!player.is_null(10)) result["last_ping_ms"]=player.integer(10);
    if(!player.is_null(11)) result["last_bandwidth_kbps"]=player.integer(11);
    if(!player.is_null(12)) result["last_output_throttle"]=player.number(12);
    if(!player.is_null(14)) result["last_position_x"]=player.number(14);
    if(!player.is_null(15)) result["last_position_y"]=player.number(15);
    if(!player.is_null(16)) result["last_position_z"]=player.number(16);

    result["aliases"]=json::array();
    Statement aliases(pool_.get(),"SELECT display_name,first_seen_ms,last_seen_ms FROM player_aliases WHERE player_id=?1 ORDER BY last_seen_ms DESC LIMIT 50");
    aliases.bind(1,player_id);
    while(aliases.row()) result["aliases"].push_back({{"display_name",aliases.text(0)},{"first_seen_ms",aliases.integer(1)},{"last_seen_ms",aliases.integer(2)}});

    result["snapshots"]=json::array();
    Statement snapshots_query(pool_.get(),R"SQL(
SELECT snapshot_id,captured_ms,item_count,profile_json::text,network_json::text,position_json::text,equipment_json::text
FROM player_inventory_snapshots WHERE player_id=?1
ORDER BY captured_ms DESC,snapshot_id DESC LIMIT 25
)SQL");
    snapshots_query.bind(1,player_id);
    while(snapshots_query.row()) result["snapshots"].push_back({
        {"snapshot_id",snapshots_query.text(0)},{"captured_ms",snapshots_query.integer(1)},
        {"item_count",snapshots_query.integer(2)},{"profile",json::parse(snapshots_query.text(3))},
        {"network",json::parse(snapshots_query.text(4))},{"position",json::parse(snapshots_query.text(5))},
        {"equipment",json::parse(snapshots_query.text(6))}
    });

    result["recent_containers"]=json::array();
    Statement sessions_query(pool_.get(),R"SQL(
SELECT s.storage_id,c.display_name,c.container_class,c.map_name,s.status,s.updated_ms
FROM cargo_sessions s JOIN storage_containers c ON c.storage_id=s.storage_id
WHERE s.player_id=?1 ORDER BY s.updated_ms DESC,s.session_id DESC LIMIT 25
)SQL");
    sessions_query.bind(1,player_id);
    while(sessions_query.row()) result["recent_containers"].push_back({
        {"storage_id",sessions_query.text(0)},{"display_name",sessions_query.text(1)},
        {"container_class",sessions_query.text(2)},{"map_name",sessions_query.text(3)},
        {"status",sessions_query.text(4)},{"updated_ms",sessions_query.integer(5)}
    });

    result["recent_events"]=json::array();
    Statement events(pool_.get(),R"SQL(
SELECT event_id,event_type,detail_json::text,created_ms
FROM player_events WHERE player_id=?1 ORDER BY created_ms DESC,event_id DESC LIMIT 30
)SQL");
    events.bind(1,player_id);
    while(events.row()) result["recent_events"].push_back({
        {"event_id",events.integer(0)},{"event_type",events.text(1)},
        {"detail",json::parse(events.text(2))},{"created_ms",events.integer(3)}
    });
    return result;
}

json AdminDatabase::player_snapshot_tree(const std::string& player_id, const std::string& snapshot_id) {
    std::lock_guard lock(gate_);
    Statement query(pool_.get(),R"SQL(
SELECT snapshot_id,captured_ms,item_count,profile_json::text,network_json::text,position_json::text,equipment_json::text,inventory_json::text
FROM player_inventory_snapshots WHERE snapshot_id=?1 AND player_id=?2
)SQL");
    query.bind(1,snapshot_id); query.bind(2,player_id);
    if(!query.row()) throw clippy::ApiError(404,"snapshot_not_found","The requested player inventory snapshot was not found.");
    return {{"snapshot_id",query.text(0)},{"player_id",player_id},{"captured_ms",query.integer(1)},
            {"item_count",query.integer(2)},{"profile",json::parse(query.text(3))},
            {"network",json::parse(query.text(4))},{"position",json::parse(query.text(5))},
            {"equipment",json::parse(query.text(6))},{"inventory",json::parse(query.text(7))}};
}

json AdminDatabase::search_player_items(const std::string& search, const std::string& player_id,
                                        const std::string& after_class, const std::string& after_player,
                                        const std::string& after_item, int limit) {
    std::lock_guard lock(gate_);
    const auto prefix = escape_like_prefix(search);
    Statement query(pool_.get(),R"SQL(
WITH latest AS (
    SELECT DISTINCT ON (player_id) snapshot_id,player_id,captured_ms
    FROM player_inventory_snapshots
    ORDER BY player_id,captured_ms DESC,snapshot_id DESC
)
SELECT i.class_name,i.player_id,p.display_name,i.item_id,i.parent_item_id,i.depth,i.quantity,i.health,
       i.adapter_id,i.location_type,l.snapshot_id,l.captured_ms
FROM latest l
JOIN player_item_index i ON i.snapshot_id=l.snapshot_id AND i.player_id=l.player_id
JOIN players p ON p.player_id=i.player_id
WHERE (?1='' OR lower(i.class_name) LIKE lower(?1)||'%' ESCAPE E'\\' OR i.item_id=?1)
  AND (?2='' OR i.player_id=?2)
  AND (lower(i.class_name),i.player_id,i.item_id)>(lower(?3),?4,?5)
ORDER BY lower(i.class_name),i.player_id,i.item_id
LIMIT ?6
)SQL");
    query.bind(1,prefix); query.bind(2,player_id); query.bind(3,after_class);
    query.bind(4,after_player); query.bind(5,after_item); query.bind(6,static_cast<std::int64_t>(limit+1));
    json rows=json::array();
    std::string nc,np,ni;
    while(query.row()) {
        if(rows.size()==static_cast<std::size_t>(limit)) {
            nc=rows.back()["class_name"].get<std::string>();
            np=rows.back()["player_id"].get<std::string>();
            ni=rows.back()["item_id"].get<std::string>();
            break;
        }
        json row={{"class_name",query.text(0)},{"player_id",query.text(1)},{"display_name",query.text(2)},
                  {"item_id",query.text(3)},{"depth",query.integer(5)},{"quantity",query.number(6)},
                  {"health",query.number(7)},{"adapter_id",query.text(8)},{"location_type",query.text(9)},
                  {"snapshot_id",query.text(10)},{"captured_ms",query.integer(11)}};
        if(!query.is_null(4)) row["parent_item_id"]=query.text(4);
        rows.push_back(std::move(row));
    }
    json result={{"rows",std::move(rows)}};
    if(!ni.empty()){result["next_after_class"]=nc;result["next_after_player"]=np;result["next_after_item"]=ni;}
    return result;
}

json AdminDatabase::player_commands(const std::string& player_id, std::int64_t before_ms,
                                    const std::string& before_id, int limit) {
    std::lock_guard lock(gate_);
    if(before_ms<=0) before_ms=(std::numeric_limits<std::int64_t>::max)();
    const auto cursor=before_id.empty()?std::string(128,'~'):before_id;
    Statement query(pool_.get(),R"SQL(
SELECT command_id,player_id,action,status,reason,created_ms,expires_ms,claimed_ms,completed_ms,result_json::text,error
FROM admin_player_commands
WHERE (?1='' OR player_id=?1) AND (created_ms,command_id)<(?2,?3)
ORDER BY created_ms DESC,command_id DESC LIMIT ?4
)SQL");
    query.bind(1,player_id);query.bind(2,before_ms);query.bind(3,cursor);query.bind(4,static_cast<std::int64_t>(limit+1));
    json rows=json::array();std::int64_t next_ms=0;std::string next_id;
    while(query.row()){
        if(rows.size()==static_cast<std::size_t>(limit)){next_ms=rows.back()["created_ms"].get<std::int64_t>();next_id=rows.back()["command_id"].get<std::string>();break;}
        json row={{"command_id",query.text(0)},{"player_id",query.text(1)},{"action",query.text(2)},{"status",query.text(3)},
                  {"reason",query.text(4)},{"created_ms",query.integer(5)},{"expires_ms",query.integer(6)}};
        if(!query.is_null(7))row["claimed_ms"]=query.integer(7);
        if(!query.is_null(8))row["completed_ms"]=query.integer(8);
        if(!query.is_null(9))row["result"]=json::parse(query.text(9));
        if(!query.is_null(10))row["error"]=query.text(10);
        rows.push_back(std::move(row));
    }
    json result={{"rows",std::move(rows)}};if(!next_id.empty()){result["next_before_ms"]=next_ms;result["next_before_id"]=next_id;}return result;
}

json AdminDatabase::player_quarantine(const std::string& player_id, std::int64_t before_ms,
                                      const std::string& before_id, int limit) {
    std::lock_guard lock(gate_);
    if(before_ms<=0) before_ms=(std::numeric_limits<std::int64_t>::max)();
    const auto cursor=before_id.empty()?std::string(128,'~'):before_id;
    Statement query(pool_.get(),R"SQL(
SELECT q.quarantine_id,q.player_id,p.display_name,q.item_id,q.tree_json->>'class_name',
       q.created_ms,q.restored_command_id,q.restored_ms
FROM player_quarantine q JOIN players p ON p.player_id=q.player_id
WHERE (?1='' OR q.player_id=?1) AND (q.created_ms,q.quarantine_id)<(?2,?3)
ORDER BY q.created_ms DESC,q.quarantine_id DESC LIMIT ?4
)SQL");
    query.bind(1,player_id);query.bind(2,before_ms);query.bind(3,cursor);query.bind(4,static_cast<std::int64_t>(limit+1));
    json rows=json::array();std::int64_t next_ms=0;std::string next_id;
    while(query.row()){
        if(rows.size()==static_cast<std::size_t>(limit)){next_ms=rows.back()["created_ms"].get<std::int64_t>();next_id=rows.back()["quarantine_id"].get<std::string>();break;}
        json row={{"quarantine_id",query.text(0)},{"player_id",query.text(1)},{"display_name",query.text(2)},
                  {"item_id",query.text(3)},{"class_name",query.text(4)},{"created_ms",query.integer(5)},
                  {"restored",!query.is_null(7)}};
        if(!query.is_null(6))row["restored_command_id"]=query.text(6);
        if(!query.is_null(7))row["restored_ms"]=query.integer(7);
        rows.push_back(std::move(row));
    }
    json result={{"rows",std::move(rows)}};if(!next_id.empty()){result["next_before_ms"]=next_ms;result["next_before_id"]=next_id;}return result;
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

    Statement schemas(pool_.get(), R"SQL(
SELECT nspname FROM pg_namespace WHERE nspname='clippy' ORDER BY nspname
)SQL");
    result["schemas"] = json::array();
    while (schemas.row()) result["schemas"].push_back(schemas.text(0));

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


json AdminDatabase::report(const std::string& kind, int limit) {
    std::lock_guard lock(gate_);
    limit = std::clamp(limit, 1, 50);
    json result={{"kind",kind},{"rows",json::array()}};

    if (kind == "top_classes") {
        if (!item_index_complete(pool_.get())) throw clippy::ApiError(409,"item_index_incomplete","The virtual cargo item index must finish before this report can run.");
        Statement q(pool_.get(), R"SQL(
SELECT class_name,count(*) AS instances,count(DISTINCT storage_id) AS containers,
       sum(CASE WHEN health<=0 THEN 1 ELSE 0 END) AS ruined
FROM cargo_item_index
GROUP BY class_name ORDER BY instances DESC,class_name LIMIT ?1
)SQL");
        q.bind(1,static_cast<std::int64_t>(limit));
        while(q.row()) result["rows"].push_back({{"class_name",q.text(0)},{"instances",q.integer(1)},{"containers",q.integer(2)},{"ruined",q.integer(3)}});
    } else if (kind == "container_types") {
        Statement q(pool_.get(), R"SQL(
SELECT COALESCE(NULLIF(container_class,''),'Unknown') AS container_class,count(*) AS containers,
       sum(CASE WHEN last_seen_ms IS NULL THEN 1 ELSE 0 END) AS never_observed
FROM storage_containers GROUP BY COALESCE(NULLIF(container_class,''),'Unknown')
ORDER BY containers DESC,container_class LIMIT ?1
)SQL");
        q.bind(1,static_cast<std::int64_t>(limit));
        while(q.row()) result["rows"].push_back({{"container_class",q.text(0)},{"containers",q.integer(1)},{"never_observed",q.integer(2)}});
    } else if (kind == "largest_containers") {
        Statement q(pool_.get(), R"SQL(
SELECT c.storage_id,c.display_name,c.container_class,c.map_name,count(r.root_item_id) AS roots,
       COALESCE(sum(r.node_count),0) AS nodes,c.last_seen_ms
FROM storage_containers c LEFT JOIN cargo_roots r ON r.storage_id=c.storage_id
GROUP BY c.storage_id,c.display_name,c.container_class,c.map_name,c.last_seen_ms
ORDER BY nodes DESC,c.storage_id LIMIT ?1
)SQL");
        q.bind(1,static_cast<std::int64_t>(limit));
        while(q.row()) {
            json row={{"storage_id",q.text(0)},{"display_name",q.text(1)},{"container_class",q.text(2)},{"map_name",q.text(3)},{"roots",q.integer(4)},{"nodes",q.integer(5)}};
            if(!q.is_null(6)) row["last_seen_ms"]=q.integer(6);
            result["rows"].push_back(std::move(row));
        }
    } else if (kind == "stale_containers") {
        const auto now=clippy::now_unix_ms();
        Statement q(pool_.get(), R"SQL(
SELECT
 sum(CASE WHEN last_seen_ms IS NULL OR last_seen_ms<?1 THEN 1 ELSE 0 END),
 sum(CASE WHEN last_seen_ms IS NULL OR last_seen_ms<?2 THEN 1 ELSE 0 END),
 sum(CASE WHEN last_seen_ms IS NULL OR last_seen_ms<?3 THEN 1 ELSE 0 END),
 count(*)
FROM storage_containers
)SQL");
        q.bind(1,static_cast<std::int64_t>(now-7LL*24*60*60*1000));
        q.bind(2,static_cast<std::int64_t>(now-30LL*24*60*60*1000));
        q.bind(3,static_cast<std::int64_t>(now-90LL*24*60*60*1000));
        if(q.row()) result["rows"].push_back({{"stale_7_days",q.integer(0)},{"stale_30_days",q.integer(1)},{"stale_90_days",q.integer(2)},{"total",q.integer(3)}});
    } else if (kind == "player_classes") {
        Statement q(pool_.get(), R"SQL(
SELECT i.class_name,count(*) AS instances,count(DISTINCT i.player_id) AS players
FROM player_item_index i GROUP BY i.class_name
ORDER BY instances DESC,i.class_name LIMIT ?1
)SQL");
        q.bind(1,static_cast<std::int64_t>(limit));
        while(q.row()) result["rows"].push_back({{"class_name",q.text(0)},{"instances",q.integer(1)},{"players",q.integer(2)}});
    } else if (kind == "duplicate_item_ids") {
        if (!item_index_complete(pool_.get())) throw clippy::ApiError(409,"item_index_incomplete","The virtual cargo item index must finish before this report can run.");
        Statement q(pool_.get(), R"SQL(
SELECT item_id,count(*) AS instances,count(DISTINCT storage_id) AS containers
FROM cargo_item_index GROUP BY item_id HAVING count(*)>1
ORDER BY instances DESC,item_id LIMIT ?1
)SQL");
        q.bind(1,static_cast<std::int64_t>(limit));
        while(q.row()) result["rows"].push_back({{"item_id",q.text(0)},{"instances",q.integer(1)},{"containers",q.integer(2)}});
    } else {
        throw clippy::ApiError(400,"invalid_report","Unknown report kind.");
    }
    return result;
}


json AdminDatabase::table_preview(const std::string& table, const std::string& after_ctid, int limit) {
    static const std::set<std::string> allowed = {
        "admin_audit_events","admin_change_entries","admin_change_sets","admin_container_locks","admin_quarantine",
        "admin_snapshot_roots","admin_storage_snapshots","application_meta","audit_events","cargo_item_index",
        "cargo_item_index_state","cargo_migration_observations","cargo_migration_roots","cargo_migrations","cargo_roots",
        "cargo_session_cleanup_roots","cargo_sessions","legacy_imports","operation_cleanup_roots","operations",
        "schema_migrations","storage_containers","players","player_aliases","player_inventory_snapshots","player_item_index","player_events","admin_player_commands","player_quarantine"
    };
    if (!allowed.count(table)) throw clippy::ApiError(404, "table_not_available", "That table is not available through the safe database browser.");
    if (!after_ctid.empty()) {
        if (after_ctid.size() > 32 || after_ctid.front() != '(' || after_ctid.back() != ')' ||
            after_ctid.find(',') == std::string::npos ||
            std::any_of(after_ctid.begin() + 1, after_ctid.end() - 1, [](unsigned char c) { return !(std::isdigit(c) || c == ','); })) {
            throw clippy::ApiError(400, "invalid_cursor", "The database table cursor is invalid.");
        }
    }
    std::lock_guard lock(gate_);
    const std::string sql = "SELECT ctid::text,(to_jsonb(t)-ARRAY['tree_json','inventory_json','before_state','after_state','payload_json','result_json','equipment_json','state_json','item_ids','original_root_ids_json','physical_source_keys_json','detail_json','search_state_json'])::text FROM clippy." + table +
                            " t WHERE (?1='' OR ctid>NULLIF(?1,'')::tid) ORDER BY ctid LIMIT ?2";
    Statement query(pool_.get(), sql);
    query.bind(1, after_ctid);
    query.bind(2, static_cast<std::int64_t>(limit + 1));
    json rows = json::array();
    std::string next;
    while (query.row()) {
        if (rows.size() == static_cast<std::size_t>(limit)) {
            next = rows.back()["ctid"].get<std::string>();
            break;
        }
        rows.push_back({{"ctid",query.text(0)},{"row",json::parse(query.text(1))}});
    }
    json result={{"table",table},{"rows",std::move(rows)},{"omitted_large_fields",json::array({"tree_json","inventory_json","before_state","after_state","payload_json","result_json","equipment_json","state_json","item_ids","original_root_ids_json","physical_source_keys_json","detail_json","search_state_json"})}};
    if (!next.empty()) result["next_after"] = next;
    return result;
}

} // namespace clippy_admin
