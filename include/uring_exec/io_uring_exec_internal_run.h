#pragma once
#include <fcntl.h>
#include <liburing.h>
#include <atomic>
#include <bit>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>
#include <system_error>
#include <ranges>
#include <algorithm>
#include <stdexec/execution.hpp>
#include "underlying_io_uring.h"
namespace uring_exec {
namespace internal {

struct io_uring_exec_local;

// CRTP for `io_uring_exec_local::run()` and `io_uring_exec_local::final_run()`.
template <typename Exec_crtp_derived,
          typename io_uring_exec_operation_base>
struct io_uring_exec_run {
    struct run_policy {
        // Informal forward progress guarantees.
        // NOTES:
        // * These are exclusive flags, but using bool (not enum) for simplification.
        // * `weakly_concurrent` is not a C++ standard part, which can make progress eventually
        //   with lower overhead compared to `concurrent`, provided it is used properly.
        // * `parallel` (which makes progress per `step`) is NOT supported for IO operations.
        bool concurrent {true};         // which requires that a thread makes progress eventually.
        bool weakly_parallel {false};   // which does not require that the thread makes progress.
        bool weakly_concurrent {false}; // which requires that a thread may make progress eventually.

        // Event handling.
        // Any combination is welcome.
        bool launch {true};
        bool submit {true};
        bool iodone {true};

        // Behavior details.
        bool busyloop {false};          // No yield.
        bool autoquit {false};          // `concurrent` runs infinitely by default.
        bool realtime {false};          // (DEPRECATED) No deferred processing.
        bool waitable {false};          // Submit and wait.
        bool hookable {true};           // Always true beacause of per-object vtable.
        bool detached {false};          // Ignore stop requests from `io_uring_exec`.
        bool progress {false};          // run() returns run_progress_info.

        bool transfer {false};          // For stopeed local context. Just a tricky restart.
        bool terminal {false};          // For stopped remote context. Cancel All.
    };

    struct run_progress_info {
        size_t loop_step {};
        size_t launched {};             // For intrusive task queue.
        size_t submitted {};            // For `io_uring_submit`.
        size_t done {};                 // For `io_uring_for_each_cqe`.

        run_progress_info operator()(size_t final_step) noexcept {
            loop_step = final_step;
            return *this;
        }

        // Vertically sum all the class members.
        auto& operator+=(const run_progress_info &rhs) noexcept {
            // Don't worry about the codegen performance,
            // it is as efficient as manually summing each member by name.
            auto &lhs = *this;
            using strict_alias = std::array<size_t, 4>;
            auto l = std::bit_cast<strict_alias>(lhs);
            auto r = std::bit_cast<strict_alias>(rhs);
            std::ranges::transform(l, r, begin(l), std::plus());
            return lhs = std::bit_cast<run_progress_info>(l);
        }
    };

    // Tell the compiler we're not using the return value.
    struct run_progress_no_info {
        decltype(std::ignore) _1, _2, _3, _4;
        void operator()(...) const noexcept {}
        auto operator+=(const auto &) const noexcept {}
    };

