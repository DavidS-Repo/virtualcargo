#include "admin_database.hpp"

#include "util.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace clippy_admin {

using nlohmann::json;
using clippy::Statement;

namespace {

constexpr std::size_t max_admin_item_nodes = 4096;
constexpr int max_admin_item_depth = 16;
constexpr std::size_t max_reason_length = 512;

std::string bounded_reason(std::string value) {
    if (value.size() > max_reason_length || value.find('\0') != std::string::npos) {
        throw clippy::ApiError(400, "invalid_reason", "The admin reason is too long or contains an invalid character.");
    }
    return value;
}

void require_editing(bool enabled, clippy::PgPool* writer) {
    if (!enabled || !writer) {
        throw clippy::ApiError(403, "editing_disabled", "Admin editing is disabled in ClippyServerManager.json.");
    }
}

std::int64_t lock_storage_row(clippy::PgPool* database, const std::string& storage_id) {
    Statement query(database, "SELECT revision FROM storage_containers WHERE storage_id=?1 FOR UPDATE");
    query.bind(1, storage_id);
    if (!query.row()) throw clippy::ApiError(404, "storage_not_found", "The storage container does not exist.");
    return query.integer(0);
}

void assert_revision(std::int64_t current, std::int64_t expected) {
    if (current != expected) {
        throw clippy::ApiError(409, "revision_conflict",
            "This container changed after you opened it. Reload the latest inventory before saving.", true);
    }
}

void delete_expired_locks(clippy::PgPool* database, const std::string& storage_id, std::int64_t now) {
    Statement cleanup(database, "DELETE FROM admin_container_locks WHERE storage_id=?1 AND expires_ms<=?2");
    cleanup.bind(1, storage_id);
    cleanup.bind(2, now);
    cleanup.done();
}

void assert_workflow_free(clippy::PgPool* database, const std::string& storage_id) {
    Statement active(database, R"SQL(
SELECT workflow_kind,workflow_id,status FROM (
  SELECT 'direct operation' AS workflow_kind,operation_id AS workflow_id,status
  FROM operations WHERE storage_id=?1 AND (status IN ('PREPARED','QUARANTINED') OR cleanup_state='PENDING')
  UNION ALL
  SELECT 'page session',session_id,status FROM cargo_sessions
  WHERE storage_id=?1 AND status IN ('OPEN','MATERIALIZED','COMMITTED')
  UNION ALL
  SELECT 'cargo migration',migration_id,status FROM cargo_migrations
  WHERE storage_id=?1 AND status IN ('PREPARED','COMMITTED')
) busy ORDER BY workflow_kind,workflow_id LIMIT 1
)SQL");
    active.bind(1, storage_id);
    if (active.row()) {
        throw clippy::ApiError(409, "storage_busy",
            "This container has an active " + active.text(0) + " (" + active.text(2) + "). Finish or recover it before editing.", true);
    }
}

struct HeldLock {
    std::string storage_id;
    std::string lock_id;
    std::int64_t expected_revision = 0;
    bool preserve_on_release = false;
};

std::vector<HeldLock> acquire_locks(clippy::PgPool* database,
                                    std::vector<std::pair<std::string,std::int64_t>> requested,
                                    const std::string& admin_session_id,
                                    const std::string& reason,
                                    int duration_seconds) {
    std::sort(requested.begin(), requested.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    requested.erase(std::unique(requested.begin(), requested.end(), [](const auto& a, const auto& b) { return a.first == b.first; }), requested.end());
    const auto now = clippy::now_unix_ms();
    const auto expires = now + static_cast<std::int64_t>(duration_seconds) * 1000;
    std::vector<HeldLock> locks;
    clippy::Transaction transaction(database);
    for (const auto& [storage_id, expected_revision] : requested) {
        const auto current = lock_storage_row(database, storage_id);
        assert_revision(current, expected_revision);
        delete_expired_locks(database, storage_id, now);
        assert_workflow_free(database, storage_id);
        Statement existing(database, "SELECT lock_id,admin_session_id FROM admin_container_locks WHERE storage_id=?1");
        existing.bind(1, storage_id);
        if (existing.row()) {
            if (existing.text(1) != admin_session_id) {
                throw clippy::ApiError(409, "admin_lock_conflict", "Another local admin session is already maintaining this container.", true);
            }
            HeldLock held{storage_id, existing.text(0), expected_revision, true};
            Statement renew(database, "UPDATE admin_container_locks SET expires_ms=?2 WHERE storage_id=?1 AND admin_session_id=?3");
            renew.bind(1, storage_id); renew.bind(2, expires); renew.bind(3, admin_session_id); renew.done();
            locks.push_back(std::move(held));
            continue;
        }
        HeldLock held{storage_id, clippy::random_hex(16), expected_revision, false};
        Statement insert(database, R"SQL(
INSERT INTO admin_container_locks(storage_id,lock_id,admin_session_id,lock_reason,created_ms,expires_ms)
VALUES(?1,?2,?3,?4,?5,?6)
)SQL");
        insert.bind(1, storage_id);
        insert.bind(2, held.lock_id);
        insert.bind(3, admin_session_id);
        insert.bind(4, reason.empty() ? std::string("Clippy Admin change") : reason);
        insert.bind(5, now);
        insert.bind(6, expires);
        insert.done();
        locks.push_back(std::move(held));
    }
    transaction.commit();
    return locks;
}

void assert_locks_owned(clippy::PgPool* database, const std::vector<HeldLock>& locks,
                        const std::string& admin_session_id) {
    const auto now = clippy::now_unix_ms();
    for (const auto& held : locks) {
        Statement query(database, R"SQL(
SELECT 1 FROM admin_container_locks
WHERE storage_id=?1 AND lock_id=?2 AND admin_session_id=?3 AND expires_ms>?4
FOR UPDATE
)SQL");
        query.bind(1, held.storage_id);
        query.bind(2, held.lock_id);
        query.bind(3, admin_session_id);
        query.bind(4, now);
        if (!query.row()) {
            throw clippy::ApiError(409, "admin_lock_expired", "The maintenance lock expired before the change could be saved. Reload and try again.", true);
        }
    }
}

void release_locks(clippy::PgPool* database, const std::vector<HeldLock>& locks,
                   const std::string& admin_session_id) {
    clippy::Transaction transaction(database);
    Statement remove(database, "DELETE FROM admin_container_locks WHERE storage_id=?1 AND lock_id=?2 AND admin_session_id=?3");
    for (const auto& held : locks) {
        if (held.preserve_on_release) continue;
        remove.bind(1, held.storage_id);
        remove.bind(2, held.lock_id);
        remove.bind(3, admin_session_id);
        remove.done();
        remove.reset();
    }
    transaction.commit();
}

void release_locks_in_transaction(clippy::PgPool* database, const std::vector<HeldLock>& locks,
                                  const std::string& admin_session_id) {
    Statement remove(database, "DELETE FROM admin_container_locks WHERE storage_id=?1 AND lock_id=?2 AND admin_session_id=?3");
    for (const auto& held : locks) {
        if (held.preserve_on_release) continue;
        remove.bind(1, held.storage_id);
        remove.bind(2, held.lock_id);
        remove.bind(3, admin_session_id);
        remove.done();
        remove.reset();
    }
}

bool find_path(const json& node, const std::string& item_id, std::vector<std::size_t>& path) {
    if (!node.is_object()) return false;
    if (node.value("item_id", "") == item_id) return true;
    if (!node.contains("children") || !node["children"].is_array()) return false;
    const auto& children = node["children"];
    for (std::size_t index = 0; index < children.size(); ++index) {
        path.push_back(index);
        if (find_path(children[index], item_id, path)) return true;
        path.pop_back();
    }
    return false;
}

json* node_at(json& root, const std::vector<std::size_t>& path) {
    json* node = &root;
    for (const auto index : path) {
        if (!node->contains("children") || !(*node)["children"].is_array() || index >= (*node)["children"].size()) return nullptr;
        node = &(*node)["children"][index];
    }
    return node;
}


json detach_node(json& root, const std::vector<std::size_t>& path, std::string& parent_item_id, int& parent_index) {
    if (path.empty()) return root;
    std::vector<std::size_t> parent_path(path.begin(), path.end() - 1);
    auto* parent = node_at(root, parent_path);
    if (!parent || !parent->contains("children") || !(*parent)["children"].is_array()) {
        throw clippy::ApiError(409, "item_tree_changed", "The stored item tree changed while the admin action was being prepared.", true);
    }
    parent_item_id = parent->value("item_id", "");
    parent_index = static_cast<int>(path.back());
    auto& children = (*parent)["children"];
    if (path.back() >= children.size()) throw clippy::ApiError(409, "item_tree_changed", "The stored item tree changed.", true);
    json detached = children[path.back()];
    children.erase(children.begin() + static_cast<json::difference_type>(path.back()));
    return detached;
}

void regenerate_item_ids(json& node) {
    if (!node.is_object()) return;
    node["item_id"] = clippy::random_hex(16);
    if (node.contains("children") && node["children"].is_array()) {
        for (auto& child : node["children"]) regenerate_item_ids(child);
    }
}

struct TreeStats {
    json ids = json::array();
    std::set<std::string> seen_ids;
    std::size_t nodes = 0;
};

void validate_tree_node(const json& node, int depth, TreeStats& stats) {
    if (!node.is_object()) throw clippy::ApiError(400, "invalid_item", "Stored item tree contains an invalid node.");
    if (depth > max_admin_item_depth) throw clippy::ApiError(400, "item_too_deep", "The edited item tree would exceed the supported nesting depth.");
    if (++stats.nodes > max_admin_item_nodes) throw clippy::ApiError(400, "too_many_items", "The edited item tree would contain too many nodes.");
    if (!node.contains("item_id") || !node["item_id"].is_string() || node["item_id"].get<std::string>().empty() || node["item_id"].get<std::string>().size() > 128) {
        throw clippy::ApiError(400, "invalid_item", "Every item needs a valid item_id.");
    }
    if (!node.contains("class_name") || !node["class_name"].is_string() || node["class_name"].get<std::string>().empty() || node["class_name"].get<std::string>().size() > 256) {
        throw clippy::ApiError(400, "invalid_item", "Every item needs a valid class_name.");
    }
    const double quantity = node.value("quantity", 0.0);
    const double health = node.value("health", 0.0);
    if (!std::isfinite(quantity) || quantity < 0.0 || !std::isfinite(health) || health < 0.0) {
        throw clippy::ApiError(400, "invalid_item", "Quantity and health must be finite, non-negative numbers.");
    }
    if (!node.contains("state") || !node["state"].is_object() || !node.contains("location") || !node["location"].is_object() ||
        !node.contains("adapter") || !node["adapter"].is_object()) {
        throw clippy::ApiError(400, "invalid_item", "The stored item adapter, state, or location is invalid.");
    }
    const auto item_id = node["item_id"].get<std::string>();
    if (!stats.seen_ids.insert(item_id).second) {
        throw clippy::ApiError(409, "duplicate_item_id", "The stored item tree contains a duplicate item ID. The admin change was refused.");
    }
    stats.ids.push_back(item_id);
    if (!node.contains("children")) return;
    if (!node["children"].is_array()) throw clippy::ApiError(400, "invalid_item", "Item children must be an array.");
    for (const auto& child : node["children"]) validate_tree_node(child, depth + 1, stats);
}

TreeStats validate_tree(const json& tree) {
    TreeStats stats;
    validate_tree_node(tree, 0, stats);
    return stats;
}

json load_root_for_update(clippy::PgPool* database, const std::string& storage_id, const std::string& root_item_id) {
    Statement query(database, "SELECT tree_json::text FROM cargo_roots WHERE storage_id=?1 AND root_item_id=?2 FOR UPDATE");
    query.bind(1, storage_id);
    query.bind(2, root_item_id);
    if (!query.row()) throw clippy::ApiError(404, "item_not_found", "The virtual root item no longer exists.");
    return json::parse(query.text(0));
}

std::optional<json> load_optional_root_for_update(clippy::PgPool* database, const std::string& storage_id, const std::string& root_item_id) {
    Statement query(database, "SELECT tree_json::text FROM cargo_roots WHERE storage_id=?1 AND root_item_id=?2 FOR UPDATE");
    query.bind(1, storage_id);
    query.bind(2, root_item_id);
    if (!query.row()) return std::nullopt;
    return json::parse(query.text(0));
}

void store_root(clippy::PgPool* database, const std::string& storage_id, const json& tree, std::int64_t now) {
    const auto root_id = tree.value("item_id", "");
    if (root_id.empty()) throw clippy::ApiError(400, "invalid_item", "The root item has no item_id.");
    const auto stats = validate_tree(tree);
    Statement upsert(database, R"SQL(
INSERT INTO cargo_roots(storage_id,root_item_id,class_name,quantity,health,state_json,tree_json,item_ids,node_count,created_ms)
VALUES(?1,?2,?3,?4,?5,?6::jsonb,?7::jsonb,?8::jsonb,?9,?10)
ON CONFLICT(storage_id,root_item_id) DO UPDATE SET
  class_name=excluded.class_name,
  quantity=excluded.quantity,
  health=excluded.health,
  state_json=excluded.state_json,
  tree_json=excluded.tree_json,
  item_ids=excluded.item_ids,
  node_count=excluded.node_count
)SQL");
    upsert.bind(1, storage_id);
    upsert.bind(2, root_id);
    upsert.bind(3, tree.value("class_name", ""));
    upsert.bind(4, tree.value("quantity", 0.0));
    upsert.bind(5, tree.value("health", 0.0));
    upsert.bind(6, tree.value("state", json::object()).dump());
    upsert.bind(7, tree.dump());
    upsert.bind(8, stats.ids.dump());
    upsert.bind(9, static_cast<std::int64_t>(stats.nodes));
    upsert.bind(10, now);
    upsert.done();
}

void delete_root(clippy::PgPool* database, const std::string& storage_id, const std::string& root_item_id) {
    Statement remove(database, "DELETE FROM cargo_roots WHERE storage_id=?1 AND root_item_id=?2");
    remove.bind(1, storage_id);
    remove.bind(2, root_item_id);
    remove.done();
    if (remove.affected_rows() != 1) throw clippy::ApiError(409, "item_tree_changed", "The virtual root changed before the admin change could be saved.", true);
}

void ensure_root_capacity(clippy::PgPool* database, const std::string& storage_id, std::int64_t additional_roots) {
    if (additional_roots <= 0) return;
    Statement query(database, R"SQL(
SELECT capacity_slots,(SELECT count(*) FROM cargo_roots r WHERE r.storage_id=c.storage_id)
FROM storage_containers c WHERE c.storage_id=?1
)SQL");
    query.bind(1, storage_id);
    if (!query.row()) throw clippy::ApiError(404, "storage_not_found", "The target storage container does not exist.");
    if (query.integer(1) + additional_roots > query.integer(0)) {
        throw clippy::ApiError(409, "storage_capacity", "The target virtual container does not have room for another root item.");
    }
}

void ensure_item_ids_available(clippy::PgPool* database, const std::string& storage_id,
                               const TreeStats& stats, const std::string& excluded_root_item_id = "") {
    Statement query(database, R"SQL(
SELECT root_item_id FROM cargo_roots
WHERE storage_id=?1
  AND (?3='' OR root_item_id<>?3)
  AND item_ids ?| ARRAY(SELECT jsonb_array_elements_text(?2::jsonb))
LIMIT 1
)SQL");
    query.bind(1, storage_id);
    query.bind(2, stats.ids.dump());
    query.bind(3, excluded_root_item_id);
    if (query.row()) {
        throw clippy::ApiError(409, "target_item_id_conflict",
            "The target container already contains one of the selected item IDs. The move or copy was refused.");
    }
}

std::int64_t increment_revision(clippy::PgPool* database, const std::string& storage_id, std::int64_t now) {
    Statement update(database, "UPDATE storage_containers SET revision=revision+1,updated_ms=?2 WHERE storage_id=?1 RETURNING revision");
    update.bind(1, storage_id);
    update.bind(2, now);
    if (!update.row()) throw clippy::ApiError(404, "storage_not_found", "The storage container does not exist.");
    return update.integer(0);
}

void bind_json_or_null(Statement& statement, int index, const std::optional<json>& value) {
    if (value) statement.bind(index, value->dump());
    else statement.bind_null(index);
}

void insert_change_set(clippy::PgPool* database,
                       const std::string& change_id,
                       const std::string& admin_session_id,
                       const std::string& windows_identity,
                       const std::string& action_type,
                       const std::string& storage_id,
                       const std::optional<std::string>& target_storage_id,
                       const std::optional<std::string>& item_id,
                       std::int64_t before_revision,
                       std::int64_t after_revision,
                       const std::optional<std::int64_t>& target_before_revision,
                       const std::optional<std::int64_t>& target_after_revision,
                       const std::string& reason,
                       const std::string& request_id,
                       std::int64_t now) {
    Statement insert(database, R"SQL(
INSERT INTO admin_change_sets(change_id,admin_session_id,windows_identity,action_type,storage_id,target_storage_id,item_id,
  before_revision,after_revision,target_before_revision,target_after_revision,reason,request_id,status,created_ms)
VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,'APPLIED',?14)
)SQL");
    insert.bind(1, change_id);
    insert.bind(2, admin_session_id);
    insert.bind(3, windows_identity);
    insert.bind(4, action_type);
    insert.bind(5, storage_id);
    if (target_storage_id) insert.bind(6, *target_storage_id); else insert.bind_null(6);
    if (item_id) insert.bind(7, *item_id); else insert.bind_null(7);
    insert.bind(8, before_revision);
    insert.bind(9, after_revision);
    if (target_before_revision) insert.bind(10, *target_before_revision); else insert.bind_null(10);
    if (target_after_revision) insert.bind(11, *target_after_revision); else insert.bind_null(11);
    insert.bind(12, reason);
    insert.bind(13, request_id);
    insert.bind(14, now);
    insert.done();
}

void insert_change_entry(clippy::PgPool* database, const std::string& change_id,
                         const std::string& storage_id, const std::string& root_item_id,
                         const std::string& item_id, const std::optional<json>& before_state,
                         const std::optional<json>& after_state) {
    Statement insert(database, R"SQL(
INSERT INTO admin_change_entries(change_id,storage_id,root_item_id,item_id,entry_kind,before_state,after_state)
VALUES(?1,?2,?3,?4,'ITEM',?5::jsonb,?6::jsonb)
)SQL");
    insert.bind(1, change_id);
    insert.bind(2, storage_id);
    insert.bind(3, root_item_id);
    insert.bind(4, item_id);
    bind_json_or_null(insert, 5, before_state);
    bind_json_or_null(insert, 6, after_state);
    insert.done();
}

void insert_audit(clippy::PgPool* database,
                  const std::string& admin_session_id,
                  const std::string& windows_identity,
                  const std::string& action,
                  const std::string& target_type,
                  const std::string& target_id,
                  const std::string& result,
                  const std::string& reason,
                  const std::string& error,
                  const std::string& request_id,
                  const std::optional<std::string>& change_id,
                  const json& detail,
                  std::int64_t now) {
    Statement insert(database, R"SQL(
INSERT INTO admin_audit_events(admin_session_id,windows_identity,action,target_type,target_id,result,reason,error,request_id,change_id,detail_json,created_ms)
VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11::jsonb,?12)
)SQL");
    insert.bind(1, admin_session_id);
    insert.bind(2, windows_identity);
    insert.bind(3, action);
    insert.bind(4, target_type);
    insert.bind(5, target_id);
    insert.bind(6, result);
    insert.bind(7, reason);
    if (error.empty()) insert.bind_null(8); else insert.bind(8, error);
    insert.bind(9, request_id);
    if (change_id) insert.bind(10, *change_id); else insert.bind_null(10);
    insert.bind(11, detail.dump());
    insert.bind(12, now);
    insert.done();
}

