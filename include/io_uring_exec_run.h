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
#include <stdexec/execution.hpp>

// CRTP for `io_uring_exec::run()` and `io_uring_exec::final_run()`.
template <typename Exec_crtp_derived>
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
        bool realtime {false};          // No deferred processing.
        bool waitable {false};          // Submit and wait.
        bool hookable {true};           // Always true beacause of per-object vtable.
        bool detached {false};          // Ignore stop requests from `io_uring_exec`.

        bool terminal {false};          // For stopped context.
    };

    template <run_policy policy = {},
              // Compatible with std::jthread and std::stop_token.
              typename any_stop_token_t = stdexec::never_stop_token>
    void run(any_stop_token_t external_stop_token = {}) {
        static_assert(
            [](auto ...options) {
                return (int{options} + ...) == 1;
            } (policy.concurrent,
               policy.weakly_parallel,
               policy.weakly_concurrent),

            "Small. Fast. Reliable. Choose any three."
        );

        using task = Exec_crtp_derived::task;
        using io_uring_exec_operation_base = Exec_crtp_derived::io_uring_exec_operation_base;
        auto &_inflight         = that()->_inflight;
        auto &_running_run      = that()->_running_run;
        auto &_intrusive_queue  = that()->_intrusive_queue;
        auto &_submit_lock      = that()->_submit_lock;
        auto &_underlying_uring = that()->_underlying_uring;

        // Progress.
        task *first_task {};        // For empty queue detection.
        ssize_t submitted {};       // For `io_uring_submit`.
        ssize_t done {};            // For `io_uring_for_each_cqe`.

        // Deferred processing.
        bool deferred_initialized {};
        ssize_t local_inflight {};
        ssize_t plugged_done {};
        constexpr size_t scheduled_sync_ratio = 32;

        auto on_exit = [](auto f) {
            auto _0x1 = std::uintptr_t {0x1};
            // reinterpret_cast is not a constexpr.
            auto make_STL_happy = reinterpret_cast<void*>(_0x1);
            auto make_dtor_happy = [f = std::move(f)](...) { f(); };
            using Defer = std::unique_ptr<void, decltype(make_dtor_happy)>;
            return Defer{make_STL_happy, std::move(make_dtor_happy)};
        };

        auto unplug = on_exit([&] {
            if constexpr (policy.realtime) return;
            if(plugged_done == 0) return;
            // We don't need an accurate execution point.
            _inflight.fetch_sub(plugged_done, std::memory_order::relaxed);
        });

        // We need a latch effect here.
        _running_run.fetch_add(1, std::memory_order::acq_rel);
        auto countdown = on_exit([&] {
            _running_run.fetch_sub(1, std::memory_order::acq_rel);
            // TODO: Notify for a rare case?
        });

        for(auto step : std::views::iota(1 /* Avoid pulling immediately */)) {
            if constexpr (policy.launch) {
                auto &q = _intrusive_queue;
                auto op = q.move_all();
                // NOTE:
                // We need to get the `next(op)` first.
                // Because `op` will be destroyed after complete/cancel().
                auto safe_for_each = [&q, op](auto &&f) mutable {
                    // It won't modify the outer `op`.
                    for(; op; f(std::exchange(op, q.next(op))));
                };
                safe_for_each([](auto op) {
                    if constexpr (policy.terminal) {
                        op->vtab.cancel(op);
                    } else {
                        op->vtab.complete(op);
                    }
                });
                first_task = op;
            }

            // `_inflight` is only needed when `autoquit` is enabled.
            // We can do deferred processing on it to avoid any communication overhead.
    
            if constexpr (policy.submit) {
                // Must hold a lock. There are many data races
                // between sqe allocation/preparation and submission.
                //
                // For non-SQPOLL mode, `submit` is a syscall (AKA a very slow function) wrapper.
                // So we use slowpath here, and leave fastpath to operation.start().
                auto guard = _submit_lock.slowpath_guard();
                // TODO: wait_{one|some|all}.
                if constexpr (policy.waitable) {
                    submitted = io_uring_submit_and_wait(&_underlying_uring, 1);
                } else {
                    submitted = io_uring_submit(&_underlying_uring);
                }
            }

            if constexpr (policy.iodone) {
                io_uring_cqe *cqe;
                done = 0;
                constexpr auto mo = std::memory_order::relaxed;

                // There may be a contention between concurrent run()s.
                while(!io_uring_peek_cqe(&_underlying_uring, &cqe)) {
                    auto user_data = io_uring_cqe_get_data(cqe);
                    if constexpr (policy.terminal) {
                        if(test_terminal_command(user_data)) {
                            io_uring_cqe_seen(&_underlying_uring, cqe);
                            continue;
                        }
                    }

                    using uop = io_uring_exec_operation_base;
                    auto uring_op = std::bit_cast<uop*>(user_data);
                    // It won't change any other shared variable,
                    // and reorder is ok because of data/control dependency,
                    // so just use relaxed order.
                    bool expected = false, desired = true;
                    // NOTES:
                    // * Not allowed to fail spuriously, use strong version.
                    // * xchg should be more API-friendly than cmpxchg,
                    //   but we need to consider performance on read-side failure.
                    if(auto &seen = uring_op->seen;
                       !seen.compare_exchange_strong(expected, desired, mo, mo))
                    {
                        continue;
                    }
                    // Cached before seen(), io_uring may overwrite this value.
                    auto cqe_res = cqe->res;
                    io_uring_cqe_seen(&_underlying_uring, cqe);
                    done++;
                    uring_op->vtab.complete(uring_op, cqe_res);
                }
            }

            if constexpr (policy.realtime) {
                // Avoid 0.
                if(done) {
                    constexpr auto acq_rel = std::memory_order::acq_rel;
                    local_inflight = _inflight.fetch_sub(done, acq_rel);
                }
            } else if constexpr (policy.autoquit) {
                plugged_done += done;
            }

            if constexpr (policy.weakly_parallel) {
                return;
            }

            bool any_progress = false;
            if constexpr (policy.launch) any_progress |= bool(first_task);
            if constexpr (policy.submit) any_progress |= bool(submitted);
            if constexpr (policy.iodone) any_progress |= bool(done);

            if constexpr (policy.weakly_concurrent) {
                if(any_progress) return;
            }

            // Per-run() stop token.
            //
            // We use `stdexec::never_stop_token` by default.
            // Its `stop_requested()` returns false in a constexpr way.
            // So we don't need to add another detached policy here.
            if(external_stop_token.stop_requested()) {
                return;
            }

            // Ignore the context's stop request can help reduce at least one atomic operation.
            // This might be useful for some network I/O patterns.
            if constexpr (not policy.detached) {
                if(that()->stop_requested()) {
                    return;
                }
            }

            if constexpr (not policy.busyloop) {
                if(any_progress) std::this_thread::yield();
            }

            if constexpr (policy.autoquit) {
                if(any_progress) continue;
                bool scheduled_synchronizable = (step % scheduled_sync_ratio == 0);
                constexpr struct {
                    struct {} forced;
                    struct {} normal;
                } mode;
                auto synchronize = [&, synchronized = false](auto tag = {}) mutable {
                    constexpr bool forced = requires { tag == mode.forced; };
                    if constexpr (!forced) {
                        if(std::exchange(synchronized, true)) return;
                    }
                    constexpr auto mo = forced ?
                          std::memory_order::acq_rel
                        : std::memory_order::relaxed;
                    if(auto delta = std::exchange(plugged_done, 0)) {
                        local_inflight = _inflight.fetch_sub(delta, mo);
                    } else {
                        local_inflight = _inflight.load(mo);
                    }
                };

                // 1. On-demand synchronization.
                if(!deferred_initialized) {
                    deferred_initialized = true;
                    synchronize(mode.normal);
                }
                // 2. Scheduled synchronization.
                if(scheduled_synchronizable) {
                    synchronize(mode.normal);
                }
                // 3. Best-effort synchronization before actual `return`.
                if(local_inflight <= 0) {
                    synchronize(mode.forced);
                }
                // 4. Finally...
                if(local_inflight <= 0) {
                    return;
                }
            }
        }
    }

    // Clear all the pending opeartions.
    void final_run() {
        auto &_running_run      = that()->_running_run;
        auto &_underlying_uring = that()->_underlying_uring;

        that()->request_stop();
        // NOTE: (stop+countdown) is not an atomic transaction.
        while(_running_run.load(std::memory_order::acquire)) {
            // TODO: atomic.wait().
            std::this_thread::yield();
        }

        // Flush, and ensure that the cancel-sqe must be allocated successfully.
        io_uring_submit(&_underlying_uring);
        auto sqe = io_uring_get_sqe(&_underlying_uring);
        io_uring_sqe_set_data(sqe, make_terminal_command());
        io_uring_prep_cancel(sqe, {}, IORING_ASYNC_CANCEL_ANY);

        constexpr auto final_policy = [] {
            auto policy = run_policy{};
            policy.terminal = true;
            policy.concurrent = false;
            policy.weakly_parallel = true;
            return policy;
        } ();

        // Cancel!
        run<final_policy>();
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

    constexpr auto make_terminal_command() noexcept {
        // Impossible address for Linux user space.
        auto impossible = std::numeric_limits<std::uintptr_t>::max();
        // Don't care about whether it is a value or a pointer.
        return std::bit_cast<unified_user_data_type>(impossible);
    }

    constexpr auto test_terminal_command(unified_user_data_type user_data) noexcept {
        return make_terminal_command() == user_data;
    }
};