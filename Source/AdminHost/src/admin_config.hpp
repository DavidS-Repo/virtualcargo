#pragma once

#include "config.hpp"

#include <filesystem>
#include <string>

namespace clippy_admin {

struct AdminConfig {
    std::filesystem::path config_path;
    int port = 27817;
    int http_threads = 8;
    int max_queued_requests = 256;
    int idle_shutdown_minutes = 30;
    std::size_t max_request_bytes = 64 * 1024;
    std::string storage_host_address = "127.0.0.1";
    int storage_host_port = 27815;
    clippy::HostConfig postgres;
};

AdminConfig load_admin_config(const std::filesystem::path& path);

} // namespace clippy_admin