json change_response(const std::string& change_id, const std::string& storage_id,
                     std::int64_t revision, const std::optional<std::string>& target_storage_id = std::nullopt,
                     const std::optional<std::int64_t>& target_revision = std::nullopt) {
    json result = {{"change_id", change_id}, {"storage_id", storage_id}, {"revision", revision}};
    if (target_storage_id) result["target_storage_id"] = *target_storage_id;
    if (target_revision) result["target_revision"] = *target_revision;
    return result;
}


struct BulkRootSelection {
    std::string storage_id;
    std::string root_item_id;
    std::string item_id;
    std::int64_t expected_revision = 0;
};

bool valid_admin_identifier(const std::string& value) {
    if (value.empty() || value.size() > 128) return false;
    for (const unsigned char c : value) {
        if (!(std::isalnum(c) || c == '.' || c == '_' || c == ':' || c == '-')) return false;
    }
    return true;
}

std::vector<BulkRootSelection> parse_bulk_roots(const json& items) {
    if (!items.is_array() || items.empty() || items.size() > 25) {
        throw clippy::ApiError(400, "invalid_bulk_selection", "Bulk operations require between 1 and 25 root items per batch.");
    }
    std::vector<BulkRootSelection> parsed;
    std::set<std::pair<std::string,std::string>> unique;
    for (const auto& item : items) {
        if (!item.is_object() || !item.contains("storage_id") || !item["storage_id"].is_string() ||
            !item.contains("root_item_id") || !item["root_item_id"].is_string() ||
            !item.contains("item_id") || !item["item_id"].is_string() ||
            !item.contains("expected_revision") || !item["expected_revision"].is_number_integer()) {
            throw clippy::ApiError(400, "invalid_bulk_selection", "Every bulk item must include storage_id, root_item_id, item_id, and expected_revision.");
        }
        BulkRootSelection selection{item["storage_id"].get<std::string>(), item["root_item_id"].get<std::string>(),
                                    item["item_id"].get<std::string>(), item["expected_revision"].get<std::int64_t>()};
        if (!valid_admin_identifier(selection.storage_id) || !valid_admin_identifier(selection.root_item_id) ||
            !valid_admin_identifier(selection.item_id) || selection.expected_revision < 0) {
            throw clippy::ApiError(400, "invalid_bulk_selection", "A bulk item contains an invalid identifier or revision.");
        }
        if (selection.item_id != selection.root_item_id) {
            throw clippy::ApiError(400, "bulk_roots_only", "Bulk removal and quarantine operate on virtual root items only.");
        }
        if (!unique.emplace(selection.storage_id, selection.root_item_id).second) {
            throw clippy::ApiError(400, "duplicate_bulk_item", "The same virtual root appears more than once in the bulk selection.");
        }
        parsed.push_back(std::move(selection));
    }
    return parsed;
}

} // namespace

