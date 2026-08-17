#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace clippy {

class ApiError final : public std::runtime_error {
public:
    ApiError(int http_status, std::string code, std::string message, bool retryable = false);

    [[nodiscard]] int http_status() const noexcept { return http_status_; }
    [[nodiscard]] const std::string& code() const noexcept { return code_; }
    [[nodiscard]] bool retryable() const noexcept { return retryable_; }

private:
    int http_status_;
    std::string code_;
    bool retryable_;
};

[[nodiscard]] std::int64_t now_unix_ms();
[[nodiscard]] std::string random_hex(std::size_t byte_count);
[[nodiscard]] bool constant_time_equal(std::string_view left, std::string_view right) noexcept;
[[nodiscard]] std::string fingerprint(std::string_view text);
[[nodiscard]] std::string fingerprint_file(const std::filesystem::path& path);
[[nodiscard]] std::string read_text_file(const std::filesystem::path& path);
void write_text_file_atomic(const std::filesystem::path& path, std::string_view contents);

} // namespace clippy

