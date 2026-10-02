#pragma once
#include <concepts>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace Engine {

// Type-erased callable that only needs to be movable (C++20 has no std::move_only_function):
// lets handlers own unique_ptrs, Subscriptions, ...
template <class Signature>
class MoveOnlyFunction;

template <class R, class... Args>
class MoveOnlyFunction<R(Args...)> {
public:
    MoveOnlyFunction() = default;

    template <class F>
        requires(!std::same_as<std::remove_cvref_t<F>, MoveOnlyFunction> && std::invocable<std::decay_t<F>&, Args...>)
    MoveOnlyFunction(F&& fn) // NOLINT(google-explicit-constructor): drop-in for std::function
        : m_Impl(std::make_unique<Model<std::decay_t<F>>>(std::forward<F>(fn)))
    {
    }

    MoveOnlyFunction(MoveOnlyFunction&&) noexcept            = default;
    MoveOnlyFunction& operator=(MoveOnlyFunction&&) noexcept = default;
    MoveOnlyFunction(const MoveOnlyFunction&)                = delete;
    MoveOnlyFunction& operator=(const MoveOnlyFunction&)     = delete;

    R operator()(Args... args) { return m_Impl->Call(std::forward<Args>(args)...); }
    [[nodiscard]] explicit operator bool() const noexcept { return m_Impl != nullptr; }

private:
    struct Concept {
        virtual ~Concept()        = default;
        virtual R Call(Args... args) = 0;
    };
    template <class F>
    struct Model final : Concept {
        template <class G>
        explicit Model(G&& g) : fn(std::forward<G>(g)) {}
        R Call(Args... args) override { return std::invoke(fn, std::forward<Args>(args)...); }
        F fn;
    };

    std::unique_ptr<Concept> m_Impl;
};

} // namespace Engine
