#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace clippy {

struct HostConfig;

class PgError : public std::runtime_error {
public:
    PgError(std::string context, std::string detail, std::string sqlstate = {});
    [[nodiscard]] const std::string& sqlstate() const noexcept { return sqlstate_; }
private:
    std::string sqlstate_;
};

class PgPool;
class PgConnection;

class ConnectionGate {
public:
    ConnectionGate() = default;
    explicit ConnectionGate(PgPool& pool) : pool_(&pool) {}
    void attach(PgPool& pool) { pool_ = &pool; }
    void lock();
    void unlock() noexcept;
private:
    PgPool* pool_ = nullptr;
};

class Statement {
public:
    Statement(PgPool* database, const char* sql);
    Statement(PgPool* database, std::string sql);
    ~Statement();
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void bind(int index, const std::string& value);
    void bind(int index, std::string_view value);
    void bind(int index, std::int64_t value);
    void bind(int index, int value) { bind(index, static_cast<std::int64_t>(value)); }
    void bind(int index, double value);
    void bind_null(int index);

    bool row();
    void done();
    void reset();
    [[nodiscard]] std::string text(int column) const;
    [[nodiscard]] std::int64_t integer(int column) const;
    [[nodiscard]] double number(int column) const;
    [[nodiscard]] bool is_null(int column) const;
    [[nodiscard]] std::int64_t affected_rows() const;

private:
    void ensure_executed();
    void ensure_index(int index);

    PgConnection* connection_ = nullptr;
    std::string sql_;
    std::vector<std::optional<std::string>> parameters_;
    void* result_ = nullptr;
    int next_row_ = 0;
    int current_row_ = -1;
};

class Transaction {
public:
    explicit Transaction(PgPool* database, const char* begin = "BEGIN");
    ~Transaction();
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    void commit();
private:
    PgPool* database_ = nullptr;
    bool finished_ = false;
};

class PgPool {
public:
    explicit PgPool(const HostConfig& config);
    ~PgPool();
    PgPool(const PgPool&) = delete;
    PgPool& operator=(const PgPool&) = delete;

    void acquire_for_thread();
    void release_for_thread() noexcept;
    [[nodiscard]] PgConnection* current();
    [[nodiscard]] int server_version() const noexcept { return server_version_; }
    [[nodiscard]] std::string server_version_text() const;
    [[nodiscard]] std::size_t size() const noexcept { return connections_.size(); }

private:
    friend class ConnectionGate;
    std::vector<std::unique_ptr<PgConnection>> connections_;
    std::vector<PgConnection*> available_;
    mutable std::mutex mutex_;
    std::condition_variable available_cv_;
    int server_version_ = 0;
};

void execute(PgPool* database, const char* sql);
void execute(PgPool* database, const std::string& sql);
[[nodiscard]] std::string postgres_library_version();

} // namespace clippy
