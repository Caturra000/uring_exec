#pragma once
#include <cstring>
#include <chrono>
#include <mutex>
#include <thread>
#include <tuple>
#include <liburing.h>
#include <stdexec/execution.hpp>
#include <exec/async_scope.hpp>
#include "detail.h"
#include "io_uring_exec_internal_run.h"
#include "underlying_io_uring.h"
namespace uring_exec {
namespace internal {

////////////////////////////////////////////////////////////////////// Task support

// Avoid requiring a default constructor in derived classes.
struct intrusive_node {
    intrusive_node *_i_next {nullptr};
};

// All the tasks are asynchronous.
// The `io_uring_exec_task` struct is queued by a user-space intrusive queue.
// NOTE: The io_uring-specified task is queued by an interal ring of io_uring.
struct io_uring_exec_task: detail::immovable, intrusive_node {
    using vtable = detail::make_vtable<
                    detail::add_complete_to_vtable<void(io_uring_exec_task*)>,
                    detail::add_cancel_to_vtable  <void(io_uring_exec_task*)>>;
    io_uring_exec_task(vtable vtab) noexcept: vtab(vtab) {}
    // Receiver types erased.
    vtable vtab;
};

// Atomic version.
using intrusive_task_queue = detail::intrusive_queue<io_uring_exec_task, &io_uring_exec_task::_i_next>;

////////////////////////////////////////////////////////////////////// io_uring async operations

// External structured callbacks support.
// See io_uring_exec_operation.h and io_uring_exec_sender.h for more details.
struct io_uring_exec_operation_base: detail::immovable {
    using result_t = decltype(std::declval<io_uring_cqe>().res);
    using _self_t = io_uring_exec_operation_base;
    using vtable = detail::make_vtable<
                    detail::add_complete_to_vtable<void(_self_t*, result_t)>,
                    detail::add_cancel_to_vtable  <void(_self_t*)>,
                    detail::add_restart_to_vtable <void(_self_t*)>>;
    vtable vtab;
};

////////////////////////////////////////////////////////////////////// Local side

struct io_uring_exec;

// Is-a runnable io_uring.
struct io_uring_exec_local: public underlying_io_uring,
                            public io_uring_exec_run<io_uring_exec_local,
                                            io_uring_exec_operation_base>
{
    io_uring_exec_local(constructor_parameters p,
                        io_uring_exec &root);

    ~io_uring_exec_local();

    using io_uring_exec_run::run_policy;
    using io_uring_exec_run::run;

    using root_type = io_uring_exec;

    auto& get_remote() noexcept { return _root; }
    auto& get_local() noexcept { return *this; }

    size_t _inflight {};
    io_uring_exec &_root;
};

////////////////////////////////////////////////////////////////////// control block

struct io_uring_exec: public underlying_io_uring, // TODO: No need to use a backup uring.
                      public io_uring_exec_run<io_uring_exec, io_uring_exec_operation_base>,
                      private detail::unified_stop_source<stdexec::inplace_stop_source>
{
    // Example: io_uring_exec uring({.uring_entries=512});
    io_uring_exec(underlying_io_uring::constructor_parameters params) noexcept
        : underlying_io_uring(params), _uring_params(params) {}

    io_uring_exec(unsigned uring_entries, int uring_flags = 0) noexcept
        : io_uring_exec({.uring_entries = uring_entries, .uring_flags = uring_flags}) {}

    ~io_uring_exec() {
        // For on-stack uring.run().
        io_uring_exec_run::transfer_run();
        // Cancel all the pending tasks/operations.
        io_uring_exec_run::terminal_run();
    }

    auto& get_local() noexcept {
        thread_local io_uring_exec_local local(_uring_params, *this);
        return local;
    }

    auto& get_remote() noexcept {
        return *this;
    }

    using task = internal::io_uring_exec_task;
    using operation_base = internal::io_uring_exec_operation_base;

    // Required by stdexec.
    // Most of its functions are invoked by stdexec.
    struct scheduler;

    scheduler get_scheduler() noexcept;

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
    // No effect.
    // Just remind you that it differs from the C++ standard.
    auto get_token() = delete;

    underlying_io_uring::constructor_parameters _uring_params;
    exec::async_scope _transfer_scope;
    intrusive_task_queue _immediate_queue;
    alignas(64) std::atomic<size_t> _running_local {};
};

////////////////////////////////////////////////////////////////////// stdexec scheduler

struct io_uring_exec::scheduler {
    template <stdexec::receiver Receiver>
    struct operation: io_uring_exec_task {
        using operation_state_concept = stdexec::operation_state_t;

        void start() noexcept {
            uring->_immediate_queue.push(this);
        }

        inline constexpr static vtable this_vtable {
            {.complete = [](io_uring_exec_task *_self) noexcept {
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
            {.cancel = [](io_uring_exec_task *_self) noexcept {
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

inline
io_uring_exec_local::io_uring_exec_local(
    io_uring_exec_local::constructor_parameters p,
    io_uring_exec &root)
    : underlying_io_uring(p),
      _root(root)
{
    _root._running_local.fetch_add(1, std::memory_order::relaxed);
}

inline
io_uring_exec_local::~io_uring_exec_local() {
    io_uring_exec_run::transfer_run();
    _root._running_local.fetch_sub(1, std::memory_order::acq_rel);
}

inline io_uring_exec::scheduler io_uring_exec::get_scheduler() noexcept {
    return {this};
}

} // namespace internal
} // namespace uring_exec
