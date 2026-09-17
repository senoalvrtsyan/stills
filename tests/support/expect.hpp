#pragma once
#include <catch2/catch_test_macros.hpp>
#include <expected>
#include <stills/error.hpp>
#include <utility>

namespace testsupport {
template <class T, class E>
T unwrap(std::expected<T, E>&& e) {
  return std::move(*e);
}
template <class E>
void unwrap(std::expected<void, E>&&) {}
}  // namespace testsupport

/// Unwraps a std::expected, failing the test with the Error text when it holds an error.
#define REQUIRE_OK(expr)                                                       \
  [&]() -> decltype(auto) {                                                    \
    auto&& stills_result_ = (expr);                                            \
    if (!stills_result_) FAIL(#expr << " failed: " << stills_result_.error()); \
    return ::testsupport::unwrap(std::move(stills_result_));                   \
  }()

/// Requires an error with the given code.
#define REQUIRE_ERROR(expr, expected_code)                   \
  do {                                                       \
    auto&& stills_result_ = (expr);                          \
    REQUIRE_FALSE(stills_result_.has_value());               \
    INFO("error: " << stills_result_.error());               \
    REQUIRE(stills_result_.error().code == (expected_code)); \
  } while (false)
