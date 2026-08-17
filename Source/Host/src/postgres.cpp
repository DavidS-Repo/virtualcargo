#include "postgres.hpp"
#include "config.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace clippy {
namespace {

struct pg_conn;
struct pg_result;
using PGconn = pg_conn;
using PGresult = pg_result;
using Oid = unsigned int;

enum ConnStatusType { CONNECTION_OK = 0, CONNECTION_BAD = 1 };
enum ExecStatusType {
    PGRES_EMPTY_QUERY = 0,
    PGRES_COMMAND_OK,
    PGRES_TUPLES_OK,
    PGRES_COPY_OUT,
    PGRES_COPY_IN,
    PGRES_BAD_RESPONSE,
    PGRES_NONFATAL_ERROR,
    PGRES_FATAL_ERROR,
    PGRES_COPY_BOTH,
    PGRES_SINGLE_TUPLE,
    PGRES_PIPELINE_SYNC,
    PGRES_PIPELINE_ABORTED,
    PGRES_TUPLES_CHUNK
};

constexpr int PG_DIAG_SQLSTATE = 'C';

class LibPq {
public:
    using PQconnectdbParams_t = PGconn* (*)(const char* const*, const char* const*, int);
    using PQstatus_t = ConnStatusType (*)(const PGconn*);
    using PQerrorMessage_t = char* (*)(const PGconn*);
    using PQfinish_t = void (*)(PGconn*);
    using PQexec_t = PGresult* (*)(PGconn*, const char*);
    using PQprepare_t = PGresult* (*)(PGconn*, const char*, const char*, int, const Oid*);
    using PQexecPrepared_t = PGresult* (*)(PGconn*, const char*, int, const char* const*, const int*, const int*, int);
    using PQresultStatus_t = ExecStatusType (*)(const PGresult*);
    using PQresultErrorMessage_t = char* (*)(const PGresult*);
    using PQresultErrorField_t = char* (*)(const PGresult*, int);
    using PQclear_t = void (*)(PGresult*);
    using PQntuples_t = int (*)(const PGresult*);
    using PQnfields_t = int (*)(const PGresult*);
    using PQgetvalue_t = char* (*)(const PGresult*, int, int);
    using PQgetisnull_t = int (*)(const PGresult*, int, int);
    using PQcmdTuples_t = char* (*)(PGresult*);
    using PQserverVersion_t = int (*)(const PGconn*);
    using PQlibVersion_t = int (*)();

    PQconnectdbParams_t PQconnectdbParams = nullptr;
    PQstatus_t PQstatus = nullptr;
    PQerrorMessage_t PQerrorMessage = nullptr;
    PQfinish_t PQfinish = nullptr;
    PQexec_t PQexec = nullptr;
    PQprepare_t PQprepare = nullptr;
    PQexecPrepared_t PQexecPrepared = nullptr;
    PQresultStatus_t PQresultStatus = nullptr;
    PQresultErrorMessage_t PQresultErrorMessage = nullptr;
    PQresultErrorField_t PQresultErrorField = nullptr;
    PQclear_t PQclear = nullptr;
    PQntuples_t PQntuples = nullptr;
    PQnfields_t PQnfields = nullptr;
    PQgetvalue_t PQgetvalue = nullptr;
    PQgetisnull_t PQgetisnull = nullptr;
    PQcmdTuples_t PQcmdTuples = nullptr;
    PQserverVersion_t PQserverVersion = nullptr;
    PQlibVersion_t PQlibVersion = nullptr;

    static LibPq& instance(const std::string& configured_path = {}) {
        static LibPq library(configured_path);
        return library;
    }

private:
#ifdef _WIN32
    HMODULE module_ = nullptr;
#else
    void* module_ = nullptr;
#endif