    // run_policy:       See the comments above.
    // any_stop_token_t: Compatible with `std::jthread` and `std::stop_token` for a unified interface.
    // Return type:      Either `run_progress_info` or `void`, depending on `run_policy.progress`.
    template <run_policy policy = {},
              typename any_stop_token_t = stdexec::never_stop_token>
    auto run(any_stop_token_t external_stop_token = {}) {
        constexpr auto sum_options = [](auto ...options) {
            return (int{options} + ...);
        };
        static_assert(
            sum_options(policy.concurrent,
                        policy.weakly_parallel,
                        policy.weakly_concurrent)
            == 1,
            "Small. Fast. Reliable. Choose any three."
        );

        constexpr bool any_progress_possible =
            sum_options(policy.launch, policy.submit, policy.iodone);

        auto &remote = that()->get_remote();
        auto &local = that()->get_local();

        // Progress, and the return value of run().
        auto progress_info = [] {
            if constexpr (policy.progress) {
                return run_progress_info();
            } else {
                return run_progress_no_info();
            }
        } ();
        auto progress_info_one_step = run_progress_info();
        auto &&[_, launched, submitted, done] = progress_info_one_step;

        // We don't need this legacy way.
        // It was originally designed to work with a single std::stop_token type,
        // and thus requires a runtime check for a unified (non-void, but won't stop) interface.
        //
        // Instead, use `stdexec::never_stop_token` for a more constexpr-friendly way.
        // We can infer the compile-time information from its type.
        //
        // If a type other than never_stop_token is passed to this function,
        // we assume that it must be `stop_possible() == true`.
        // This allow us to check stop_requested directly,
        // and reduce at least one trivial operation.
        //
        // auto legacy_stop_requested =
        //     [&, possible = external_stop_token.stop_possible()] {
        //         if(!possible) return false;
        //         return external_stop_token.stop_requested();
        //     };

        // Return `step` as a performance hint.
        for(auto step : std::views::iota(1 /* 0 means no-op. */)) {
            if constexpr (policy.launch && not policy.transfer) {
                auto &q = remote._immediate_queue;
                auto op = q.move_all();
                // NOTE:
                // We need to get the `next(op)` first.
                // Because `op` will be destroyed after complete/cancel().
                auto safe_for_each = [&q, op](auto &&f) mutable {
                    // It won't modify the outer `op`.
                    // If we need any later operation on it.
                    for(; op; f(std::exchange(op, q.next(op))));
                };
                safe_for_each([&launched](auto op) {
                    if constexpr (policy.terminal) {
                        op->vtab.cancel(op);
                        // Make Clang happy.
                        (void)launched;
                    } else {
                        op->vtab.complete(op);
                        launched++;
                    }
                });
                // TODO: record the first task (op).
            }

            if constexpr (policy.submit) {
                // TODO: wait_{one|some|all}.
                if constexpr (policy.waitable) {
                    submitted = io_uring_submit_and_wait(&local, 1);
                } else {
                    submitted = io_uring_submit(&local);
                }
            }

            if constexpr (policy.iodone) {
                io_uring_cqe *cqe;

                // TODO: use batch peek.
                while(!io_uring_peek_cqe(&local, &cqe)) {
                    auto user_data = io_uring_cqe_get_data(cqe);
                    if constexpr (policy.terminal || policy.transfer) {
                        if(test_destructive_command(user_data)) {
                            io_uring_cqe_seen(&local, cqe);
                            continue;
                        }
                    }

                    using uop = io_uring_exec_operation_base;
                    auto uring_op = std::bit_cast<uop*>(user_data);
                    auto cqe_res = cqe->res;
                    io_uring_cqe_seen(&local, cqe);
                    done++;
                    local._inflight--;
                    if constexpr (policy.transfer) {
                        uring_op->vtab.restart(uring_op);
                    } else {
                        uring_op->vtab.complete(uring_op, cqe_res);
                    }
                }
            }

            if constexpr (policy.weakly_parallel) {
                return progress_info(step);
            }

            bool any_progress = false;
            if constexpr (any_progress_possible) {
                any_progress |= bool(launched);
                any_progress |= bool(submitted);
                any_progress |= bool(done);
                progress_info += progress_info_one_step;
                progress_info_one_step = {};
            }

            if constexpr (policy.weakly_concurrent) {
                if(any_progress) return progress_info(step);
            }

            // Per-run() stop token.
            //
            // We use `stdexec::never_stop_token` by default.
            // Its `stop_requested()` returns false in a constexpr way.
            // So we don't need to add another detached policy here.
            if(external_stop_token.stop_requested()) {
                return progress_info(step);
            }

            // Ignore the context's stop request can help reduce at least one atomic operation.
            // This might be useful for some network I/O patterns.
            if constexpr (not policy.detached) {
                if(remote.stop_requested()) {
                    // TODO: Eager cancel.
                    return progress_info(step);
                }
            }

            if constexpr (policy.autoquit) {
                if(!local._inflight) {
                    return progress_info(step);
                }
            }

            if constexpr (not policy.busyloop) {
                if(!any_progress) {
                    std::this_thread::yield();
                }
            }
        }
        return progress_info(0);
    }

    void transfer_run() {
        auto &local = that()->get_local();
        prepare_cancellation(local);
        constexpr auto policy = [] {
            auto policy = run_policy{};
            policy.concurrent = false;
            policy.weakly_parallel = true;
            policy.transfer = true;
            return policy;
        } ();
        run<policy>();
    }

    void terminal_run() {
        auto &main_local = that()->get_local();
        auto &remote = that()->get_remote();
        // Exit run() and transfer.
        remote.request_stop();
        // TODO: latch or std::atomic::wait().
        // Main thread == 1.
        while(remote._running_local.load(std::memory_order::acquire) > 1) {
            std::this_thread::yield();
        }
        prepare_cancellation(main_local);
        constexpr auto policy = [] {
            auto policy = run_policy{};
            policy.concurrent = false;
            policy.weakly_parallel = true;
            policy.terminal = true;
            return policy;
        } ();
        run<policy>();
    }

private:

    constexpr auto that() noexcept -> Exec_crtp_derived* {
        return static_cast<Exec_crtp_derived*>(this);
    }

    // liburing has different types between cqe->`user_data` and set_data[64](`user_data`).
    // Don't know why.
    using unified_user_data_type = decltype(
        []<typename R, typename T>(R(io_uring_sqe*, T)) {
            return T{};
        } (io_uring_sqe_set_data));

    constexpr auto make_destructive_command() noexcept {
        // Impossible address for Linux user space.
        auto impossible = std::numeric_limits<std::uintptr_t>::max();
        // Don't care about whether it is a value or a pointer.
        return std::bit_cast<unified_user_data_type>(impossible);
    }

    constexpr auto test_destructive_command(unified_user_data_type user_data) noexcept {
        return make_destructive_command() == user_data;
    }

    void prepare_cancellation(underlying_io_uring &uring) {
        // Flush, and ensure that the cancel-sqe must be allocated successfully.
        io_uring_submit(&uring);
        auto sqe = io_uring_get_sqe(&uring);
        io_uring_sqe_set_data(sqe, make_destructive_command());
        io_uring_prep_cancel(sqe, {}, IORING_ASYNC_CANCEL_ANY);
    }
};

} // namespace internal
} // namespace uring_exec
