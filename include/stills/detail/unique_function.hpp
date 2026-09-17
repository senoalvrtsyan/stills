#pragma once
// stills/detail/unique_function.hpp — a small move-only type-erased callable.
// std::move_only_function is not available on libc++ < 20 (Apple clang), and std::function
// would reject handlers that capture move-only state such as std::promise or std::unique_ptr.

#include <cassert>
#include <concepts>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace stills::detail {

template <class Signature>
class unique_function;

template <class R, class... Args>
class unique_function<R(Args...)> {
  struct concept_t {
    virtual ~concept_t() = default;
    virtual R invoke(Args&&... args) = 0;
  };
  template <class F>
  struct model_t final : concept_t {
    F fn;
    explicit model_t(F&& f) : fn(std::move(f)) {}
    explicit model_t(const F& f) : fn(f) {}
    R invoke(Args&&... args) override { return std::invoke(fn, std::forward<Args>(args)...); }
  };

 public:
  unique_function() noexcept = default;
  unique_function(std::nullptr_t) noexcept {}  // NOLINT(google-explicit-constructor)

  template <class F>
    requires(!std::same_as<std::decay_t<F>, unique_function> &&
             std::is_invocable_r_v<R, std::decay_t<F>&, Args...>)
  unique_function(F&& f)  // NOLINT(google-explicit-constructor)
      : impl_(std::make_unique<model_t<std::decay_t<F>>>(std::forward<F>(f))) {}

  unique_function(unique_function&&) noexcept = default;
  unique_function& operator=(unique_function&&) noexcept = default;
  unique_function(const unique_function&) = delete;
  unique_function& operator=(const unique_function&) = delete;
  ~unique_function() = default;

  /// Precondition: not empty. Calling an empty one is UB; the assert names it in debug builds.
  R operator()(Args... args) {
    assert(impl_ != nullptr && "called an empty unique_function");
    return impl_->invoke(std::forward<Args>(args)...);
  }

  [[nodiscard]] explicit operator bool() const noexcept { return impl_ != nullptr; }
  void reset() noexcept { impl_.reset(); }

 private:
  std::unique_ptr<concept_t> impl_;
};

}  // namespace stills::detail