    explicit LibPq(const std::string& configured_path) {
#ifdef _WIN32
        auto load = [&](const std::wstring& path) {
            module_ = LoadLibraryExW(path.c_str(), nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        };
        if (!configured_path.empty()) {
            const auto wide_size = MultiByteToWideChar(CP_UTF8, 0, configured_path.c_str(), -1, nullptr, 0);
            if (wide_size <= 0) throw std::runtime_error("Could not decode postgresLibraryPath as UTF-8.");
            std::wstring wide(static_cast<std::size_t>(wide_size), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, configured_path.c_str(), -1, wide.data(), wide_size);
            wide.resize(wide.size() - 1);
            load(wide);
        } else {
            load(L"libpq.dll");
        }
        if (!module_) {
            throw std::runtime_error("Could not load libpq.dll. Run the Clippy setup script so PostgreSQL is installed and postgresLibraryPath is configured.");
        }
        auto symbol = [&](const char* name) -> void* {
            auto value = reinterpret_cast<void*>(GetProcAddress(module_, name));
            if (!value) throw std::runtime_error(std::string("libpq is missing required function ") + name + ".");
            return value;
        };
#else
        module_ = dlopen(configured_path.empty() ? "libpq.so.5" : configured_path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!module_) throw std::runtime_error(std::string("Could not load libpq: ") + dlerror());
        auto symbol = [&](const char* name) -> void* {
            auto value = dlsym(module_, name);
            if (!value) throw std::runtime_error(std::string("libpq is missing required function ") + name + ".");
            return value;
        };
#endif
#define LOAD_PQ(name) name = reinterpret_cast<name##_t>(symbol(#name))
        LOAD_PQ(PQconnectdbParams);
        LOAD_PQ(PQstatus);
        LOAD_PQ(PQerrorMessage);
        LOAD_PQ(PQfinish);
        LOAD_PQ(PQexec);
        LOAD_PQ(PQprepare);
        LOAD_PQ(PQexecPrepared);
        LOAD_PQ(PQresultStatus);
        LOAD_PQ(PQresultErrorMessage);
        LOAD_PQ(PQresultErrorField);
        LOAD_PQ(PQclear);
        LOAD_PQ(PQntuples);
        LOAD_PQ(PQnfields);
        LOAD_PQ(PQgetvalue);
        LOAD_PQ(PQgetisnull);
        LOAD_PQ(PQcmdTuples);
        LOAD_PQ(PQserverVersion);
        LOAD_PQ(PQlibVersion);
#undef LOAD_PQ
    }
};

std::string trim_error(std::string value) {
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    return value;
}

std::string result_sqlstate(PGresult* result) {
    if (!result) return {};
    auto& pq = LibPq::instance();
    const char* value = pq.PQresultErrorField(result, PG_DIAG_SQLSTATE);
    return value ? value : "";
}

[[noreturn]] void result_failure(PGconn* connection, PGresult* result, const std::string& context) {
    auto& pq = LibPq::instance();
    std::string detail;
    if (result) {
        const char* message = pq.PQresultErrorMessage(result);
        if (message) detail = message;
    }
    if (detail.empty() && connection) {
        const char* message = pq.PQerrorMessage(connection);
        if (message) detail = message;
    }
    const auto state = result_sqlstate(result);
    throw PgError(context, trim_error(detail.empty() ? "unknown PostgreSQL error" : detail), state);
}

std::string convert_placeholders(std::string_view sql) {
    std::string output;
    output.reserve(sql.size());
    for (std::size_t i = 0; i < sql.size(); ++i) {
        if (sql[i] == '?' && i + 1 < sql.size() && sql[i + 1] >= '0' && sql[i + 1] <= '9') {
            output.push_back('$');
            ++i;
            while (i < sql.size() && sql[i] >= '0' && sql[i] <= '9') {
                output.push_back(sql[i]);
                ++i;
            }
            --i;
        } else {
            output.push_back(sql[i]);
        }
    }
    return output;
}

std::string version_text(int version) {
    if (version <= 0) return "unknown";
    const int major = version / 10000;
    const int minor = (version / 100) % 100;
    const int patch = version % 100;
    std::ostringstream out;
    out << major;
    if (major < 10) out << '.' << minor;
    if (patch != 0) out << '.' << patch;
    return out.str();
}

} // namespace

class PgConnection {
public:
    explicit PgConnection(const HostConfig& config) {
        auto& pq = LibPq::instance(config.postgres_library_path);
        const std::string port = std::to_string(config.postgres_port);
        const std::string timeout = std::to_string(config.postgres_connect_timeout_seconds);
        const char* keywords[] = {
            "host", "port", "dbname", "user", "password", "connect_timeout",
            "application_name", "sslmode", nullptr
        };
        const char* values[] = {
            config.postgres_host.c_str(), port.c_str(), config.postgres_database.c_str(),
            config.postgres_user.c_str(), config.postgres_password.c_str(), timeout.c_str(),
            "ClippyStorageHost", "disable", nullptr
        };
        handle_ = pq.PQconnectdbParams(keywords, values, 0);
        if (!handle_ || pq.PQstatus(handle_) != CONNECTION_OK) {
            std::string detail = handle_ && pq.PQerrorMessage(handle_) ? pq.PQerrorMessage(handle_) : "no PostgreSQL connection handle";
            if (handle_) pq.PQfinish(handle_);
            handle_ = nullptr;
            throw std::runtime_error("Could not connect to PostgreSQL: " + trim_error(detail));
        }
        server_version_ = pq.PQserverVersion(handle_);
        if (server_version_ < 140000) {
            throw std::runtime_error("PostgreSQL 14 or newer is required. Server version is " + version_text(server_version_) + ".");
        }

        const auto safe_timeout = [](int value) { return std::to_string((std::max)(value, 1)) + "ms"; };
        exec("SET timezone='UTC'; SET synchronous_commit=on; SET client_min_messages=warning; SET search_path=clippy,pg_catalog; "
             "SET statement_timeout='" + safe_timeout(config.postgres_statement_timeout_ms) + "'; "
             "SET lock_timeout='" + safe_timeout(config.postgres_lock_timeout_ms) + "'; "
             "SET idle_in_transaction_session_timeout='" + safe_timeout(config.postgres_idle_transaction_timeout_ms) + "';");
    }