json AdminDatabase::edit_item(const std::string& storage_id, const std::string& root_item_id,
                              const std::string& item_id, std::int64_t expected_revision,
                              const json& patch, const std::string& raw_reason,
                              const std::string& admin_session_id, const std::string& windows_identity,
                              const std::string& request_id) {
    require_editing(editing_enabled_, writer_pool_.get());
    if (!patch.is_object()) throw clippy::ApiError(400, "invalid_request", "The item edit must be a JSON object.");
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        if (it.key() != "quantity" && it.key() != "health") {
            throw clippy::ApiError(400, "unsupported_edit", "Only quantity and health are supported by the generic item editor. Adapter state is not changed without adapter-specific validation.");
        }
    }
    if (patch.empty()) throw clippy::ApiError(400, "invalid_request", "No supported item changes were supplied.");
    const auto reason = bounded_reason(raw_reason);
    std::lock_guard lock(*writer_gate_);
    auto* db = writer_pool_.get();
    auto locks = acquire_locks(db, {{storage_id, expected_revision}}, admin_session_id, reason, maintenance_lock_seconds_);
    try {
        clippy::Transaction transaction(db);
        const auto current = lock_storage_row(db, storage_id);
        assert_revision(current, expected_revision);
        assert_locks_owned(db, locks, admin_session_id);
        assert_workflow_free(db, storage_id);

        auto before = load_root_for_update(db, storage_id, root_item_id);
        auto after = before;
        std::vector<std::size_t> path;
        if (!find_path(after, item_id, path)) throw clippy::ApiError(404, "item_not_found", "The selected item no longer exists in this root tree.");
        auto* node = node_at(after, path);
        if (!node) throw clippy::ApiError(409, "item_tree_changed", "The item tree changed while the edit was being prepared.", true);
        if (patch.contains("quantity")) {
            if (!patch["quantity"].is_number()) throw clippy::ApiError(400, "invalid_quantity", "Quantity must be a number.");
            const double value = patch["quantity"].get<double>();
            if (!std::isfinite(value) || value < 0.0) throw clippy::ApiError(400, "invalid_quantity", "Quantity must be finite and non-negative.");
            (*node)["quantity"] = value;
        }
        if (patch.contains("health")) {
            if (!patch["health"].is_number()) throw clippy::ApiError(400, "invalid_health", "Health must be a number.");
            const double value = patch["health"].get<double>();
            if (!std::isfinite(value) || value < 0.0) throw clippy::ApiError(400, "invalid_health", "Health must be finite and non-negative.");
            (*node)["health"] = value;
        }
        validate_tree(after);
        const auto now = clippy::now_unix_ms();
        store_root(db, storage_id, after, now);
        const auto revision = increment_revision(db, storage_id, now);
        const auto change_id = clippy::random_hex(16);
        insert_change_set(db, change_id, admin_session_id, windows_identity, "edit_item", storage_id, std::nullopt,
                          item_id, expected_revision, revision, std::nullopt, std::nullopt, reason, request_id, now);
        insert_change_entry(db, change_id, storage_id, root_item_id, item_id, before, after);
        insert_audit(db, admin_session_id, windows_identity, "edit_item", "item", item_id, "SUCCESS", reason, "",
                     request_id, change_id, {{"storage_id", storage_id}, {"root_item_id", root_item_id}, {"revision", revision}}, now);
        release_locks_in_transaction(db, locks, admin_session_id);
        transaction.commit();
        return change_response(change_id, storage_id, revision);
    } catch (...) {
        try { release_locks(db, locks, admin_session_id); } catch (...) {}
        throw;
    }
}

json AdminDatabase::remove_item(const std::string& storage_id, const std::string& root_item_id,
                                const std::string& item_id, std::int64_t expected_revision,
                                bool quarantine_item, const std::string& raw_reason,
                                const std::string& admin_session_id, const std::string& windows_identity,
                                const std::string& request_id) {
    require_editing(editing_enabled_, writer_pool_.get());
    const auto reason = bounded_reason(raw_reason);
    std::lock_guard lock(*writer_gate_);
    auto* db = writer_pool_.get();
    auto locks = acquire_locks(db, {{storage_id, expected_revision}}, admin_session_id, reason, maintenance_lock_seconds_);
    try {
        clippy::Transaction transaction(db);
        const auto current = lock_storage_row(db, storage_id);
        assert_revision(current, expected_revision);
        assert_locks_owned(db, locks, admin_session_id);
        assert_workflow_free(db, storage_id);
        auto before = load_root_for_update(db, storage_id, root_item_id);
        auto after = before;
        std::vector<std::size_t> path;
        if (!find_path(after, item_id, path)) throw clippy::ApiError(404, "item_not_found", "The selected item no longer exists in this root tree.");
        std::string parent_item_id;
        int parent_index = -1;
        json removed = detach_node(after, path, parent_item_id, parent_index);
        const bool root_removed = path.empty();
        const auto now = clippy::now_unix_ms();
        if (root_removed) delete_root(db, storage_id, root_item_id);
        else store_root(db, storage_id, after, now);
        const auto revision = increment_revision(db, storage_id, now);
        const auto change_id = clippy::random_hex(16);
        const auto action = quarantine_item ? std::string("quarantine_item") : std::string("remove_item");
        insert_change_set(db, change_id, admin_session_id, windows_identity, action, storage_id, std::nullopt,
                          item_id, expected_revision, revision, std::nullopt, std::nullopt, reason, request_id, now);
        insert_change_entry(db, change_id, storage_id, root_item_id, item_id, before,
                            root_removed ? std::optional<json>{} : std::optional<json>{after});
        std::optional<std::string> quarantine_id;
        if (quarantine_item) {
            quarantine_id = clippy::random_hex(16);
            Statement insert(db, R"SQL(
INSERT INTO admin_quarantine(quarantine_id,change_id,storage_id,root_item_id,item_id,parent_item_id,parent_index,tree_json,reason,created_ms)
VALUES(?1,?2,?3,?4,?5,?6,?7,?8::jsonb,?9,?10)
)SQL");
            insert.bind(1, *quarantine_id);
            insert.bind(2, change_id);
            insert.bind(3, storage_id);
            insert.bind(4, root_item_id);
            insert.bind(5, item_id);
            if (parent_item_id.empty()) insert.bind_null(6); else insert.bind(6, parent_item_id);
            if (parent_index < 0) insert.bind_null(7); else insert.bind(7, parent_index);
            insert.bind(8, removed.dump());
            insert.bind(9, reason);
            insert.bind(10, now);
            insert.done();
        }
        json detail = {{"storage_id", storage_id}, {"root_item_id", root_item_id}, {"revision", revision}};
        if (quarantine_id) detail["quarantine_id"] = *quarantine_id;
        insert_audit(db, admin_session_id, windows_identity, action, "item", item_id, "SUCCESS", reason, "",
                     request_id, change_id, detail, now);
        release_locks_in_transaction(db, locks, admin_session_id);
        transaction.commit();
        auto result = change_response(change_id, storage_id, revision);
        if (quarantine_id) result["quarantine_id"] = *quarantine_id;
        return result;
    } catch (...) {
        try { release_locks(db, locks, admin_session_id); } catch (...) {}
        throw;
    }
}

json AdminDatabase::copy_or_move_item(const std::string& source_storage_id,
                                      const std::string& source_root_item_id,
                                      const std::string& item_id,
                                      std::int64_t source_expected_revision,
                                      const std::string& target_storage_id,
                                      std::int64_t target_expected_revision,
                                      bool copy,
                                      const std::string& raw_reason,
                                      const std::string& admin_session_id,
                                      const std::string& windows_identity,
                                      const std::string& request_id) {
    require_editing(editing_enabled_, writer_pool_.get());
    const auto reason = bounded_reason(raw_reason);
    if (source_storage_id == target_storage_id && source_expected_revision != target_expected_revision) {
        throw clippy::ApiError(400, "revision_mismatch", "Source and target revisions must match when moving within one container.");
    }
    std::lock_guard lock(*writer_gate_);
    auto* db = writer_pool_.get();
    std::vector<std::pair<std::string,std::int64_t>> requested{{source_storage_id, source_expected_revision}};
    if (target_storage_id != source_storage_id) requested.push_back({target_storage_id, target_expected_revision});
    auto locks = acquire_locks(db, requested, admin_session_id, reason, maintenance_lock_seconds_);
    try {
        clippy::Transaction transaction(db);
        std::map<std::string,std::int64_t> revisions;
        std::vector<std::string> order{source_storage_id};
        if (target_storage_id != source_storage_id) order.push_back(target_storage_id);
        std::sort(order.begin(), order.end());
        for (const auto& id : order) revisions[id] = lock_storage_row(db, id);
        assert_revision(revisions[source_storage_id], source_expected_revision);
        assert_revision(revisions[target_storage_id], target_storage_id == source_storage_id ? source_expected_revision : target_expected_revision);
        assert_locks_owned(db, locks, admin_session_id);
        assert_workflow_free(db, source_storage_id);
        if (target_storage_id != source_storage_id) assert_workflow_free(db, target_storage_id);

        auto source_before = load_root_for_update(db, source_storage_id, source_root_item_id);
        validate_tree(source_before);
        auto source_after = source_before;
        std::vector<std::size_t> path;
        if (!find_path(source_after, item_id, path)) throw clippy::ApiError(404, "item_not_found", "The selected item no longer exists in the source root.");
        const auto* selected = node_at(source_before, path);
        if (!selected) throw clippy::ApiError(409, "item_tree_changed", "The source item tree changed.", true);
        json moved = *selected;
        const bool selected_root = path.empty();
        if (!copy) {
            std::string parent;
            int index = -1;
            moved = detach_node(source_after, path, parent, index);
        } else {
            regenerate_item_ids(moved);
        }
        if (!moved.contains("location") || !moved["location"].is_object()) moved["location"] = json::object();
        moved["location"]["kind"] = "cargo";
        const auto moved_stats = validate_tree(moved);
        const auto target_root_id = moved.value("item_id", "");
        if (!copy && selected_root && source_storage_id == target_storage_id) {
            throw clippy::ApiError(400, "same_storage_move", "A root item is already in this container. Choose another target container.");
        }
        ensure_root_capacity(db, target_storage_id, 1);
        const auto excluded_root = (!copy && target_storage_id == source_storage_id) ? source_root_item_id : std::string{};
        ensure_item_ids_available(db, target_storage_id, moved_stats, excluded_root);

        const auto now = clippy::now_unix_ms();
        if (!copy) {
            if (selected_root) delete_root(db, source_storage_id, source_root_item_id);
            else store_root(db, source_storage_id, source_after, now);
        }
        store_root(db, target_storage_id, moved, now);

        std::int64_t source_after_revision = source_expected_revision;
        std::int64_t target_after_revision = target_storage_id == source_storage_id ? source_expected_revision : target_expected_revision;
        if (target_storage_id == source_storage_id) {
            source_after_revision = increment_revision(db, source_storage_id, now);
            target_after_revision = source_after_revision;
        } else {
            if (!copy) source_after_revision = increment_revision(db, source_storage_id, now);
            target_after_revision = increment_revision(db, target_storage_id, now);
        }

        const auto change_id = clippy::random_hex(16);
        insert_change_set(db, change_id, admin_session_id, windows_identity, copy ? "copy_item" : "move_item",
                          source_storage_id, target_storage_id, item_id,
                          source_expected_revision, source_after_revision,
                          target_storage_id == source_storage_id ? std::optional<std::int64_t>{} : std::optional<std::int64_t>{target_expected_revision},
                          target_storage_id == source_storage_id ? std::optional<std::int64_t>{} : std::optional<std::int64_t>{target_after_revision},
                          reason, request_id, now);
        if (!copy) {
            insert_change_entry(db, change_id, source_storage_id, source_root_item_id, item_id, source_before,
                                selected_root ? std::optional<json>{} : std::optional<json>{source_after});
        }
        insert_change_entry(db, change_id, target_storage_id, target_root_id, target_root_id, std::nullopt, moved);
        insert_audit(db, admin_session_id, windows_identity, copy ? "copy_item" : "move_item", "item", item_id,
                     "SUCCESS", reason, "", request_id, change_id,
                     {{"source_storage_id", source_storage_id}, {"target_storage_id", target_storage_id},
                      {"target_root_item_id", target_root_id}, {"source_revision", source_after_revision},
                      {"target_revision", target_after_revision}}, now);
        release_locks_in_transaction(db, locks, admin_session_id);
        transaction.commit();
        return change_response(change_id, source_storage_id, source_after_revision, target_storage_id, target_after_revision);
    } catch (...) {
        try { release_locks(db, locks, admin_session_id); } catch (...) {}
        throw;
    }
}

