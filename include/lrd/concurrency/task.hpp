#pragma once

#include <concepts>
#include <memory>
#include <type_traits>
#include <utility>

namespace lrd::concurrency {

/// A move-only, type-erased `void()` callable.
///
/// ## Why not std::function
///
/// std::function requires its target to be **copy-constructible**. That is not
/// a detail we can work around, and it collides directly with what step 5 needs
/// to do:
///
/// ```cpp
/// pool.post([stream = std::move(connection)]() mutable { serve(stream); });
/// ```
///
/// A UnixStream owns an Fd, which is deliberately move-only (copying it would
/// double-close a descriptor). A lambda capturing one is therefore move-only
/// too, and `std::function<void()>` will not compile against it. The same
/// applies to std::packaged_task and to any capture of a std::unique_ptr.
///
/// The usual workaround is to wrap the payload in a shared_ptr so the lambda
/// becomes copyable. That works, but it buys a second allocation and an atomic
/// refcount to paper over a requirement we never wanted - and it quietly
/// weakens the ownership story from "exactly one owner" to "however many
/// copies exist".
///
/// C++23 solves this properly with std::move_only_function. This build is C++20
/// (verified: __cpp_lib_move_only_function is not defined by libstdc++ under
/// -std=c++20), so Task is the ~40-line stand-in. If the project moves to
/// C++23, `using Task = std::move_only_function<void()>;` is the whole
/// migration.
///
/// ## How the type erasure works
///
/// The classic concept/model pair: an abstract base declaring the one operation
/// we need, and a template derived class holding the concrete callable. The
/// unique_ptr to the base is what makes Task a single, non-template type that
/// can sit in a std::deque, while the derived class remembers the callable's
/// real type. Move-only falls out for free, because unique_ptr is move-only.
///
/// The cost is one heap allocation and one virtual call per task. Against a
/// task that is about to do socket I/O, both are noise - but they are the
/// reason a small-buffer optimisation exists in real implementations.
class Task {
public:
    /// An empty Task. Calling it is undefined; check with operator bool.
    Task() noexcept = default;

    /// Wraps any callable invocable with no arguments.
    ///
    /// The `requires` clause excludes Task itself. Without it, this greedy
    /// template is a better match than the move constructor for a non-const
    /// `Task&`, and `Task b(a)` would try to wrap `a` in a new Task rather than
    /// move from it. That is a genuinely confusing failure, and the constraint
    /// is why it cannot happen.
    template <typename F>
        requires(!std::same_as<std::decay_t<F>, Task>) && std::invocable<std::decay_t<F>>
    Task(F&& callable)  // NOLINT(google-explicit-constructor) - implicit is the point
        : impl_(std::make_unique<Model<std::decay_t<F>>>(std::forward<F>(callable))) {}

    Task(Task&&) noexcept = default;
    Task& operator=(Task&&) noexcept = default;

    // Deleted rather than absent: an explicit deletion makes the compiler say
    // "use of deleted function" rather than silently picking the template
    // constructor above and producing an error deep inside it.
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    ~Task() = default;

    void operator()() { impl_->invoke(); }

    [[nodiscard]] explicit operator bool() const noexcept { return impl_ != nullptr; }

private:
    struct Concept {
        virtual ~Concept() = default;
        virtual void invoke() = 0;
    };

    template <typename F>
    struct Model final : Concept {
        explicit Model(F&& callable) : fn(std::move(callable)) {}
        explicit Model(const F& callable) : fn(callable) {}

        // Not const: a std::packaged_task's operator() is non-const, and so is
        // a mutable lambda's. Marking this const would silently exclude both.
        void invoke() override { fn(); }

        F fn;
    };

    std::unique_ptr<Concept> impl_;
};

}  // namespace lrd::concurrency