    ~PgConnection() {
        if (handle_) LibPq::instance().PQfinish(handle_);
    }

    PGconn* handle() const noexcept { return handle_; }
    int server_version() const noexcept { return server_version_; }

    PGresult* exec(const std::string& sql) {
        auto& pq = LibPq::instance();
        PGresult* result = pq.PQexec(handle_, sql.c_str());
        if (!result) result_failure(handle_, nullptr, "Could not execute PostgreSQL statement");
        const auto status = pq.PQresultStatus(result);
        if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK && status != PGRES_TUPLES_CHUNK) {
            try { result_failure(handle_, result, "PostgreSQL statement failed"); }
            catch (...) { pq.PQclear(result); throw; }
        }
        return result;
    }

    std::string prepare(const std::string& sql) {
        auto found = prepared_.find(sql);
        if (found != prepared_.end()) return found->second;
        const std::string name = "cvc_" + std::to_string(++prepared_counter_);
        auto& pq = LibPq::instance();
        PGresult* result = pq.PQprepare(handle_, name.c_str(), sql.c_str(), 0, nullptr);
        if (!result) result_failure(handle_, nullptr, "Could not prepare PostgreSQL statement");
        const auto status = pq.PQresultStatus(result);
        if (status != PGRES_COMMAND_OK) {
            try { result_failure(handle_, result, "Could not prepare PostgreSQL statement"); }
            catch (...) { pq.PQclear(result); throw; }
        }
        pq.PQclear(result);
        prepared_.emplace(sql, name);
        return name;
    }

    PGresult* exec_prepared(const std::string& sql, const std::vector<std::optional<std::string>>& parameters) {
        const auto name = prepare(sql);
        std::vector<const char*> values(parameters.size(), nullptr);
        for (std::size_t i = 0; i < parameters.size(); ++i) {
            if (parameters[i]) values[i] = parameters[i]->c_str();
        }
        auto& pq = LibPq::instance();
        PGresult* result = pq.PQexecPrepared(handle_, name.c_str(), static_cast<int>(values.size()),
                                             values.data(), nullptr, nullptr, 0);
        if (!result) result_failure(handle_, nullptr, "Could not execute prepared PostgreSQL statement");
        const auto status = pq.PQresultStatus(result);
        if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK && status != PGRES_TUPLES_CHUNK) {
            try { result_failure(handle_, result, "PostgreSQL prepared statement failed"); }
            catch (...) { pq.PQclear(result); throw; }
        }
        return result;
    }