json AdminDatabase::create_snapshot(const std::string& storage_id, std::int64_t expected_revision,
                                    const std::string& raw_reason, const std::string& admin_session_id,
                                    const std::string& windows_identity, const std::string& request_id) {
    require_editing(editing_enabled_, writer_pool_.get());
    const auto reason = bounded_reason(raw_reason);
    std::lock_guard lock(*writer_gate_);
    auto* db = writer_pool_.get();
    clippy::Transaction transaction(db);
    const auto current = lock_storage_row(db, storage_id);
    assert_revision(current, expected_revision);
    Statement counts(db, "SELECT count(*),COALESCE(sum(node_count),0) FROM cargo_roots WHERE storage_id=?1");
    counts.bind(1, storage_id);
    counts.row();
    const auto snapshot_id = clippy::random_hex(16);
    const auto now = clippy::now_unix_ms();
    Statement insert(db, R"SQL(
INSERT INTO admin_storage_snapshots(snapshot_id,storage_id,revision,root_count,node_count,reason,admin_session_id,windows_identity,created_ms)
VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)
)SQL");
    insert.bind(1, snapshot_id); insert.bind(2, storage_id); insert.bind(3, current);
    insert.bind(4, counts.integer(0)); insert.bind(5, counts.integer(1)); insert.bind(6, reason);
    insert.bind(7, admin_session_id); insert.bind(8, windows_identity); insert.bind(9, now); insert.done();
    Statement copy_roots(db, R"SQL(
INSERT INTO admin_snapshot_roots(snapshot_id,root_item_id,tree_json)
SELECT ?1,root_item_id,tree_json FROM cargo_roots WHERE storage_id=?2 ORDER BY root_item_id
)SQL");
    copy_roots.bind(1, snapshot_id); copy_roots.bind(2, storage_id); copy_roots.done();
    insert_audit(db, admin_session_id, windows_identity, "create_snapshot", "container", storage_id,
                 "SUCCESS", reason, "", request_id, std::nullopt,
                 {{"snapshot_id", snapshot_id}, {"revision", current}, {"root_count", counts.integer(0)}, {"node_count", counts.integer(1)}}, now);
    transaction.commit();
    return {{"snapshot_id", snapshot_id}, {"storage_id", storage_id}, {"revision", current},
            {"root_count", counts.integer(0)}, {"node_count", counts.integer(1)}};
}

json AdminDatabase::restore_quarantine(const std::string& quarantine_id, std::int64_t expected_revision,
                                       const std::string& raw_reason, const std::string& admin_session_id,
                                       const std::string& windows_identity, const std::string& request_id) {
    require_editing(editing_enabled_, writer_pool_.get());
    const auto reason = bounded_reason(raw_reason);
    std::lock_guard lock(*writer_gate_);
    auto* db = writer_pool_.get();

    std::string storage_id;
    {
        clippy::Transaction lookup_tx(db);
        Statement lookup(db, "SELECT storage_id FROM admin_quarantine WHERE quarantine_id=?1 AND restored_ms IS NULL");
        lookup.bind(1, quarantine_id);
        if (!lookup.row()) throw clippy::ApiError(404, "quarantine_not_found", "The quarantine entry does not exist or was already restored.");
        storage_id = lookup.text(0);
        lookup_tx.commit();
    }
    auto locks = acquire_locks(db, {{storage_id, expected_revision}}, admin_session_id, reason, maintenance_lock_seconds_);
    try {
        clippy::Transaction transaction(db);
        const auto current = lock_storage_row(db, storage_id);
        assert_revision(current, expected_revision);
        assert_locks_owned(db, locks, admin_session_id);
        assert_workflow_free(db, storage_id);
        Statement q(db, R"SQL(
SELECT root_item_id,item_id,parent_item_id,parent_index,tree_json::text
FROM admin_quarantine WHERE quarantine_id=?1 AND restored_ms IS NULL FOR UPDATE
)SQL");
        q.bind(1, quarantine_id);
        if (!q.row()) throw clippy::ApiError(404, "quarantine_not_found", "The quarantine entry does not exist or was already restored.");
        const auto root_item_id = q.text(0);
        const auto item_id = q.text(1);
        const bool root_restore = q.is_null(2);
        const std::string parent_item_id = root_restore ? "" : q.text(2);
        const int parent_index = q.is_null(3) ? -1 : static_cast<int>(q.integer(3));
        auto quarantined_tree = json::parse(q.text(4));
        std::optional<json> before;
        json after;
        if (root_restore) {
            before = load_optional_root_for_update(db, storage_id, root_item_id);
            if (before) throw clippy::ApiError(409, "restore_conflict", "A root with the original item ID already exists. The quarantine entry was not restored.");
            ensure_root_capacity(db, storage_id, 1);
            after = quarantined_tree;
            store_root(db, storage_id, after, clippy::now_unix_ms());
        } else {
            before = load_root_for_update(db, storage_id, root_item_id);
            after = *before;
            std::vector<std::size_t> duplicate_path;
            if (find_path(after, item_id, duplicate_path)) throw clippy::ApiError(409, "restore_conflict", "The quarantined item ID already exists in the current root.");
            std::vector<std::size_t> parent_path;
            if (!find_path(after, parent_item_id, parent_path)) throw clippy::ApiError(409, "restore_parent_missing", "The original parent item no longer exists. Restore was refused.");
            auto* parent = node_at(after, parent_path);
            if (!parent) throw clippy::ApiError(409, "restore_parent_missing", "The original parent item no longer exists.");
            if (!parent->contains("children") || !(*parent)["children"].is_array()) (*parent)["children"] = json::array();
            auto& children = (*parent)["children"];
            const auto position = static_cast<std::size_t>((std::max)(0, (std::min)(parent_index, static_cast<int>(children.size()))));
            children.insert(children.begin() + static_cast<json::difference_type>(position), quarantined_tree);
            store_root(db, storage_id, after, clippy::now_unix_ms());
        }
        const auto now = clippy::now_unix_ms();
        const auto revision = increment_revision(db, storage_id, now);
        const auto change_id = clippy::random_hex(16);
        insert_change_set(db, change_id, admin_session_id, windows_identity, "restore_quarantine", storage_id,
                          std::nullopt, item_id, expected_revision, revision, std::nullopt, std::nullopt,
                          reason, request_id, now);
        insert_change_entry(db, change_id, storage_id, root_item_id, item_id, before, after);
        Statement restore(db, "UPDATE admin_quarantine SET restored_change_id=?2,restored_ms=?3 WHERE quarantine_id=?1 AND restored_ms IS NULL");
        restore.bind(1, quarantine_id); restore.bind(2, change_id); restore.bind(3, now); restore.done();
        insert_audit(db, admin_session_id, windows_identity, "restore_quarantine", "quarantine", quarantine_id,
                     "SUCCESS", reason, "", request_id, change_id,
                     {{"storage_id", storage_id}, {"item_id", item_id}, {"revision", revision}}, now);
        release_locks_in_transaction(db, locks, admin_session_id);
        transaction.commit();
        return change_response(change_id, storage_id, revision);
    } catch (...) {
        try { release_locks(db, locks, admin_session_id); } catch (...) {}
        throw;
    }
}

json AdminDatabase::undo_change(const std::string& change_id, const std::string& raw_reason,
                                const std::string& admin_session_id, const std::string& windows_identity,
                                const std::string& request_id) {
    require_editing(editing_enabled_, writer_pool_.get());
    const auto reason = bounded_reason(raw_reason);
    std::lock_guard lock(*writer_gate_);
    auto* db = writer_pool_.get();

    struct Entry { std::string storage_id; std::string root_item_id; std::string item_id; std::optional<json> before; };
    std::vector<Entry> entries;
    std::string original_action;
    std::string source_storage;
    std::optional<std::string> target_storage;
    std::int64_t source_expected = 0;
    std::optional<std::int64_t> target_expected;
    {
        clippy::Transaction lookup_tx(db);
        Statement change(db, R"SQL(
SELECT action_type,storage_id,target_storage_id,after_revision,target_after_revision,status
FROM admin_change_sets WHERE change_id=?1 FOR UPDATE
)SQL");
        change.bind(1, change_id);
        if (!change.row()) throw clippy::ApiError(404, "change_not_found", "The admin change does not exist.");
        if (change.text(5) != "APPLIED") throw clippy::ApiError(409, "change_not_undoable", "This change is no longer in an applied state.");
        original_action = change.text(0);
        if (original_action == "undo") throw clippy::ApiError(409, "change_not_undoable", "Undo records are not themselves undoable from the panel.");
        source_storage = change.text(1);
        source_expected = change.integer(3);
        if (!change.is_null(2)) target_storage = change.text(2);
        if (!change.is_null(4)) target_expected = change.integer(4);
        Statement rows(db, "SELECT storage_id,root_item_id,COALESCE(item_id,''),before_state::text FROM admin_change_entries WHERE change_id=?1 ORDER BY entry_id");
        rows.bind(1, change_id);
        while (rows.row()) {
            Entry entry{rows.text(0), rows.text(1), rows.text(2), std::nullopt};
            if (!rows.is_null(3)) entry.before = json::parse(rows.text(3));
            entries.push_back(std::move(entry));
        }
        if (entries.empty()) throw clippy::ApiError(409, "change_not_undoable", "This change has no recoverable before-state entries.");
        lookup_tx.commit();
    }

    std::map<std::string,std::int64_t> expected;
    for (const auto& entry : entries) {
        if (entry.storage_id == source_storage) expected[entry.storage_id] = source_expected;
        else if (target_storage && entry.storage_id == *target_storage && target_expected) expected[entry.storage_id] = *target_expected;
        else throw clippy::ApiError(409, "change_not_undoable", "The change revision record is incomplete.");
    }
    std::vector<std::pair<std::string,std::int64_t>> requested(expected.begin(), expected.end());
    auto locks = acquire_locks(db, requested, admin_session_id, reason, maintenance_lock_seconds_);
    try {
        clippy::Transaction transaction(db);
        for (const auto& [storage_id, revision] : expected) {
            const auto current = lock_storage_row(db, storage_id);
            assert_revision(current, revision);
            assert_workflow_free(db, storage_id);
        }
        assert_locks_owned(db, locks, admin_session_id);
        const auto now = clippy::now_unix_ms();
        const auto undo_id = clippy::random_hex(16);
        std::vector<std::tuple<Entry,std::optional<json>>> undo_entries;
        for (const auto& entry : entries) {
            auto current = load_optional_root_for_update(db, entry.storage_id, entry.root_item_id);
            undo_entries.emplace_back(entry, current);
            if (entry.before) store_root(db, entry.storage_id, *entry.before, now);
            else if (current) delete_root(db, entry.storage_id, entry.root_item_id);
        }
        std::map<std::string,std::int64_t> after;
        for (const auto& [storage_id, revision] : expected) after[storage_id] = increment_revision(db, storage_id, now);

        const auto source_after = after.count(source_storage) ? after[source_storage] : source_expected;
        std::optional<std::int64_t> target_after;
        if (target_storage && after.count(*target_storage)) target_after = after[*target_storage];
        insert_change_set(db, undo_id, admin_session_id, windows_identity, "undo", source_storage, target_storage,
                          std::nullopt, source_expected, source_after,
                          target_storage && target_expected ? target_expected : std::optional<std::int64_t>{}, target_after,
                          reason, request_id, now);
        for (const auto& [entry, current] : undo_entries) {
            insert_change_entry(db, undo_id, entry.storage_id, entry.root_item_id, entry.item_id, current, entry.before);
        }
        if (original_action == "quarantine_item" || original_action == "bulk_quarantine_roots") {
            Statement mark_quarantine(db, "UPDATE admin_quarantine SET restored_change_id=?2,restored_ms=?3 WHERE change_id=?1 AND restored_ms IS NULL");
            mark_quarantine.bind(1, change_id); mark_quarantine.bind(2, undo_id); mark_quarantine.bind(3, now); mark_quarantine.done();
        } else if (original_action == "restore_quarantine") {
            Statement reactivate_quarantine(db, "UPDATE admin_quarantine SET restored_change_id=NULL,restored_ms=NULL WHERE restored_change_id=?1");
            reactivate_quarantine.bind(1, change_id); reactivate_quarantine.done();
        }
        Statement mark(db, "UPDATE admin_change_sets SET status='UNDONE',undone_ms=?2,undo_change_id=?3 WHERE change_id=?1 AND status='APPLIED'");
        mark.bind(1, change_id); mark.bind(2, now); mark.bind(3, undo_id); mark.done();
        if (mark.affected_rows() != 1) throw clippy::ApiError(409, "change_not_undoable", "The change was modified by another admin session before undo completed.", true);
        insert_audit(db, admin_session_id, windows_identity, "undo_change", "change", change_id, "SUCCESS", reason, "",
                     request_id, undo_id, {{"original_change_id", change_id}}, now);
        release_locks_in_transaction(db, locks, admin_session_id);
        transaction.commit();
        json result = {{"change_id", undo_id}, {"undid_change_id", change_id}, {"revisions", json::object()}};
        for (const auto& [storage_id, revision] : after) result["revisions"][storage_id] = revision;
        return result;
    } catch (...) {
        try { release_locks(db, locks, admin_session_id); } catch (...) {}
        throw;
    }
}

