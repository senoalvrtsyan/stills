#pragma once
// stills/detail/stills_UniqueFunction.h — a small move-only type-erased callable.
// std::move_only_function is not available on libc++ < 20 (Apple clang), and std::function
// would reject handlers that capture move-only state such as std::promise or std::unique_ptr.

#include <cassert>
#include <concepts>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace stills::detail
{

template <class Signature>
class UniqueFunction;

template <class R, class... Args>
class UniqueFunction<R (Args...)>
{
    struct Concept
    {
        virtual ~Concept() = default;
        virtual R invoke (Args&&... args) = 0;
    };

    template <class F>
    struct Model final : Concept
    {
        F fn;
        explicit Model (F&& f) : fn (std::move (f)) {}
        explicit Model (const F& f) : fn (f) {}
        R invoke (Args&&... args) override { return std::invoke (fn, std::forward<Args> (args)...); }
    };

public:
    UniqueFunction() noexcept = default;
    UniqueFunction (std::nullptr_t) noexcept {}

    template <class F>
        requires (! std::same_as<std::decay_t<F>, UniqueFunction>
                  && std::is_invocable_r_v<R, std::decay_t<F>&, Args...>)
    UniqueFunction (F&& f) : impl (std::make_unique<Model<std::decay_t<F>>> (std::forward<F> (f)))
    {
    }

    UniqueFunction (UniqueFunction&&) noexcept = default;
    UniqueFunction& operator= (UniqueFunction&&) noexcept = default;
    UniqueFunction (const UniqueFunction&) = delete;
    UniqueFunction& operator= (const UniqueFunction&) = delete;
    ~UniqueFunction() = default;

    // Precondition: not empty. Calling an empty one is UB; the assert names it in debug builds.
    R operator() (Args... args)
    {
        assert (impl != nullptr && "called an empty UniqueFunction");
        return impl->invoke (std::forward<Args> (args)...);
    }

    [[nodiscard]] explicit operator bool() const noexcept { return impl != nullptr; }
    void reset() noexcept { impl.reset(); }

private:
    std::unique_ptr<Concept> impl;
};

} // namespace stills::detail