private:
    PGconn* handle_ = nullptr;
    int server_version_ = 0;
    std::unordered_map<std::string, std::string> prepared_;
    std::uint64_t prepared_counter_ = 0;
};

namespace {
struct ThreadLease {
    PgPool* pool = nullptr;
    PgConnection* connection = nullptr;
    std::size_t depth = 0;
};
thread_local ThreadLease thread_lease;
}

PgError::PgError(std::string context, std::string detail, std::string sqlstate)
    : std::runtime_error(std::move(context) + ": " + std::move(detail)), sqlstate_(std::move(sqlstate)) {}

void ConnectionGate::lock() {
    if (!pool_) throw std::runtime_error("PostgreSQL connection gate is not attached.");
    pool_->acquire_for_thread();
}

void ConnectionGate::unlock() noexcept {
    if (pool_) pool_->release_for_thread();
}

PgPool::PgPool(const HostConfig& config) {
    LibPq::instance(config.postgres_library_path);
    connections_.reserve(static_cast<std::size_t>(config.postgres_pool_size));
    for (int i = 0; i < config.postgres_pool_size; ++i) {
        connections_.push_back(std::make_unique<PgConnection>(config));
        available_.push_back(connections_.back().get());
    }
    if (connections_.empty()) throw std::runtime_error("PostgreSQL connection pool cannot be empty.");
    server_version_ = connections_.front()->server_version();
}

PgPool::~PgPool() = default;

void PgPool::acquire_for_thread() {
    if (thread_lease.depth != 0) {
        if (thread_lease.pool != this) throw std::runtime_error("Nested PostgreSQL pools are not supported on one worker thread.");
        ++thread_lease.depth;
        return;
    }
    std::unique_lock lock(mutex_);
    available_cv_.wait(lock, [&] { return !available_.empty(); });
    auto* connection = available_.back();
    available_.pop_back();
    thread_lease = {this, connection, 1};
}

void PgPool::release_for_thread() noexcept {
    if (thread_lease.pool != this || thread_lease.depth == 0) return;
    if (--thread_lease.depth != 0) return;
    auto* connection = thread_lease.connection;
    thread_lease = {};
    {
        std::lock_guard lock(mutex_);
        available_.push_back(connection);
    }
    available_cv_.notify_one();
}

PgConnection* PgPool::current() {
    if (thread_lease.pool != this || !thread_lease.connection || thread_lease.depth == 0) {
        throw std::runtime_error("PostgreSQL statement attempted without a checked-out pool connection.");
    }
    return thread_lease.connection;
}

std::string PgPool::server_version_text() const { return version_text(server_version_); }

Statement::Statement(PgPool* database, const char* sql)
    : Statement(database, std::string(sql ? sql : "")) {}

Statement::Statement(PgPool* database, std::string sql)
    : connection_(database ? database->current() : nullptr), sql_(convert_placeholders(sql)) {
    if (!connection_) throw std::runtime_error("PostgreSQL statement has no connection.");
}

Statement::~Statement() {
    if (result_) LibPq::instance().PQclear(static_cast<PGresult*>(result_));
}

void Statement::ensure_index(int index) {
    if (index <= 0 || index > 256) throw std::runtime_error("PostgreSQL parameter index is out of range.");
    if (parameters_.size() < static_cast<std::size_t>(index)) parameters_.resize(static_cast<std::size_t>(index));
}

void Statement::bind(int index, const std::string& value) { ensure_index(index); parameters_[index - 1] = value; }
void Statement::bind(int index, std::string_view value) { ensure_index(index); parameters_[index - 1] = std::string(value); }
void Statement::bind(int index, std::int64_t value) { ensure_index(index); parameters_[index - 1] = std::to_string(value); }
void Statement::bind(int index, double value) {
    if (!std::isfinite(value)) throw std::runtime_error("Cannot bind a non-finite PostgreSQL number.");
    ensure_index(index);
    std::ostringstream out;
    out << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    parameters_[index - 1] = out.str();
}
void Statement::bind_null(int index) { ensure_index(index); parameters_[index - 1] = std::nullopt; }