json AdminDatabase::bulk_preview(const json& items, const std::string& admin_session_id) {
    const auto parsed = parse_bulk_roots(items);
    std::lock_guard lock(gate_);
    json normalized = json::array();
    json conflicts = json::array();
    std::set<std::string> containers;
    std::int64_t total_nodes = 0;
    for (const auto& item : parsed) {
        Statement query(pool_.get(), R"SQL(
SELECT c.revision,r.node_count,r.class_name,
       EXISTS(SELECT 1 FROM cargo_sessions s WHERE s.storage_id=c.storage_id AND s.status IN ('OPEN','MATERIALIZED','COMMITTED')),
       EXISTS(SELECT 1 FROM operations o WHERE o.storage_id=c.storage_id AND (o.status IN ('PREPARED','QUARANTINED') OR o.cleanup_state='PENDING')),
       EXISTS(SELECT 1 FROM cargo_migrations m WHERE m.storage_id=c.storage_id AND m.status IN ('PREPARED','COMMITTED')),
       EXISTS(SELECT 1 FROM admin_container_locks l WHERE l.storage_id=c.storage_id AND l.expires_ms>CAST(EXTRACT(EPOCH FROM clock_timestamp())*1000 AS BIGINT) AND l.admin_session_id<>?3)
FROM storage_containers c
LEFT JOIN cargo_roots r ON r.storage_id=c.storage_id AND r.root_item_id=?2
WHERE c.storage_id=?1
)SQL");
        query.bind(1, item.storage_id);
        query.bind(2, item.root_item_id);
        query.bind(3, admin_session_id);
        if (!query.row()) {
            conflicts.push_back({{"storage_id",item.storage_id},{"root_item_id",item.root_item_id},{"kind","storage_missing"}});
            continue;
        }
        const auto current_revision = query.integer(0);
        if (query.is_null(1)) {
            conflicts.push_back({{"storage_id",item.storage_id},{"root_item_id",item.root_item_id},{"kind","root_missing"}});
            continue;
        }
        const auto node_count = query.integer(1);
        const bool session = query.text(3) == "t";
        const bool operation = query.text(4) == "t";
        const bool migration = query.text(5) == "t";
        const bool admin_lock = query.text(6) == "t";
        if (current_revision != item.expected_revision) {
            conflicts.push_back({{"storage_id",item.storage_id},{"root_item_id",item.root_item_id},{"kind","revision_conflict"},
                                 {"expected_revision",item.expected_revision},{"current_revision",current_revision}});
        }
        if (session || operation || migration || admin_lock) {
            conflicts.push_back({{"storage_id",item.storage_id},{"root_item_id",item.root_item_id},{"kind","storage_busy"},
                                 {"session",session},{"operation",operation},{"migration",migration},{"admin_lock",admin_lock}});
        }
        normalized.push_back({{"storage_id",item.storage_id},{"root_item_id",item.root_item_id},{"item_id",item.item_id},
                              {"expected_revision",item.expected_revision},{"current_revision",current_revision},
                              {"class_name",query.text(2)},{"node_count",node_count}});
        containers.insert(item.storage_id);
        total_nodes += node_count;
    }
    return {{"items",std::move(normalized)},{"roots_affected",static_cast<std::int64_t>(parsed.size())},
            {"containers_affected",static_cast<std::int64_t>(containers.size())},{"nodes_affected",total_nodes},
            {"conflicts",std::move(conflicts)},{"can_execute",conflicts.empty()},{"max_batch_roots",25}};
}

json AdminDatabase::bulk_transfer_preview(const json& items, const std::string& target_storage_id,
                                          std::int64_t target_expected_revision, const std::string& admin_session_id) {
    auto result = bulk_preview(items, admin_session_id);
    const auto parsed = parse_bulk_roots(items);
    auto& conflicts = result["conflicts"];
    const auto source_storage_id = parsed.front().storage_id;
    const auto source_revision = parsed.front().expected_revision;
    for (const auto& item : parsed) {
        if (item.storage_id != source_storage_id) {
            conflicts.push_back({{"kind","bulk_single_source"},{"message","Bulk move/copy requires all selected roots to use one source container."}});
            break;
        }
        if (item.expected_revision != source_revision) {
            conflicts.push_back({{"kind","revision_mismatch"},{"message","Selected roots from the source container use different revisions."}});
            break;
        }
    }
    if (!valid_admin_identifier(target_storage_id) || target_expected_revision < 0) {
        conflicts.push_back({{"kind","invalid_target"},{"message","The target storage ID or revision is invalid."}});
        result["can_execute"] = false;
        return result;
    }
    if (target_storage_id == source_storage_id) {
        conflicts.push_back({{"kind","same_storage_transfer"},{"message","Choose a different target container for bulk move/copy."}});
        result["can_execute"] = false;
        return result;
    }
    std::lock_guard lock(gate_);
    Statement target(pool_.get(), R"SQL(
SELECT c.revision,c.capacity_slots,(SELECT count(*) FROM cargo_roots r WHERE r.storage_id=c.storage_id),
       EXISTS(SELECT 1 FROM cargo_sessions s WHERE s.storage_id=c.storage_id AND s.status IN ('OPEN','MATERIALIZED','COMMITTED')),
       EXISTS(SELECT 1 FROM operations o WHERE o.storage_id=c.storage_id AND (o.status IN ('PREPARED','QUARANTINED') OR o.cleanup_state='PENDING')),
       EXISTS(SELECT 1 FROM cargo_migrations m WHERE m.storage_id=c.storage_id AND m.status IN ('PREPARED','COMMITTED')),
       EXISTS(SELECT 1 FROM admin_container_locks l WHERE l.storage_id=c.storage_id AND l.expires_ms>CAST(EXTRACT(EPOCH FROM clock_timestamp())*1000 AS BIGINT) AND l.admin_session_id<>?2),
       c.display_name
FROM storage_containers c WHERE c.storage_id=?1
)SQL");
    target.bind(1, target_storage_id);
    target.bind(2, admin_session_id);
    if (!target.row()) {
        conflicts.push_back({{"kind","target_missing"},{"storage_id",target_storage_id}});
    } else {
        const auto current_revision = target.integer(0);
        const auto capacity = target.integer(1);
        const auto roots = target.integer(2);
        const bool session = target.text(3) == "t";
        const bool operation = target.text(4) == "t";
        const bool migration = target.text(5) == "t";
        const bool admin_lock = target.text(6) == "t";
        result["target"] = {{"storage_id",target_storage_id},{"display_name",target.text(7)},
                            {"expected_revision",target_expected_revision},{"current_revision",current_revision},
                            {"capacity_slots",capacity},{"root_count",roots}};
        if (current_revision != target_expected_revision) {
            conflicts.push_back({{"kind","target_revision_conflict"},{"storage_id",target_storage_id},
                                 {"expected_revision",target_expected_revision},{"current_revision",current_revision}});
        }
        if (roots + static_cast<std::int64_t>(parsed.size()) > capacity) {
            conflicts.push_back({{"kind","target_capacity"},{"storage_id",target_storage_id},
                                 {"capacity_slots",capacity},{"current_roots",roots},{"additional_roots",static_cast<std::int64_t>(parsed.size())}});
        }
        if (session || operation || migration || admin_lock) {
            conflicts.push_back({{"kind","target_busy"},{"storage_id",target_storage_id},
                                 {"session",session},{"operation",operation},{"migration",migration},{"admin_lock",admin_lock}});
        }
    }
    result["containers_affected"] = 2;
    result["can_execute"] = conflicts.empty();
    return result;
}

json AdminDatabase::bulk_remove_roots(const json& items, bool quarantine_items,
                                      const std::string& raw_reason, const std::string& admin_session_id,
                                      const std::string& windows_identity, const std::string& request_id) {
    require_editing(editing_enabled_, writer_pool_.get());
    const auto reason = bounded_reason(raw_reason);
    if (reason.empty()) throw clippy::ApiError(400, "reason_required", "A reason is required for a bulk change.");
    const auto parsed = parse_bulk_roots(items);
    std::map<std::string,std::int64_t> expected;
    for (const auto& item : parsed) {
        const auto found = expected.find(item.storage_id);
        if (found != expected.end() && found->second != item.expected_revision) {
            throw clippy::ApiError(400, "revision_mismatch", "All selected roots from one container must use the same expected revision.");
        }
        expected[item.storage_id] = item.expected_revision;
    }
    std::vector<std::pair<std::string,std::int64_t>> requested(expected.begin(), expected.end());
    std::lock_guard lock(*writer_gate_);
    auto* db = writer_pool_.get();
    auto locks = acquire_locks(db, requested, admin_session_id, reason, maintenance_lock_seconds_);
    try {
        clippy::Transaction transaction(db);
        for (const auto& [storage_id, revision] : expected) {
            const auto current = lock_storage_row(db, storage_id);
            assert_revision(current, revision);
            assert_workflow_free(db, storage_id);
        }
        assert_locks_owned(db, locks, admin_session_id);

        struct RootBefore { BulkRootSelection selection; json tree; std::int64_t nodes; };
        std::vector<RootBefore> before;
        before.reserve(parsed.size());
        for (const auto& item : parsed) {
            auto tree = load_root_for_update(db, item.storage_id, item.root_item_id);
            if (tree.value("item_id", "") != item.root_item_id) {
                throw clippy::ApiError(409, "root_identity_mismatch", "A selected cargo root has an unexpected root item ID.", true);
            }
            const auto stats = validate_tree(tree);
            before.push_back({item, std::move(tree), static_cast<std::int64_t>(stats.nodes)});
        }

        const auto now = clippy::now_unix_ms();
        for (const auto& row : before) delete_root(db, row.selection.storage_id, row.selection.root_item_id);
        std::map<std::string,std::int64_t> after_revision;
        for (const auto& [storage_id, revision] : expected) {
            (void)revision;
            after_revision[storage_id] = increment_revision(db, storage_id, now);
        }

        const auto bulk_id = clippy::random_hex(16);
        json changes = json::array();
        for (const auto& [storage_id, before_revision] : expected) {
            const auto change_id = clippy::random_hex(16);
            insert_change_set(db, change_id, admin_session_id, windows_identity,
                              quarantine_items ? "bulk_quarantine_roots" : "bulk_remove_roots",
                              storage_id, std::nullopt, std::nullopt, before_revision, after_revision[storage_id],
                              std::nullopt, std::nullopt, reason, request_id, now);
            std::int64_t roots = 0;
            std::int64_t nodes = 0;
            for (const auto& row : before) {
                if (row.selection.storage_id != storage_id) continue;
                ++roots;
                nodes += row.nodes;
                insert_change_entry(db, change_id, storage_id, row.selection.root_item_id, row.selection.item_id, row.tree, std::nullopt);
                if (quarantine_items) {
                    Statement insert(db, R"SQL(
INSERT INTO admin_quarantine(quarantine_id,change_id,storage_id,root_item_id,item_id,parent_item_id,parent_index,tree_json,reason,created_ms)
VALUES(?1,?2,?3,?4,?5,NULL,NULL,?6::jsonb,?7,?8)
)SQL");
                    insert.bind(1, clippy::random_hex(16));
                    insert.bind(2, change_id);
                    insert.bind(3, storage_id);
                    insert.bind(4, row.selection.root_item_id);
                    insert.bind(5, row.selection.item_id);
                    insert.bind(6, row.tree.dump());
                    insert.bind(7, reason);
                    insert.bind(8, now);
                    insert.done();
                }
            }
            insert_audit(db, admin_session_id, windows_identity,
                         quarantine_items ? "bulk_quarantine_roots" : "bulk_remove_roots",
                         "container", storage_id, "SUCCESS", reason, "", request_id, change_id,
                         {{"bulk_id",bulk_id},{"roots",roots},{"nodes",nodes},{"revision",after_revision[storage_id]}}, now);
            changes.push_back({{"change_id",change_id},{"storage_id",storage_id},{"before_revision",before_revision},
                               {"after_revision",after_revision[storage_id]},{"roots",roots},{"nodes",nodes}});
        }
        release_locks_in_transaction(db, locks, admin_session_id);
        transaction.commit();
        return {{"bulk_id",bulk_id},{"action",quarantine_items?"quarantine":"remove"},
                {"roots_affected",static_cast<std::int64_t>(before.size())},{"changes",std::move(changes)}};
    } catch (...) {
        try { release_locks(db, locks, admin_session_id); } catch (...) {}
        throw;
    }
}

