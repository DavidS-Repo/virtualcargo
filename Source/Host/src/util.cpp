#include "util.hpp"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/evp.h>
#include <sys/random.h>
#endif

#include <array>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <system_error>
#include <vector>

namespace clippy {

ApiError::ApiError(int http_status, std::string code, std::string message, bool retryable)
    : std::runtime_error(std::move(message)),
      http_status_(http_status),
      code_(std::move(code)),
      retryable_(retryable) {}

std::int64_t now_unix_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string random_hex(std::size_t byte_count) {
    std::vector<unsigned char> bytes(byte_count);
#ifdef _WIN32
    const auto status = BCryptGenRandom(
        nullptr,
        bytes.data(),
        static_cast<ULONG>(bytes.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) throw std::runtime_error("Windows could not generate cryptographically secure random data.");
#else
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto result = ::getrandom(bytes.data() + offset, bytes.size() - offset, 0);
        if (result <= 0) throw std::runtime_error("Could not generate cryptographically secure random data.");
        offset += static_cast<std::size_t>(result);
    }
#endif

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : bytes) output << std::setw(2) << static_cast<unsigned int>(byte);
    return output.str();
}

bool constant_time_equal(std::string_view left, std::string_view right) noexcept {
    const std::size_t longest = (std::max)(left.size(), right.size());
    std::size_t difference = left.size() ^ right.size();
    for (std::size_t index = 0; index < longest; ++index) {
        const unsigned char l = index < left.size() ? static_cast<unsigned char>(left[index]) : 0;
        const unsigned char r = index < right.size() ? static_cast<unsigned char>(right[index]) : 0;
        difference |= static_cast<std::size_t>(l ^ r);
    }
    return difference == 0;
}

namespace {
std::string hex_digest(const unsigned char* data, std::size_t size) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < size; ++i) output << std::setw(2) << static_cast<unsigned int>(data[i]);
    return output.str();
}

#ifdef _WIN32
std::string bcrypt_sha256(const std::function<void(BCRYPT_HASH_HANDLE)>& feed) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<unsigned char> object;
    std::array<unsigned char, 32> digest{};
    auto close_handles = [&] {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    };
    try {
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
            throw std::runtime_error("Windows could not initialize SHA-256.");
        DWORD object_size = 0, copied = 0;
        if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                              reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &copied, 0) < 0 ||
            copied != sizeof(object_size) || object_size == 0)
            throw std::runtime_error("Windows could not query the SHA-256 object size.");
        object.resize(object_size);
        if (BCryptCreateHash(algorithm, &hash, object.data(), static_cast<ULONG>(object.size()), nullptr, 0, 0) < 0)
            throw std::runtime_error("Windows could not create a SHA-256 hash.");
        feed(hash);
        if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
            throw std::runtime_error("Windows could not finish a SHA-256 hash.");
    } catch (...) {
        close_handles();
        throw;
    }
    close_handles();
    return hex_digest(digest.data(), digest.size());
}

void bcrypt_feed(BCRYPT_HASH_HANDLE hash, const char* data, std::size_t size) {
    while (size != 0) {
        const auto chunk_size = (std::min)(size, static_cast<std::size_t>((std::numeric_limits<ULONG>::max)()));
        if (BCryptHashData(hash,
                           reinterpret_cast<PUCHAR>(const_cast<char*>(data)),
                           static_cast<ULONG>(chunk_size), 0) < 0)
            throw std::runtime_error("Windows could not hash SHA-256 data.");
        data += chunk_size;
        size -= chunk_size;
    }
}
#else
std::string openssl_sha256(const std::function<void(EVP_MD_CTX*)>& feed) {
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (!context) throw std::runtime_error("Could not create SHA-256 context.");
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    try {
        if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) throw std::runtime_error("Could not initialize SHA-256.");
        feed(context);
        if (EVP_DigestFinal_ex(context, digest.data(), &digest_size) != 1) throw std::runtime_error("Could not finish SHA-256.");
    } catch (...) {
        EVP_MD_CTX_free(context);
        throw;
    }
    EVP_MD_CTX_free(context);
    return hex_digest(digest.data(), digest_size);
}
#endif
} // namespace

std::string fingerprint(std::string_view text) {
    constexpr std::string_view domain = "ClippyVirtualCargoFingerprint:v1\0";
#ifdef _WIN32
    return bcrypt_sha256([&](BCRYPT_HASH_HANDLE hash) {
        bcrypt_feed(hash, domain.data(), domain.size());
        bcrypt_feed(hash, text.data(), text.size());
    });
#else
    return openssl_sha256([&](EVP_MD_CTX* context) {
        if (EVP_DigestUpdate(context, domain.data(), domain.size()) != 1 ||
            EVP_DigestUpdate(context, text.data(), text.size()) != 1)
            throw std::runtime_error("Could not hash fingerprint data.");
    });
#endif
}

std::string fingerprint_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Could not open file for SHA-256: " + path.string());
    std::array<char, 1024 * 1024> buffer{};
#ifdef _WIN32
    return bcrypt_sha256([&](BCRYPT_HASH_HANDLE hash) {
        while (input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = input.gcount();
            if (count > 0) bcrypt_feed(hash, buffer.data(), static_cast<std::size_t>(count));
        }
        if (!input.eof()) throw std::runtime_error("Could not finish reading file for SHA-256: " + path.string());
    });
#else
    return openssl_sha256([&](EVP_MD_CTX* context) {
        while (input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = input.gcount();
            if (count > 0 && EVP_DigestUpdate(context, buffer.data(), static_cast<std::size_t>(count)) != 1)
                throw std::runtime_error("Could not hash file data.");
        }
        if (!input.eof()) throw std::runtime_error("Could not finish reading file for SHA-256: " + path.string());
    });
#endif
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Could not open file: " + path.string());
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void write_text_file_atomic(const std::filesystem::path& path, std::string_view contents) {
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    const auto temporary = path.string() + ".tmp-" + random_hex(8);
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("Could not create file: " + temporary);
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.flush();
        if (!output) throw std::runtime_error("Could not finish writing file: " + temporary);
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary);
        throw std::runtime_error("Could not install file " + path.string() + ": " + error.message());
    }
}

} // namespace clippy