void Statement::ensure_executed() {
    if (result_) return;
    result_ = connection_->exec_prepared(sql_, parameters_);
    next_row_ = 0;
    current_row_ = -1;
}

bool Statement::row() {
    ensure_executed();
    auto* result = static_cast<PGresult*>(result_);
    const int count = LibPq::instance().PQntuples(result);
    if (next_row_ >= count) {
        current_row_ = -1;
        return false;
    }
    current_row_ = next_row_++;
    return true;
}

void Statement::done() {
    ensure_executed();
    const auto status = LibPq::instance().PQresultStatus(static_cast<PGresult*>(result_));
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK && status != PGRES_TUPLES_CHUNK) {
        result_failure(connection_->handle(), static_cast<PGresult*>(result_), "PostgreSQL command did not complete");
    }
}

void Statement::reset() {
    if (result_) {
        LibPq::instance().PQclear(static_cast<PGresult*>(result_));
        result_ = nullptr;
    }
    parameters_.clear();
    next_row_ = 0;
    current_row_ = -1;
}

std::string Statement::text(int column) const {
    if (!result_ || current_row_ < 0) throw std::runtime_error("PostgreSQL row is not positioned.");
    auto& pq = LibPq::instance();
    auto* result = static_cast<PGresult*>(result_);
    if (column < 0 || column >= pq.PQnfields(result)) throw std::runtime_error("PostgreSQL column index is out of range.");
    if (pq.PQgetisnull(result, current_row_, column)) return {};
    const char* value = pq.PQgetvalue(result, current_row_, column);
    return value ? value : "";
}

std::int64_t Statement::integer(int column) const {
    const auto value = text(column);
    std::int64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        throw std::runtime_error("PostgreSQL integer result could not be parsed.");
    }
    return result;
}

double Statement::number(int column) const {
    const auto value = text(column);
    char* end = nullptr;
    const double result = std::strtod(value.c_str(), &end);
    if (!end || end != value.c_str() + value.size() || !std::isfinite(result)) {
        throw std::runtime_error("PostgreSQL numeric result could not be parsed.");
    }
    return result;
}

bool Statement::is_null(int column) const {
    if (!result_ || current_row_ < 0) throw std::runtime_error("PostgreSQL row is not positioned.");
    auto& pq = LibPq::instance();
    auto* result = static_cast<PGresult*>(result_);
    if (column < 0 || column >= pq.PQnfields(result)) throw std::runtime_error("PostgreSQL column index is out of range.");
    return pq.PQgetisnull(result, current_row_, column) != 0;
}

std::int64_t Statement::affected_rows() const {
    if (!result_) return 0;
    const char* value = LibPq::instance().PQcmdTuples(static_cast<PGresult*>(result_));
    if (!value || !*value) return 0;
    std::int64_t result = 0;
    const auto length = std::strlen(value);
    const auto parsed = std::from_chars(value, value + length, result);
    return parsed.ec == std::errc{} ? result : 0;
}

Transaction::Transaction(PgPool* database, const char* begin) : database_(database) {
    execute(database_, begin && *begin ? begin : "BEGIN");
}

Transaction::~Transaction() {
    if (!finished_ && database_) {
        try { execute(database_, "ROLLBACK"); } catch (...) {}
    }
}

void Transaction::commit() {
    execute(database_, "COMMIT");
    finished_ = true;
}

void execute(PgPool* database, const char* sql) { execute(database, std::string(sql ? sql : "")); }

void execute(PgPool* database, const std::string& sql) {
    if (!database) throw std::runtime_error("PostgreSQL execute has no pool.");
    auto* connection = database->current();
    PGresult* result = connection->exec(sql);
    LibPq::instance().PQclear(result);
}

std::string postgres_library_version() {
    const int version = LibPq::instance().PQlibVersion();
    return version_text(version);
}

} // namespace clippy
