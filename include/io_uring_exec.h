#pragma once
#include <cstring>
#include <chrono>
#include <mutex>
#include <thread>
#include <tuple>
#include <liburing.h>
#include <stdexec/execution.hpp>
#include <exec/async_scope.hpp>
#include "detail.hpp"
#include "io_uring_exec_run.h"

struct io_uring_exec: public detail::immovable,
                      private io_uring_exec_run<io_uring_exec>,
                      private detail::unified_stop_source<stdexec::inplace_stop_source>
{
    io_uring_exec(size_t uring_entries, int uring_flags = 0) {
        if(int err = io_uring_queue_init(uring_entries, &_underlying_uring, uring_flags)) {
            throw std::system_error(-err, std::system_category());
        }
    }

    struct constructor_parameters {
        size_t uring_entries;
        int uring_flags = 0;
    };

    // Example: io_uring_exec uring({.uring_entries=512});
    io_uring_exec(constructor_parameters p)
        : io_uring_exec(p.uring_entries, p.uring_flags)
    {}

    ~io_uring_exec() {
        final_run();
        io_uring_queue_exit(&_underlying_uring);
    }

    // Avoid requiring a default constructor in derived classes.
    struct intrusive_node {
        intrusive_node *_i_next {nullptr};
    };

    // All the tasks are asynchronous.
    // The `task` struct is queued by a user-space intrusive queue.
    // NOTE: The io_uring-specified task is queued by an interal ring of io_uring.
    struct task: detail::immovable, intrusive_node {
        using vtable = detail::make_vtable<
                        detail::add_complete_to_vtable<void(task*)>,
                        detail::add_cancel_to_vtable  <void(task*)>>;
        task(vtable vtab) noexcept: vtab(vtab) {}
        vtable vtab;
    };

    using intrusive_task_queue = detail::intrusive_queue<task, &task::_i_next>;

    // Required by stdexec.
    struct scheduler {
        template <stdexec::receiver Receiver>
        struct operation: task {
            using operation_state_concept = stdexec::operation_state_t;

            void start() noexcept {
                uring->_intrusive_queue.push(this);
            }

            inline constexpr static vtable this_vtable {
                {.complete = [](task *_self) noexcept {
                    auto &receiver = static_cast<operation*>(_self)->receiver;
                    using env_type = stdexec::env_of_t<Receiver>;
                    using stop_token_type = stdexec::stop_token_of_t<env_type>;
                    if constexpr (stdexec::unstoppable_token<stop_token_type>) {
                        stdexec::set_value(std::move(receiver));
                        return;
                    }
                    auto stop_token = stdexec::get_stop_token(stdexec::get_env(receiver));
                    stop_token.stop_requested() ?
                        stdexec::set_stopped(std::move(receiver))
                        : stdexec::set_value(std::move(receiver));
                }},
                {.cancel = [](task *_self) noexcept {
                    auto self = static_cast<operation*>(_self);
                    stdexec::set_stopped(std::move(self->receiver));
                }}
            };

            Receiver receiver;
            io_uring_exec *uring;
        };

        struct sender {
            using sender_concept = stdexec::sender_t;
            using completion_signatures = stdexec::completion_signatures<
                                            stdexec::set_value_t(),
                                            stdexec::set_stopped_t()>;
            struct env {
                template <typename CPO>
                auto query(stdexec::get_completion_scheduler_t<CPO>) const noexcept {
                    return scheduler{uring};
                }
                io_uring_exec *uring;
            };

            env get_env() const noexcept { return {uring}; }

            template <stdexec::receiver Receiver>
            operation<Receiver> connect(Receiver receiver) noexcept {
                return {{operation<Receiver>::this_vtable}, std::move(receiver), uring};
            }
            io_uring_exec *uring;
        };
        bool operator<=>(const scheduler &) const=default;
        sender schedule() noexcept { return {uring}; }
        io_uring_exec *uring;
    };

    scheduler get_scheduler() noexcept { return {this}; }

    // External structured callbacks support.
    struct io_uring_exec_operation_base: detail::immovable {
        using result_t = decltype(std::declval<io_uring_cqe>().res);
        using _self_t = io_uring_exec_operation_base;
        using vtable = detail::make_vtable<
                        detail::add_complete_to_vtable<void(_self_t*, result_t)>,
                        detail::add_cancel_to_vtable  <void(_self_t*)>>;
        vtable vtab;
        std::atomic<bool> seen {false};
    };

    // For CRTP.
    friend io_uring_exec_run<io_uring_exec>;

    // Run with customizable policy.
    //
    // If you want to change a few options based on a default config, try this way:
    // ```
    // constexpr auto policy = [] {
    //     auto policy = io_uring_exec::run_policy{};
    //     policy.launch = false;
    //     policy.realtime = true;
    //     // ...
    //     return policy;
    // } ();
    // uring.run<policy>();
    // ```
    //
    // If you want to change only one option, try this way:
    // ```
    // constexpr io_uring_exec::run_policy policy {.busyloop = true};
    // ```
    // NOTE: Designated initializers cannot be reordered.
    using io_uring_exec_run::run_policy;
    using io_uring_exec_run::run;

    using stop_source_type::underlying_stop_source_type;
    using stop_source_type::request_stop;
    using stop_source_type::stop_requested;
    using stop_source_type::stop_possible;
    using stop_source_type::get_stop_token;
    auto get_token() = delete;

    // See the comments on `coroutine.h` and `config.h`.
    // The `_inflight` value is estimated (or inaccurate).
    alignas(64) std::atomic<ssize_t> /*_estimated*/_inflight {};
    // A simple reference counter for run().
    alignas(64) std::atomic<size_t> _running_run {};
    intrusive_task_queue _intrusive_queue;
    detail::multi_lock<std::mutex> _submit_lock;
    io_uring _underlying_uring;
};
