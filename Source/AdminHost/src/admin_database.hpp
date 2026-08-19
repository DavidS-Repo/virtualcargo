#pragma once

#include "admin_config.hpp"
#include "json.hpp"
#include "postgres.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace clippy_admin {

class AdminDatabase {
public:
    explicit AdminDatabase(const AdminConfig& config);

    bool editing_enabled() const noexcept { return editing_enabled_; }

    nlohmann::json health();
    nlohmann::json overview();
    nlohmann::json containers(const std::string& query, const std::string& after, const std::string& contains_class,
                              const std::string& status, std::int64_t min_nodes, int stale_days, int limit);
    nlohmann::json container(const std::string& storage_id, const std::string& admin_session_id = "");
    nlohmann::json locks(std::int64_t before_expiry_ms, const std::string& before_storage_id, int limit);
    nlohmann::json roots(const std::string& storage_id, const std::string& after, int limit);
    nlohmann::json tree(const std::string& storage_id, const std::string& root_item_id);
    nlohmann::json export_container(const std::string& storage_id, const std::filesystem::path& export_directory);
    nlohmann::json search_items(const std::string& query,
                                const std::string& after_class,
                                const std::string& after_storage,
                                const std::string& after_root,
                                const std::string& after_item,
                                double min_quantity,
                                double max_quantity,
                                double min_health,
                                double max_health,
                                const std::string& adapter_id,
                                const std::string& location_type,
                                int limit);
    nlohmann::json sessions(std::int64_t before_ms, const std::string& before_id, int limit);
    nlohmann::json recovery();
    nlohmann::json database_info();
    nlohmann::json report(const std::string& kind, int limit);
    nlohmann::json table_preview(const std::string& table, const std::string& after_ctid, int limit);
    nlohmann::json changes(const std::string& storage_id, std::int64_t before_ms,
                           const std::string& before_id, int limit);
    nlohmann::json change_detail(const std::string& change_id);
    nlohmann::json item_history(const std::string& item_id, std::int64_t before_ms,
                                const std::string& before_id, int limit);
    nlohmann::json audit(std::int64_t before_ms, std::int64_t before_id, std::int64_t from_ms, std::int64_t to_ms, int limit,
                         const std::string& admin, const std::string& action,
                         const std::string& target_type, const std::string& target_id,
                         const std::string& result);
    nlohmann::json activity(std::int64_t before_ms, std::int64_t before_id, std::int64_t from_ms, std::int64_t to_ms, int limit,
                            const std::string& target, const std::string& event_type,
                            const std::string& source);
    nlohmann::json quarantine(std::int64_t before_ms, const std::string& before_id, int limit);
    nlohmann::json snapshots(const std::string& storage_id, std::int64_t before_ms,
                             const std::string& before_id, int limit);
    nlohmann::json snapshot_compare(const std::string& snapshot_id, const std::string& after_root, int limit);
    nlohmann::json players(const std::string& query, std::int64_t before_ms,
                           const std::string& before_id, int limit, std::int64_t online_window_ms);
    nlohmann::json player_detail(const std::string& player_id, std::int64_t online_window_ms);
    nlohmann::json player_snapshot_tree(const std::string& player_id, const std::string& snapshot_id);
    nlohmann::json search_player_items(const std::string& query, const std::string& player_id,
                                       const std::string& after_class, const std::string& after_player,
                                       const std::string& after_item, int limit);
    nlohmann::json player_commands(const std::string& player_id, std::int64_t before_ms,
                                   const std::string& before_id, int limit);
    nlohmann::json player_quarantine(const std::string& player_id, std::int64_t before_ms,
                                     const std::string& before_id, int limit);

    nlohmann::json edit_item(const std::string& storage_id, const std::string& root_item_id,
                             const std::string& item_id, std::int64_t expected_revision,
                             const nlohmann::json& patch, const std::string& reason,
                             const std::string& admin_session_id, const std::string& windows_identity,
                             const std::string& request_id);
    nlohmann::json remove_item(const std::string& storage_id, const std::string& root_item_id,
                               const std::string& item_id, std::int64_t expected_revision,
                               bool quarantine_item, const std::string& reason,
                               const std::string& admin_session_id, const std::string& windows_identity,
                               const std::string& request_id);
    nlohmann::json copy_or_move_item(const std::string& source_storage_id,
                                     const std::string& source_root_item_id,
                                     const std::string& item_id,
                                     std::int64_t source_expected_revision,
                                     const std::string& target_storage_id,
                                     std::int64_t target_expected_revision,
                                     bool copy,
                                     const std::string& reason,
                                     const std::string& admin_session_id,
                                     const std::string& windows_identity,
                                     const std::string& request_id);
    nlohmann::json create_snapshot(const std::string& storage_id, std::int64_t expected_revision,
                                   const std::string& reason, const std::string& admin_session_id,
                                   const std::string& windows_identity, const std::string& request_id);
    nlohmann::json restore_quarantine(const std::string& quarantine_id, std::int64_t expected_revision,
                                      const std::string& reason, const std::string& admin_session_id,
                                      const std::string& windows_identity, const std::string& request_id);
    nlohmann::json undo_change(const std::string& change_id, const std::string& reason,
                               const std::string& admin_session_id, const std::string& windows_identity,
                               const std::string& request_id);
    nlohmann::json bulk_preview(const nlohmann::json& items, const std::string& admin_session_id = "");
    nlohmann::json bulk_transfer_preview(const nlohmann::json& items, const std::string& target_storage_id,
                                         std::int64_t target_expected_revision, const std::string& admin_session_id = "");
    nlohmann::json bulk_remove_roots(const nlohmann::json& items, bool quarantine_items,
                                     const std::string& reason, const std::string& admin_session_id,
                                     const std::string& windows_identity, const std::string& request_id);
    nlohmann::json bulk_transfer_roots(const nlohmann::json& items, const std::string& target_storage_id,
                                       std::int64_t target_expected_revision, bool copy,
                                       const std::string& reason, const std::string& admin_session_id,
                                       const std::string& windows_identity, const std::string& request_id);
    nlohmann::json acquire_manual_lock(const std::string& storage_id, std::int64_t expected_revision,
                                       const std::string& reason, const std::string& admin_session_id,
                                       const std::string& windows_identity, const std::string& request_id);
    nlohmann::json release_manual_lock(const std::string& storage_id, const std::string& reason,
                                       const std::string& admin_session_id, const std::string& windows_identity,
                                       const std::string& request_id);
    nlohmann::json enqueue_player_command(const std::string& player_id, const std::string& action,
                                          const nlohmann::json& payload, const std::string& reason,
                                          int expiry_seconds, const std::string& admin_session_id,
                                          const std::string& windows_identity, const std::string& request_id);

    void record_external_audit(const std::string& admin_session_id, const std::string& windows_identity,
                               const std::string& action, const std::string& target_type,
                               const std::string& target_id, const std::string& result,
                               const std::string& reason, const std::string& error,
                               const std::string& request_id, const nlohmann::json& detail = nlohmann::json::object());

private:
    std::unique_ptr<clippy::PgPool> pool_;
    clippy::ConnectionGate gate_;
    std::unique_ptr<clippy::PgPool> writer_pool_;
    std::unique_ptr<clippy::ConnectionGate> writer_gate_;
    bool editing_enabled_ = false;
    int maintenance_lock_seconds_ = 300;
};

} // namespace clippy_admin
