#pragma once

#include <format>
#include <string>
#include <utility>
#include <variant>

namespace px {

enum class ErrorCode {
    Unknown,
    NotFound,
    InvalidArgument,
    InvalidData,
    Unsupported,
    IoError,
    LimitExceeded,
    AccessDenied,
};

[[nodiscard]] const char* errorCodeName(ErrorCode code) noexcept;

// An error with a machine-readable code and a human-readable message.
// Messages are written for creators: say what is wrong and where.
struct Error {
    ErrorCode code = ErrorCode::Unknown;
    std::string message;
};

template <class... Args>
[[nodiscard]] Error makeError(ErrorCode code, std::format_string<Args...> fmt, Args&&... args) {
    return Error{code, std::format(fmt, std::forward<Args>(args)...)};
}

namespace detail {
[[noreturn]] void badResultAccess(const Error* error) noexcept;
}

// Either a value or an Error. Used at module boundaries instead of exceptions.
// Accessing value() of a failed Result terminates with a diagnostic.
template <class T>
class [[nodiscard]] Result {
public:
    Result(T value) : m_storage(std::in_place_index<0>, std::move(value)) {}
    Result(Error error) : m_storage(std::in_place_index<1>, std::move(error)) {}

    [[nodiscard]] bool ok() const noexcept { return m_storage.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] T& value() & {
        check();
        return std::get<0>(m_storage);
    }
    [[nodiscard]] const T& value() const& {
        check();
        return std::get<0>(m_storage);
    }
    [[nodiscard]] T&& value() && {
        check();
        return std::get<0>(std::move(m_storage));
    }
    [[nodiscard]] const Error& error() const {
        if (ok()) {
            detail::badResultAccess(nullptr);
        }
        return std::get<1>(m_storage);
    }

    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }
    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }

private:
    void check() const {
        if (!ok()) {
            detail::badResultAccess(&std::get<1>(m_storage));
        }
    }

    std::variant<T, Error> m_storage;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : m_error(std::move(error)), m_ok(false) {}

    [[nodiscard]] bool ok() const noexcept { return m_ok; }
    explicit operator bool() const noexcept { return m_ok; }
    [[nodiscard]] const Error& error() const {
        if (m_ok) {
            detail::badResultAccess(nullptr);
        }
        return m_error;
    }

private:
    Error m_error;
    bool m_ok = true;
};

using Status = Result<void>;

} // namespace px
