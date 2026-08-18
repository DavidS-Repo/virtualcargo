#pragma once

#include "admin_config.hpp"
#include "json.hpp"
#include "postgres.hpp"

#include <memory>
#include <string>

namespace clippy_admin {

class AdminDatabase {
public:
    explicit AdminDatabase(const AdminConfig& config);

    nlohmann::json health();
    nlohmann::json overview();
    nlohmann::json containers(const std::string& query, const std::string& after, int limit);
    nlohmann::json container(const std::string& storage_id);
    nlohmann::json roots(const std::string& storage_id, const std::string& after, int limit);
    nlohmann::json tree(const std::string& storage_id, const std::string& root_item_id);
    nlohmann::json search_items(const std::string& query,
                                const std::string& after_class,
                                const std::string& after_storage,
                                const std::string& after_root,
                                const std::string& after_item,
                                double min_quantity,
                                double max_quantity,
                                double min_health,
                                double max_health,
                                int limit);
    nlohmann::json sessions(std::int64_t before_ms, const std::string& before_id, int limit);
    nlohmann::json recovery();
    nlohmann::json database_info();

private:
    std::unique_ptr<clippy::PgPool> pool_;
    clippy::ConnectionGate gate_;
};

} // namespace clippy_admin