json AdminDatabase::bulk_transfer_roots(const json& items, const std::string& target_storage_id,
                                       std::int64_t target_expected_revision, bool copy,
                                       const std::string& raw_reason, const std::string& admin_session_id,
                                       const std::string& windows_identity, const std::string& request_id) {
    require_editing(editing_enabled_, writer_pool_.get());
    const auto reason = bounded_reason(raw_reason);
    if (reason.empty()) throw clippy::ApiError(400, "reason_required", "A reason is required for a bulk transfer.");
    if (!valid_admin_identifier(target_storage_id) || target_expected_revision < 0) {
        throw clippy::ApiError(400, "invalid_target", "The target storage ID or revision is invalid.");
    }
    const auto parsed = parse_bulk_roots(items);
    const auto source_storage_id = parsed.front().storage_id;
    const auto source_expected_revision = parsed.front().expected_revision;
    if (source_storage_id == target_storage_id) {
        throw clippy::ApiError(400, "same_storage_transfer", "Bulk move/copy requires a different target container.");
    }
    for (const auto& item : parsed) {
        if (item.storage_id != source_storage_id) {
            throw clippy::ApiError(400, "bulk_single_source", "Bulk move/copy requires every selected root to come from the same source container.");
        }
        if (item.expected_revision != source_expected_revision) {
            throw clippy::ApiError(400, "revision_mismatch", "Every selected root must use the same current source revision.");
        }
    }

    std::lock_guard lock(*writer_gate_);
    auto* db = writer_pool_.get();
    auto locks = acquire_locks(db, {{source_storage_id, source_expected_revision}, {target_storage_id, target_expected_revision}},
                               admin_session_id, reason, maintenance_lock_seconds_);
    try {
        clippy::Transaction transaction(db);
        std::vector<std::string> order{source_storage_id, target_storage_id};
        std::sort(order.begin(), order.end());
        std::map<std::string,std::int64_t> current;
        for (const auto& id : order) current[id] = lock_storage_row(db, id);
        assert_revision(current[source_storage_id], source_expected_revision);
        assert_revision(current[target_storage_id], target_expected_revision);
        assert_workflow_free(db, source_storage_id);
        assert_workflow_free(db, target_storage_id);
        assert_locks_owned(db, locks, admin_session_id);
        ensure_root_capacity(db, target_storage_id, static_cast<std::int64_t>(parsed.size()));

        struct TransferRoot { std::string original_root_id; json source_tree; json target_tree; TreeStats target_stats; };
        std::vector<TransferRoot> roots;
        roots.reserve(parsed.size());
        std::set<std::string> batch_item_ids;
        for (const auto& selection : parsed) {
            auto source_tree = load_root_for_update(db, source_storage_id, selection.root_item_id);
            if (source_tree.value("item_id", "") != selection.root_item_id) {
                throw clippy::ApiError(409, "root_identity_mismatch", "A selected cargo root has an unexpected root item ID.", true);
            }
            validate_tree(source_tree);
            auto target_tree = source_tree;
            if (copy) regenerate_item_ids(target_tree);
            if (!target_tree.contains("location") || !target_tree["location"].is_object()) target_tree["location"] = json::object();
            target_tree["location"]["kind"] = "cargo";
            auto stats = validate_tree(target_tree);
            for (const auto& id_value : stats.ids) {
                const auto id = id_value.get<std::string>();
                if (!batch_item_ids.insert(id).second) {
                    throw clippy::ApiError(409, "duplicate_item_id", "The selected roots would create a duplicate item ID in the target container.");
                }
            }
            ensure_item_ids_available(db, target_storage_id, stats);
            roots.push_back({selection.root_item_id, std::move(source_tree), std::move(target_tree), std::move(stats)});
        }

        const auto now = clippy::now_unix_ms();
        if (!copy) {
            for (const auto& root : roots) delete_root(db, source_storage_id, root.original_root_id);
        }
        for (const auto& root : roots) store_root(db, target_storage_id, root.target_tree, now);

        auto source_after_revision = source_expected_revision;
        if (!copy) source_after_revision = increment_revision(db, source_storage_id, now);
        const auto target_after_revision = increment_revision(db, target_storage_id, now);
        const auto change_id = clippy::random_hex(16);
        const auto action = copy ? std::string("bulk_copy_roots") : std::string("bulk_move_roots");
        insert_change_set(db, change_id, admin_session_id, windows_identity, action,
                          source_storage_id, target_storage_id, std::nullopt,
                          source_expected_revision, source_after_revision,
                          target_expected_revision, target_after_revision,
                          reason, request_id, now);
        std::int64_t total_nodes = 0;
        for (const auto& root : roots) {
            if (!copy) insert_change_entry(db, change_id, source_storage_id, root.original_root_id,
                                           root.original_root_id, root.source_tree, std::nullopt);
            const auto target_root_id = root.target_tree.value("item_id", "");
            insert_change_entry(db, change_id, target_storage_id, target_root_id, target_root_id,
                                std::nullopt, root.target_tree);
            total_nodes += static_cast<std::int64_t>(root.target_stats.nodes);
        }
        insert_audit(db, admin_session_id, windows_identity, action, "selection", change_id, "SUCCESS", reason, "",
                     request_id, change_id,
                     {{"source_storage_id",source_storage_id},{"target_storage_id",target_storage_id},
                      {"roots",static_cast<std::int64_t>(roots.size())},{"nodes",total_nodes},
                      {"source_revision",source_after_revision},{"target_revision",target_after_revision}}, now);
        release_locks_in_transaction(db, locks, admin_session_id);
        transaction.commit();
        return {{"change_id",change_id},{"action",copy?"copy":"move"},{"source_storage_id",source_storage_id},
                {"target_storage_id",target_storage_id},{"roots_affected",static_cast<std::int64_t>(roots.size())},
                {"nodes_affected",total_nodes},{"source_revision",source_after_revision},{"target_revision",target_after_revision}};
    } catch (...) {
        try { release_locks(db, locks, admin_session_id); } catch (...) {}
        throw;
    }
}

json AdminDatabase::acquire_manual_lock(const std::string& storage_id, std::int64_t expected_revision,
                                        const std::string& raw_reason, const std::string& admin_session_id,
                                        const std::string& windows_identity, const std::string& request_id) {
    require_editing(editing_enabled_, writer_pool_.get());
    const auto reason = bounded_reason(raw_reason);
    if (reason.empty()) throw clippy::ApiError(400, "reason_required", "A reason is required for a maintenance lock.");
    std::lock_guard lock(*writer_gate_);
    auto* db = writer_pool_.get();
    clippy::Transaction transaction(db);
    const auto current = lock_storage_row(db, storage_id);
    assert_revision(current, expected_revision);
    const auto now = clippy::now_unix_ms();
    const auto expires = now + static_cast<std::int64_t>(maintenance_lock_seconds_) * 1000;
    delete_expired_locks(db, storage_id, now);
    assert_workflow_free(db, storage_id);
    Statement existing(db, "SELECT lock_id,admin_session_id FROM admin_container_locks WHERE storage_id=?1 FOR UPDATE");
    existing.bind(1, storage_id);
    bool renewed = false;
    std::string lock_id;
    if (existing.row()) {
        if (existing.text(1) != admin_session_id) {
            throw clippy::ApiError(409, "admin_lock_conflict", "Another local admin session is already maintaining this container.", true);
        }
        lock_id = existing.text(0);
        Statement update(db, "UPDATE admin_container_locks SET lock_reason=?2,expires_ms=?3 WHERE storage_id=?1 AND admin_session_id=?4");
        update.bind(1, storage_id); update.bind(2, reason); update.bind(3, expires); update.bind(4, admin_session_id); update.done();
        renewed = true;
    } else {
        lock_id = clippy::random_hex(16);
        Statement insert_lock(db, R"SQL(
INSERT INTO admin_container_locks(storage_id,lock_id,admin_session_id,lock_reason,created_ms,expires_ms)
VALUES(?1,?2,?3,?4,?5,?6)
)SQL");
        insert_lock.bind(1, storage_id); insert_lock.bind(2, lock_id); insert_lock.bind(3, admin_session_id);
        insert_lock.bind(4, reason); insert_lock.bind(5, now); insert_lock.bind(6, expires); insert_lock.done();
    }
    insert_audit(db, admin_session_id, windows_identity, renewed ? "renew_maintenance_lock" : "acquire_maintenance_lock",
                 "container", storage_id, "SUCCESS", reason, "", request_id, std::nullopt,
                 {{"revision",current},{"expires_ms",expires}}, now);
    transaction.commit();
    return {{"storage_id",storage_id},{"revision",current},{"expires_ms",expires},{"renewed",renewed}};
}

json AdminDatabase::release_manual_lock(const std::string& storage_id, const std::string& raw_reason,
                                        const std::string& admin_session_id, const std::string& windows_identity,
                                        const std::string& request_id) {
    require_editing(editing_enabled_, writer_pool_.get());
    const auto reason = bounded_reason(raw_reason);
    std::lock_guard lock(*writer_gate_);
    auto* db = writer_pool_.get();
    clippy::Transaction transaction(db);
    Statement existing(db, "SELECT admin_session_id FROM admin_container_locks WHERE storage_id=?1 FOR UPDATE");
    existing.bind(1, storage_id);
    if (!existing.row()) {
        transaction.commit();
        return {{"storage_id",storage_id},{"released",false}};
    }
    if (existing.text(0) != admin_session_id) {
        throw clippy::ApiError(409, "admin_lock_conflict", "This maintenance lock belongs to another local admin session.", true);
    }
    Statement remove(db, "DELETE FROM admin_container_locks WHERE storage_id=?1 AND admin_session_id=?2");
    remove.bind(1, storage_id); remove.bind(2, admin_session_id); remove.done();
    const auto now = clippy::now_unix_ms();
    insert_audit(db, admin_session_id, windows_identity, "release_maintenance_lock", "container", storage_id,
                 "SUCCESS", reason, "", request_id, std::nullopt, json::object(), now);
    transaction.commit();
    return {{"storage_id",storage_id},{"released",true}};
}


