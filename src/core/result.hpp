#pragma once

#include <cassert>
#include <string>
#include <utility>
#include <variant>

namespace gateway {

/// Machine-readable error identity plus human-readable detail.
/// Codes are stable lowercase strings: "transport", "protocol", "venue:<sCode>", ...
struct Error
{
    std::string code;
    std::string message;

    friend auto operator==(const Error& a_lhs, const Error& a_rhs) -> bool = default;
};

/// C++20 stand-in for std::expected (C++23 is out of scope).
/// Expected failures are communicated through Result; accessors must be
/// guarded by is_ok(). value()/error() have is_ok()/!is_ok() as preconditions.
template <typename T> class Result
{
  public:
    Result(T a_value)
        requires(!std::is_same_v<T, Error>)
        : storage_(std::in_place_index<0>, std::move(a_value))
    {}

    Result(Error a_error) : storage_(std::in_place_index<1>, std::move(a_error)) {}

    [[nodiscard]] auto is_ok() const noexcept -> bool { return storage_.index() == 0; }

    /// Precondition: is_ok().
    [[nodiscard]] auto value() const& -> const T&
    {
        assert(is_ok());
        return std::get<0>(storage_);
    }

    /// Precondition: is_ok().
    [[nodiscard]] auto value() && -> T
    {
        assert(is_ok());
        return std::get<0>(std::move(storage_));
    }

    /// Precondition: !is_ok().
    [[nodiscard]] auto error() const& -> const Error&
    {
        assert(!is_ok());
        return std::get<1>(storage_);
    }

    [[nodiscard]] auto value_or(T a_default) const& -> T
    {
        return is_ok() ? std::get<0>(storage_) : std::move(a_default);
    }

  private:
    std::variant<T, Error> storage_;
};

template <typename T> Result(T) -> Result<T>;

} // namespace gateway
