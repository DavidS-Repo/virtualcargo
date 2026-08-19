#pragma once

#include "config.hpp"
#include "json.hpp"
#include "postgres.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace clippy {

class StorageDatabase {
public:
    explicit StorageDatabase(HostConfig config);
    ~StorageDatabase();

    StorageDatabase(const StorageDatabase&) = delete;
    StorageDatabase& operator=(const StorageDatabase&) = delete;

    nlohmann::json resolve_container(const nlohmann::json& request);
    nlohmann::json observe_container(const nlohmann::json& request);
    nlohmann::json snapshot(const nlohmann::json& request);
    nlohmann::json item_tree(const nlohmann::json& request);
    nlohmann::json prepare_deposit(const nlohmann::json& request);
    nlohmann::json prepare_withdrawal(const nlohmann::json& request);
    nlohmann::json mark_quarantined(const nlohmann::json& request);
    nlohmann::json commit_deposit(const nlohmann::json& request);
    nlohmann::json commit_withdrawal(const nlohmann::json& request);
    nlohmann::json abort_operation(const nlohmann::json& request);
    nlohmann::json acknowledge_operation_cleanup(const nlohmann::json& request);
    nlohmann::json incomplete_operations(const nlohmann::json& request);
    nlohmann::json open_session(const nlohmann::json& request);
    nlohmann::json mark_session_materialized(const nlohmann::json& request);
    nlohmann::json commit_session(const nlohmann::json& request);
    nlohmann::json acknowledge_session_cleanup(const nlohmann::json& request);
    nlohmann::json abort_session(const nlohmann::json& request);
    nlohmann::json incomplete_sessions(const nlohmann::json& request);
    nlohmann::json prepare_migration(const nlohmann::json& request);
    nlohmann::json commit_migration(const nlohmann::json& request);
    nlohmann::json acknowledge_migration_cleanup(const nlohmann::json& request);
    nlohmann::json incomplete_migrations(const nlohmann::json& request);
    nlohmann::json observe_migration(const nlohmann::json& request);
    nlohmann::json health();
    nlohmann::json item_index_status();
    nlohmann::json rebuild_item_index_batch(const nlohmann::json& request);
    nlohmann::json quick_check();
    nlohmann::json backup(const nlohmann::json& request);
    nlohmann::json verify_backup(const nlohmann::json& request);
    nlohmann::json metrics(const nlohmann::json& request);
    nlohmann::json player_snapshot(const nlohmann::json& request);
    nlohmann::json poll_player_commands(const nlohmann::json& request);
    nlohmann::json complete_player_command(const nlohmann::json& request);
    nlohmann::json migrate_legacy_sqlite(const std::filesystem::path& legacy_database,
                                         const std::string& expected_fingerprint = {});

    void checkpoint();
    void optimize();

private:
    struct ReadSlot {
        explicit ReadSlot(PgPool& pool) : connection(&pool), mutex(pool) {}
        PgPool* connection = nullptr;
        ConnectionGate mutex;
    };

    HostConfig config_;
    std::unique_ptr<PgPool> pool_;
    PgPool* writer_ = nullptr;
    ConnectionGate writer_gate_;
    std::mutex backup_mutex_;
    std::vector<std::unique_ptr<ReadSlot>> readers_;
    std::atomic<std::size_t> next_reader_ = 0;

    ReadSlot& reader();
    void initialize_schema();
    void prune_terminal_history();
};

} // namespace clippy