json AdminDatabase::enqueue_player_command(const std::string& player_id, const std::string& action,
                                           const json& raw_payload, const std::string& raw_reason,
                                           int expiry_seconds, const std::string& admin_session_id,
                                           const std::string& windows_identity, const std::string& request_id) {
    require_editing(editing_enabled_, writer_pool_.get());
    static const std::set<std::string> allowed_actions = {
        "REQUEST_SNAPSHOT","REMOVE_ITEM","GIVE_ITEM","REPAIR_ITEM","MOVE_ITEM","QUARANTINE_ITEM","RESTORE_QUARANTINE"
    };
    if (!allowed_actions.count(action)) throw clippy::ApiError(400, "invalid_player_command", "That live player command is not supported.");
    if (player_id.empty() || player_id.size() > 128) throw clippy::ApiError(400, "invalid_player", "The player ID is invalid.");
    if (expiry_seconds < 5 || expiry_seconds > 300) throw clippy::ApiError(400, "invalid_expiry", "Player commands must expire between 5 and 300 seconds.");
    if (!raw_payload.is_object()) throw clippy::ApiError(400, "invalid_player_command", "The player command payload must be a JSON object.");

    const auto reason = bounded_reason(raw_reason);
    json payload = json::object();
    auto require_payload_string = [&](const char* key, std::size_t maximum) {
        if (!raw_payload.contains(key) || !raw_payload[key].is_string()) throw clippy::ApiError(400, "invalid_player_command", std::string(key) + " is required.");
        const auto value = raw_payload[key].get<std::string>();
        if (value.empty() || value.size() > maximum || value.find('\0') != std::string::npos) throw clippy::ApiError(400, "invalid_player_command", std::string(key) + " is invalid.");
        return value;
    };

    if (action == "REMOVE_ITEM" || action == "REPAIR_ITEM" || action == "MOVE_ITEM" || action == "QUARANTINE_ITEM") payload["item_id"] = require_payload_string("item_id", 192);
    if (action == "GIVE_ITEM") {
        const auto class_name = require_payload_string("class_name", 128);
        if (!std::all_of(class_name.begin(), class_name.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; })) throw clippy::ApiError(400, "invalid_player_command", "class_name contains unsupported characters.");
        payload["class_name"] = class_name;
        if (raw_payload.contains("quantity")) {
            if (!raw_payload["quantity"].is_number()) throw clippy::ApiError(400,"invalid_player_command","quantity must be numeric.");
            const double quantity=raw_payload["quantity"].get<double>();
            if(!std::isfinite(quantity)||quantity<0.0||quantity>1.0e9) throw clippy::ApiError(400,"invalid_player_command","quantity is outside the supported range.");
            payload["quantity"]=quantity;
        }
        double health = raw_payload.value("health",1.0);
        if(!std::isfinite(health)||health<0.0||health>1.0) throw clippy::ApiError(400,"invalid_player_command","health must be between 0 and 1.");
        payload["health"]=health;
    }
    if (action == "REPAIR_ITEM") {
        double health = raw_payload.value("health",1.0);
        if(!std::isfinite(health)||health<0.0||health>1.0) throw clippy::ApiError(400,"invalid_player_command","health must be between 0 and 1.");
        payload["health"]=health;
    }
    if (action == "MOVE_ITEM") {
        const auto target = require_payload_string("target_player_id",128);
        if (target == player_id) throw clippy::ApiError(400,"invalid_player_command","The source and target players must be different.");
        payload["target_player_id"]=target;
    }

    std::lock_guard lock(*writer_gate_);
    auto* db=writer_pool_.get();
    clippy::Transaction transaction(db);
    Statement player(db,"SELECT 1 FROM players WHERE player_id=?1 FOR SHARE");
    player.bind(1,player_id);
    if(!player.row()) throw clippy::ApiError(404,"player_not_found","The player is not present in the telemetry registry.");

    if (action == "MOVE_ITEM") {
        Statement target(db,"SELECT 1 FROM players WHERE player_id=?1 FOR SHARE");
        target.bind(1,payload["target_player_id"].get<std::string>());
        if(!target.row()) throw clippy::ApiError(404,"target_player_not_found","The target player is not present in the telemetry registry.");
    }

    if (action == "RESTORE_QUARANTINE") {
        const auto quarantine_id = require_payload_string("quarantine_id",64);
        Statement quarantine(db,R"SQL(
SELECT tree_json::text FROM player_quarantine
WHERE quarantine_id=?1 AND player_id=?2 AND restored_ms IS NULL
FOR SHARE
)SQL");
        quarantine.bind(1,quarantine_id);quarantine.bind(2,player_id);
        if(!quarantine.row()) throw clippy::ApiError(404,"player_quarantine_not_found","The live player quarantine entry is missing or already restored.");
        payload["quarantine_id"]=quarantine_id;
        payload["item_tree"]=json::parse(quarantine.text(0));
    }

    const auto now=clippy::now_unix_ms();
    const auto command_id=clippy::random_hex(16);
    Statement insert(db,R"SQL(
INSERT INTO admin_player_commands(
    command_id,idempotency_key,player_id,action,payload_json,status,admin_session_id,
    windows_identity,request_id,reason,created_ms,expires_ms
) VALUES(?1,?2,?3,?4,?5::jsonb,'PENDING',?6,?7,?8,?9,?10,?11)
)SQL");
    insert.bind(1,command_id); insert.bind(2,request_id); insert.bind(3,player_id); insert.bind(4,action); insert.bind(5,payload.dump());
    insert.bind(6,admin_session_id); insert.bind(7,windows_identity); insert.bind(8,request_id); insert.bind(9,reason); insert.bind(10,now);
    insert.bind(11,now+static_cast<std::int64_t>(expiry_seconds)*1000); insert.done();

    insert_audit(db,admin_session_id,windows_identity,"enqueue_player_command","player",player_id,
                 "SUCCESS",reason,"",request_id,std::nullopt,
                 json{{"command_id",command_id},{"action",action},{"expires_in_seconds",expiry_seconds}},now);
    transaction.commit();
    return {{"command_id",command_id},{"player_id",player_id},{"action",action},{"status","PENDING"},{"created_ms",now},{"expires_ms",now+static_cast<std::int64_t>(expiry_seconds)*1000}};
}

void AdminDatabase::record_external_audit(const std::string& admin_session_id, const std::string& windows_identity,
                                          const std::string& action, const std::string& target_type,
                                          const std::string& target_id, const std::string& result,
                                          const std::string& raw_reason, const std::string& error,
                                          const std::string& request_id, const json& detail) {
    if (!editing_enabled_ || !writer_pool_ || !writer_gate_) return;
    const auto reason = bounded_reason(raw_reason);
    std::lock_guard lock(*writer_gate_);
    clippy::Transaction transaction(writer_pool_.get());
    insert_audit(writer_pool_.get(), admin_session_id, windows_identity, action, target_type, target_id, result,
                 reason, error.substr(0, 2048), request_id, std::nullopt, detail, clippy::now_unix_ms());
    transaction.commit();
}

json AdminDatabase::changes(const std::string& storage_id, std::int64_t before_ms,
                            const std::string& before_id, int limit) {
    std::lock_guard lock(gate_);
    if (before_ms <= 0) before_ms = (std::numeric_limits<std::int64_t>::max)();
    const auto cursor = before_id.empty() ? std::string(128, '~') : before_id;
    Statement query(pool_.get(), R"SQL(
SELECT change_id,action_type,storage_id,target_storage_id,item_id,before_revision,after_revision,
       target_before_revision,target_after_revision,reason,status,created_ms,undone_ms,undo_change_id,windows_identity
FROM admin_change_sets
WHERE (?1='' OR storage_id=?1 OR target_storage_id=?1) AND (created_ms,change_id)<(?2,?3)
ORDER BY created_ms DESC,change_id DESC LIMIT ?4
)SQL");
    query.bind(1, storage_id); query.bind(2, before_ms); query.bind(3, cursor); query.bind(4, static_cast<std::int64_t>(limit + 1));
    json rows = json::array();
    std::int64_t next_ms = 0; std::string next_id;
    while (query.row()) {
        if (rows.size() == static_cast<std::size_t>(limit)) { next_ms = rows.back()["created_ms"].get<std::int64_t>(); next_id = rows.back()["change_id"].get<std::string>(); break; }
        json row = {{"change_id",query.text(0)},{"action_type",query.text(1)},{"storage_id",query.text(2)},
                    {"before_revision",query.integer(5)},{"after_revision",query.integer(6)},{"reason",query.text(9)},
                    {"status",query.text(10)},{"created_ms",query.integer(11)},{"windows_identity",query.text(14)}};
        if (!query.is_null(3)) row["target_storage_id"] = query.text(3);
        if (!query.is_null(4)) row["item_id"] = query.text(4);
        if (!query.is_null(7)) row["target_before_revision"] = query.integer(7);
        if (!query.is_null(8)) row["target_after_revision"] = query.integer(8);
        if (!query.is_null(12)) row["undone_ms"] = query.integer(12);
        if (!query.is_null(13)) row["undo_change_id"] = query.text(13);
        rows.push_back(std::move(row));
    }
    json result={{"rows",std::move(rows)}};
    if (!next_id.empty()) { result["next_before_ms"]=next_ms; result["next_before_id"]=next_id; }
    return result;
}

json AdminDatabase::change_detail(const std::string& change_id) {
    std::lock_guard lock(gate_);
    Statement change(pool_.get(), R"SQL(
SELECT change_id,admin_session_id,windows_identity,action_type,storage_id,target_storage_id,item_id,
       before_revision,after_revision,target_before_revision,target_after_revision,reason,request_id,status,
       created_ms,undone_ms,undo_change_id
FROM admin_change_sets WHERE change_id=?1
)SQL");
    change.bind(1, change_id);
    if (!change.row()) throw clippy::ApiError(404, "change_not_found", "The admin change does not exist.");
    json result={{"change_id",change.text(0)},{"admin_session_id",change.text(1)},{"windows_identity",change.text(2)},
                 {"action_type",change.text(3)},{"storage_id",change.text(4)},{"before_revision",change.integer(7)},
                 {"after_revision",change.integer(8)},{"reason",change.text(11)},{"request_id",change.text(12)},
                 {"status",change.text(13)},{"created_ms",change.integer(14)}};
    if(!change.is_null(5)) result["target_storage_id"]=change.text(5);
    if(!change.is_null(6)) result["item_id"]=change.text(6);
    if(!change.is_null(9)) result["target_before_revision"]=change.integer(9);
    if(!change.is_null(10)) result["target_after_revision"]=change.integer(10);
    if(!change.is_null(15)) result["undone_ms"]=change.integer(15);
    if(!change.is_null(16)) result["undo_change_id"]=change.text(16);

    Statement entries(pool_.get(), R"SQL(
SELECT entry_id,storage_id,root_item_id,COALESCE(item_id,''),before_state::text,after_state::text
FROM admin_change_entries WHERE change_id=?1 ORDER BY entry_id
)SQL");
    entries.bind(1,change_id);
    result["entries"]=json::array();
    while(entries.row()) {
        json row={{"entry_id",entries.integer(0)},{"storage_id",entries.text(1)},{"root_item_id",entries.text(2)},
                  {"item_id",entries.text(3)}};
        if(!entries.is_null(4)) row["before_state"]=json::parse(entries.text(4));
        if(!entries.is_null(5)) row["after_state"]=json::parse(entries.text(5));
        result["entries"].push_back(std::move(row));
    }
    return result;
}

json AdminDatabase::item_history(const std::string& item_id, std::int64_t before_ms,
                                 const std::string& before_id, int limit) {
    std::lock_guard lock(gate_);
    if(before_ms<=0) before_ms=(std::numeric_limits<std::int64_t>::max)();
    const auto cursor=before_id.empty()?std::string(128,'~'):before_id;
    Statement query(pool_.get(), R"SQL(
SELECT DISTINCT c.change_id,c.action_type,c.storage_id,c.target_storage_id,c.item_id,c.before_revision,c.after_revision,
       c.reason,c.status,c.created_ms,c.windows_identity
FROM admin_change_entries e JOIN admin_change_sets c ON c.change_id=e.change_id
WHERE e.item_id=?1 AND (c.created_ms,c.change_id)<(?2,?3)
ORDER BY c.created_ms DESC,c.change_id DESC LIMIT ?4
)SQL");
    query.bind(1,item_id); query.bind(2,before_ms); query.bind(3,cursor); query.bind(4,static_cast<std::int64_t>(limit+1));
    json rows=json::array(); std::int64_t next_ms=0; std::string next_id;
    while(query.row()) {
        if(rows.size()==static_cast<std::size_t>(limit)){next_ms=rows.back()["created_ms"].get<std::int64_t>();next_id=rows.back()["change_id"].get<std::string>();break;}
        json row={{"change_id",query.text(0)},{"action_type",query.text(1)},{"storage_id",query.text(2)},
                  {"before_revision",query.integer(5)},{"after_revision",query.integer(6)},{"reason",query.text(7)},
                  {"status",query.text(8)},{"created_ms",query.integer(9)},{"windows_identity",query.text(10)}};
        if(!query.is_null(3)) row["target_storage_id"]=query.text(3);
        if(!query.is_null(4)) row["item_id"]=query.text(4);
        rows.push_back(std::move(row));
    }
    json result={{"rows",std::move(rows)}};
    if(!next_id.empty()){result["next_before_ms"]=next_ms;result["next_before_id"]=next_id;}
    return result;
}

json AdminDatabase::audit(std::int64_t before_ms, std::int64_t before_id, std::int64_t from_ms, std::int64_t to_ms, int limit,
                          const std::string& admin, const std::string& action,
                          const std::string& target_type, const std::string& target_id,
                          const std::string& result_filter) {
    std::lock_guard lock(gate_);
    if (before_ms <= 0) before_ms = (std::numeric_limits<std::int64_t>::max)();
    if (before_id <= 0) before_id = (std::numeric_limits<std::int64_t>::max)();
    Statement query(pool_.get(), R"SQL(
SELECT event_id,admin_session_id,windows_identity,action,target_type,target_id,result,reason,error,request_id,change_id,detail_json::text,created_ms
FROM admin_audit_events
WHERE (created_ms,event_id)<(?1,?2)
  AND (?3<=0 OR created_ms>=?3)
  AND (?4<=0 OR created_ms<=?4)
  AND (?5='' OR lower(windows_identity) LIKE lower(?5)||'%' ESCAPE E'\\')
  AND (?6='' OR action=?6)
  AND (?7='' OR target_type=?7)
  AND (?8='' OR lower(target_id) LIKE lower(?8)||'%' ESCAPE E'\\')
  AND (?9='' OR result=?9)
ORDER BY created_ms DESC,event_id DESC LIMIT ?10
)SQL");
    query.bind(1,before_ms); query.bind(2,before_id); query.bind(3,from_ms); query.bind(4,to_ms);
    query.bind(5,admin); query.bind(6,action); query.bind(7,target_type); query.bind(8,target_id); query.bind(9,result_filter);
    query.bind(10,static_cast<std::int64_t>(limit+1));
    json rows=json::array(); std::int64_t next_ms=0,next_id=0;
    while(query.row()) {
        if(rows.size()==static_cast<std::size_t>(limit)){next_ms=rows.back()["created_ms"].get<std::int64_t>();next_id=rows.back()["event_id"].get<std::int64_t>();break;}
        json row={{"event_id",query.integer(0)},{"admin_session_id",query.text(1)},{"windows_identity",query.text(2)},
                  {"action",query.text(3)},{"target_type",query.text(4)},{"target_id",query.text(5)},{"result",query.text(6)},
                  {"reason",query.text(7)},{"request_id",query.text(9)},{"detail",json::parse(query.text(11))},{"created_ms",query.integer(12)}};
        if (!query.is_null(8)) row["error"] = query.text(8);
        if (!query.is_null(10)) row["change_id"] = query.text(10);
        rows.push_back(std::move(row));
    }
    json result={{"rows",std::move(rows)}}; if(next_id){result["next_before_ms"]=next_ms;result["next_before_id"]=next_id;} return result;
}

json AdminDatabase::activity(std::int64_t before_ms, std::int64_t before_id, std::int64_t from_ms, std::int64_t to_ms, int limit,
                             const std::string& target, const std::string& event_type,
                             const std::string& source_filter) {
    std::lock_guard lock(gate_);
    if (before_ms <= 0) before_ms = (std::numeric_limits<std::int64_t>::max)();
    if (before_id <= 0) before_id = (std::numeric_limits<std::int64_t>::max)();
    Statement query(pool_.get(), R"SQL(
SELECT source,event_id,event_type,target_id,detail_json::text,created_ms FROM (
  SELECT 'storage'::text AS source,a.event_id,a.event_type,a.operation_id AS target_id,a.detail_json,a.created_ms
  FROM audit_events a
  UNION ALL
  SELECT 'admin'::text AS source,1000000000000+a.event_id AS event_id,a.action AS event_type,a.target_id,a.detail_json,a.created_ms
  FROM admin_audit_events a WHERE a.result='SUCCESS'
  UNION ALL
  SELECT 'player'::text AS source,2000000000000+p.event_id AS event_id,p.event_type,p.player_id AS target_id,p.detail_json,p.created_ms
  FROM player_events p
) events
WHERE (created_ms,event_id)<(?1,?2)
  AND (?3<=0 OR created_ms>=?3)
  AND (?4<=0 OR created_ms<=?4)
  AND (?5='' OR lower(target_id) LIKE lower(?5)||'%' ESCAPE E'\\')
  AND (?6='' OR event_type=?6)
  AND (?7='' OR source=?7)
ORDER BY created_ms DESC,event_id DESC LIMIT ?8
)SQL");
    query.bind(1,before_ms); query.bind(2,before_id); query.bind(3,from_ms); query.bind(4,to_ms);
    query.bind(5,target); query.bind(6,event_type); query.bind(7,source_filter);
    query.bind(8,static_cast<std::int64_t>(limit+1));
    json rows=json::array(); std::int64_t next_ms=0,next_id=0;
    while(query.row()){
        if(rows.size()==static_cast<std::size_t>(limit)){next_ms=rows.back()["created_ms"].get<std::int64_t>();next_id=rows.back()["event_id"].get<std::int64_t>();break;}
        rows.push_back({{"source",query.text(0)},{"event_id",query.integer(1)},{"event_type",query.text(2)},
                        {"target_id",query.text(3)},{"detail",json::parse(query.text(4))},{"created_ms",query.integer(5)}});
    }
    json result={{"rows",std::move(rows)}}; if(next_id){result["next_before_ms"]=next_ms;result["next_before_id"]=next_id;} return result;
}

json AdminDatabase::quarantine(std::int64_t before_ms, const std::string& before_id, int limit) {
    std::lock_guard lock(gate_);
    if(before_ms<=0) before_ms=(std::numeric_limits<std::int64_t>::max)();
    const auto cursor=before_id.empty()?std::string(128,'~'):before_id;
    Statement query(pool_.get(),R"SQL(
SELECT q.quarantine_id,q.storage_id,c.display_name,q.root_item_id,q.item_id,q.parent_item_id,q.reason,q.created_ms,q.restored_ms,
       q.restored_change_id,q.tree_json->>'class_name'
FROM admin_quarantine q JOIN storage_containers c ON c.storage_id=q.storage_id
WHERE (q.created_ms,q.quarantine_id)<(?1,?2)
ORDER BY q.created_ms DESC,q.quarantine_id DESC LIMIT ?3
)SQL");
    query.bind(1,before_ms);query.bind(2,cursor);query.bind(3,static_cast<std::int64_t>(limit+1));
    json rows=json::array();std::int64_t next_ms=0;std::string next_id;
    while(query.row()){
        if(rows.size()==static_cast<std::size_t>(limit)){next_ms=rows.back()["created_ms"].get<std::int64_t>();next_id=rows.back()["quarantine_id"].get<std::string>();break;}
        json row={{"quarantine_id",query.text(0)},{"storage_id",query.text(1)},{"container",query.text(2)},{"root_item_id",query.text(3)},
                  {"item_id",query.text(4)},{"reason",query.text(6)},{"created_ms",query.integer(7)},{"class_name",query.text(10)}};
        if (!query.is_null(5)) row["parent_item_id"] = query.text(5);
        if (!query.is_null(8)) row["restored_ms"] = query.integer(8);
        if (!query.is_null(9)) row["restored_change_id"] = query.text(9);
        rows.push_back(std::move(row));
    }
    json result={{"rows",std::move(rows)}};if(!next_id.empty()){result["next_before_ms"]=next_ms;result["next_before_id"]=next_id;}return result;
}

json AdminDatabase::snapshots(const std::string& storage_id, std::int64_t before_ms,
                              const std::string& before_id, int limit) {
    std::lock_guard lock(gate_);
    if(before_ms<=0) before_ms=(std::numeric_limits<std::int64_t>::max)();
    const auto cursor=before_id.empty()?std::string(128,'~'):before_id;
    Statement query(pool_.get(),R"SQL(
SELECT snapshot_id,storage_id,revision,root_count,node_count,reason,windows_identity,created_ms
FROM admin_storage_snapshots
WHERE (?1='' OR storage_id=?1) AND (created_ms,snapshot_id)<(?2,?3)
ORDER BY created_ms DESC,snapshot_id DESC LIMIT ?4
)SQL");
    query.bind(1,storage_id);query.bind(2,before_ms);query.bind(3,cursor);query.bind(4,static_cast<std::int64_t>(limit+1));
    json rows=json::array();std::int64_t next_ms=0;std::string next_id;
    while(query.row()){
        if(rows.size()==static_cast<std::size_t>(limit)){next_ms=rows.back()["created_ms"].get<std::int64_t>();next_id=rows.back()["snapshot_id"].get<std::string>();break;}
        rows.push_back({{"snapshot_id",query.text(0)},{"storage_id",query.text(1)},{"revision",query.integer(2)},
                        {"root_count",query.integer(3)},{"node_count",query.integer(4)},{"reason",query.text(5)},
                        {"windows_identity",query.text(6)},{"created_ms",query.integer(7)}});
    }
    json result={{"rows",std::move(rows)}};if(!next_id.empty()){result["next_before_ms"]=next_ms;result["next_before_id"]=next_id;}return result;
}

json AdminDatabase::snapshot_compare(const std::string& snapshot_id, const std::string& after_root, int limit) {
    std::lock_guard lock(gate_);
    Statement meta(pool_.get(), R"SQL(
SELECT s.storage_id,s.revision,s.root_count,s.node_count,s.created_ms,c.revision,c.display_name
FROM admin_storage_snapshots s
JOIN storage_containers c ON c.storage_id=s.storage_id
WHERE s.snapshot_id=?1
)SQL");
    meta.bind(1, snapshot_id);
    if (!meta.row()) throw clippy::ApiError(404, "snapshot_not_found", "The admin snapshot does not exist.");
    const auto storage_id = meta.text(0);

    Statement counts(pool_.get(), R"SQL(
WITH snapshot_rows AS (
  SELECT root_item_id,tree_json FROM admin_snapshot_roots WHERE snapshot_id=?1
), current_rows AS (
  SELECT root_item_id,tree_json FROM cargo_roots WHERE storage_id=?2
), diff AS (
  SELECT COALESCE(s.root_item_id,c.root_item_id) AS root_item_id,
         CASE WHEN s.root_item_id IS NULL THEN 'added'
              WHEN c.root_item_id IS NULL THEN 'removed'
              WHEN s.tree_json IS DISTINCT FROM c.tree_json THEN 'changed'
              ELSE 'same' END AS kind
  FROM snapshot_rows s FULL OUTER JOIN current_rows c USING(root_item_id)
)
SELECT count(*) FILTER (WHERE kind='added'),count(*) FILTER (WHERE kind='removed'),
       count(*) FILTER (WHERE kind='changed') FROM diff
)SQL");
    counts.bind(1, snapshot_id); counts.bind(2, storage_id); counts.row();

    Statement rows(pool_.get(), R"SQL(
WITH snapshot_rows AS (
  SELECT root_item_id,tree_json FROM admin_snapshot_roots WHERE snapshot_id=?1
), current_rows AS (
  SELECT root_item_id,tree_json FROM cargo_roots WHERE storage_id=?2
)
SELECT COALESCE(s.root_item_id,c.root_item_id) AS root_item_id,
       CASE WHEN s.root_item_id IS NULL THEN 'added'
            WHEN c.root_item_id IS NULL THEN 'removed'
            ELSE 'changed' END AS kind,
       COALESCE(s.tree_json->>'class_name',c.tree_json->>'class_name','') AS class_name
FROM snapshot_rows s FULL OUTER JOIN current_rows c USING(root_item_id)
WHERE COALESCE(s.root_item_id,c.root_item_id)>?3
  AND (s.root_item_id IS NULL OR c.root_item_id IS NULL OR s.tree_json IS DISTINCT FROM c.tree_json)
ORDER BY root_item_id LIMIT ?4
)SQL");
    rows.bind(1, snapshot_id); rows.bind(2, storage_id); rows.bind(3, after_root);
    rows.bind(4, static_cast<std::int64_t>(limit + 1));
    json differences = json::array();
    std::string next_root;
    while (rows.row()) {
        if (differences.size() == static_cast<std::size_t>(limit)) {
            next_root = differences.back()["root_item_id"].get<std::string>();
            break;
        }
        differences.push_back({{"root_item_id",rows.text(0)},{"kind",rows.text(1)},{"class_name",rows.text(2)}});
    }
    json result = {{"snapshot_id",snapshot_id},{"storage_id",storage_id},{"container",meta.text(6)},
                   {"snapshot_revision",meta.integer(1)},{"current_revision",meta.integer(5)},
                   {"snapshot_root_count",meta.integer(2)},{"snapshot_node_count",meta.integer(3)},
                   {"snapshot_created_ms",meta.integer(4)},
                   {"added_roots",counts.integer(0)},{"removed_roots",counts.integer(1)},
                   {"changed_roots",counts.integer(2)},{"differences",std::move(differences)}};
    if (!next_root.empty()) result["next_after_root"] = next_root;
    return result;
}


} // namespace clippy_admin
