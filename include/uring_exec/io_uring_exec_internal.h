#pragma once
#include <cstring>
#include <chrono>
#include <mutex>
#include <thread>
#include <tuple>
#include <array>
#include <random>
#include <liburing.h>
#include <stdexec/execution.hpp>
#include <exec/async_scope.hpp>
#include "detail.h"
#include "io_uring_exec_internal_run.h"
#include "underlying_io_uring.h"
namespace uring_exec {
namespace internal {

////////////////////////////////////////////////////////////////////// Containers

// Avoid requiring a default constructor in derived classes.
struct intrusive_node {
    intrusive_node *_i_next {nullptr};
};

// Simple fixed-size map to reduce thread contention.
template <typename Bucket, size_t Fixed_bucket_size = 16>
struct trivial_intrusive_map: detail::immovable {
    using bucket_type = Bucket;
    using element_type = typename Bucket::element_type;

    auto& operator[](size_t index) noexcept { return map[index]; }
    auto& operator[](element_type *element) noexcept {
        // Elements have stable addresses and can locate their queue using their own address.
        static_assert(
                !std::is_move_constructible_v<element_type> &&
                !std::is_move_assignable_v<element_type>);
        auto index = std::bit_cast<uintptr_t>(element) % N_way_concurrency;
        return map[index];
    }

    struct robin_access_pattern {
        trivial_intrusive_map &self;
        auto& operator[](size_t index) const noexcept { return self[index % Fixed_bucket_size]; };
    };

    struct random_access_pattern {
        trivial_intrusive_map &self;
        auto& operator[](size_t) const noexcept { return self[mt_random()]; };

        // Random device may perform an open() syscall and fd won't be closed until dtor is called.
        // Therefore, we don't cache it.
        inline static thread_local auto mt_random =
            [algo = std::mt19937_64{std::random_device{}()},
             dist = std::uniform_int_distribution<size_t>{0, Fixed_bucket_size - 1}]() mutable
        {
            static_assert(Fixed_bucket_size > 0, "Zero bucket design is not allowed.");
            return dist(algo);
        };
    };

    auto robin_access() noexcept -> robin_access_pattern { return {*this}; };
    auto random_access() noexcept -> random_access_pattern { return {*this}; }

    inline constexpr static size_t N_way_concurrency = Fixed_bucket_size;

private:
    std::array<bucket_type, N_way_concurrency> map;
};

////////////////////////////////////////////////////////////////////// Task support

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

// More concurrency-friendly.
using intrusive_task_map = trivial_intrusive_map<intrusive_task_queue>;

// `internal::start_operation` is a customization point for `trivial_scheduler` template.
inline void start_operation(intrusive_task_queue *self, auto *operation) noexcept {
    self->push(operation);
}

////////////////////////////////////////////////////////////////////// io_uring async operations

// For (personal) debugging purpose, make it a different type from `intrusive_node`.
struct intrusive_node_with_meta {
    intrusive_node_with_meta *_i_m_next {nullptr};

#ifdef URING_EXEC_DEBUG
    void *_for_gdb {};
#endif
};

// External structured callbacks support.
// See io_uring_exec_operation.h and io_uring_exec_sender.h for more details.
struct io_uring_exec_operation_base: detail::immovable, intrusive_node_with_meta {
    using result_t = decltype(std::declval<io_uring_cqe>().res);
    using _self_t = io_uring_exec_operation_base;
    using vtable = detail::make_vtable<
                    detail::add_complete_to_vtable<void(_self_t*, result_t)>,
                    detail::add_cancel_to_vtable  <void(_self_t*)>,
                    detail::add_restart_to_vtable <void(_self_t*)>>;
    constexpr io_uring_exec_operation_base(vtable vtab) noexcept: vtab(vtab) {}
    vtable vtab;
    // We might need a bidirectional intrusive node for early stopping support.
    // However, this feature is rarely used in practice.
    // We can use hashmap instead to avoid wasting footprint for every object's memory layout.
};

using intrusive_operation_queue = detail::intrusive_queue<
                                    io_uring_exec_operation_base,
                                    &io_uring_exec_operation_base::_i_m_next>;

using intrusive_operation_map = trivial_intrusive_map<intrusive_operation_queue>;

////////////////////////////////////////////////////////////////////// stdexec scheduler template

// For `stdexec::scheduler` concept.
// We might have several scheduler implementations, so make a simple template for them.
template <typename Context>
struct trivial_scheduler {
    template <stdexec::receiver Receiver>
    struct operation: io_uring_exec_task {
        using operation_state_concept = stdexec::operation_state_t;

        void start() noexcept {
            // Private customization point.
            start_operation(context, this);
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
        Context *context;
    };
    struct sender {
        using sender_concept = stdexec::sender_t;
        using completion_signatures = stdexec::completion_signatures<
                                        stdexec::set_value_t(),
                                        stdexec::set_stopped_t()>;
        struct env {
            template <typename CPO>
            auto query(stdexec::get_completion_scheduler_t<CPO>) const noexcept {
                return trivial_scheduler{context};
            }
            Context *context;
        };

        env get_env() const noexcept { return {context}; }

        template <stdexec::receiver Receiver>
        operation<Receiver> connect(Receiver receiver) noexcept {
            return {{operation<Receiver>::this_vtable}, std::move(receiver), context};
        }

        Context *context;
    };
    bool operator<=>(const trivial_scheduler &) const=default;
    sender schedule() const noexcept { return {context}; }

    Context *context;
};

////////////////////////////////////////////////////////////////////// Local side

class io_uring_exec;

// Is-a runnable io_uring.
class io_uring_exec_local: public underlying_io_uring,
                           public io_uring_exec_run<io_uring_exec_local,
                                           io_uring_exec_operation_base>
{
public:
    io_uring_exec_local(constructor_parameters p,
                        io_uring_exec &root);

    ~io_uring_exec_local();

    using io_uring_exec_run::run_policy;
    using io_uring_exec_run::run;

    auto& get_remote() noexcept { return _root; }
    auto& get_local() noexcept { return *this; }

    auto& get_async_scope() noexcept { return _local_scope; }

    using scheduler = trivial_scheduler<io_uring_exec_local>;
    scheduler get_scheduler() noexcept { return {this}; }

    void add_inflight() noexcept { _inflight++; }
    void remove_inflight() noexcept { _inflight--; }

    decltype(auto) get_stopping_queue(io_uring_exec_operation_base *op) noexcept {
        return _stopping_map[op];
    }

// Hidden friends.
private:
    friend void start_operation(io_uring_exec_local *self, auto *operation) noexcept {
        self->_attached_queue.push(operation);
    }

    template <typename, typename>
    friend struct io_uring_exec_run;
    friend class io_uring_exec;

private:
    // This is a MPSC queue, while remote queue is a MPMC queue.
    // It can help scheduler attach to a specified thread (C).
    intrusive_task_queue _attached_queue;
    intrusive_operation_map _stopping_map;
    size_t _inflight {};
    exec::async_scope _local_scope;
    io_uring_exec &_root;
    std::thread::id _root_tid;
};

////////////////////////////////////////////////////////////////////// control block

class io_uring_exec: public underlying_io_uring, // For IORING_SETUP_ATTACH_WQ.
                     public io_uring_exec_run<io_uring_exec, io_uring_exec_operation_base>,
                     private detail::unified_stop_source<stdexec::inplace_stop_source>
{
public:
    // Example: io_uring_exec uring({.uring_entries=512});
    template <auto Unique = []{}> // For per-object-thread_local dispatch in compile time.
    io_uring_exec(underlying_io_uring::constructor_parameters params) noexcept
        : underlying_io_uring(params), _uring_params(params),
          _thread_id(std::this_thread::get_id()),
          _remote_handle{io_uring_exec_remote_handle::this_vtable<Unique>}
    {
        // Then it will broadcast to thread-local urings.
        params.ring_fd = this->ring_fd;
    }

    template <auto = []{}> // MUST declare.
    io_uring_exec(unsigned uring_entries, int uring_flags = 0) noexcept
        : io_uring_exec({.uring_entries = uring_entries, .uring_flags = uring_flags}) {}

    ~io_uring_exec() {
        // For on-stack uring.run().
        io_uring_exec_run::transfer_run();
        // Cancel all the pending tasks/operations.
        io_uring_exec_run::terminal_run();
    }

    // NOTE: Assumed that you're most likely using a singleton pattern. (e.g., async_main())
    // However, in some use cases, it can also be a per-object `get_local()`.
    //
    // Example 1 (Good):
    // io_uring_exec a {...};
    // io_uring_exec b {...};
    // assert(&a.get_local() != &b.get_local());
    //
    // Example 2 (Bad):
    // for(auto step : iota(1))
    //     io_uring_exec c {...};
    // assert(&c<in-step-1>.get_local() == &c<in-step-2>.get_local());
    //
    // Example 2 can be fixed by a more complex trick without runtime mapping.
    // Although the overhead is low (rebind `_root`), I don't plan to support it.
    auto get_local() noexcept -> io_uring_exec_local& { return _remote_handle.vtab.complete(*this); }
    auto get_remote() noexcept -> io_uring_exec& { return *this; }

    using task = internal::io_uring_exec_task;
    using operation_base = internal::io_uring_exec_operation_base;
    using task_queue = internal::intrusive_task_queue;
    using task_map = internal::intrusive_task_map;

    // Required by stdexec.
    // Most of its functions are invoked by stdexec.
    using scheduler = trivial_scheduler<io_uring_exec>;
    using local_scheduler = io_uring_exec_local::scheduler;

    auto get_scheduler() noexcept { return scheduler{this}; }
    auto get_local_scheduler() noexcept { return get_local().get_scheduler(); }
    auto get_local_scheduler(std::thread::id) = delete; // TODO

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

    auto get_async_scope() noexcept -> exec::async_scope& { return _transfer_scope; }

// Hidden friends.
private:
    template <typename, typename>
    friend struct io_uring_exec_run;
    friend class io_uring_exec_local;

    friend void start_operation(io_uring_exec *self, auto *operation) noexcept {
        // Break self-indexing rule as we don't need to find map bucket by operation itself.
        auto index = self->_store_balance.fetch_add(1, std::memory_order::relaxed);
        self->_immediate_map.robin_access()[index].push(operation);
    }

private:
    underlying_io_uring::constructor_parameters _uring_params;
    intrusive_task_map _immediate_map;
    alignas(64) std::atomic<size_t> _store_balance {};
    alignas(64) std::atomic<size_t> _running_local {};
    exec::async_scope _transfer_scope;
    std::thread::id _thread_id;

private:
    // This makes per-io_uring_exec.get_local() possible
    // in certain use cases. (NOT all use cases.)
    struct io_uring_exec_remote_handle {
        using vtable = detail::make_vtable<
                        detail::add_complete_to_vtable<
                            io_uring_exec_local&(io_uring_exec&)>>;
        const vtable vtab;

        template <auto>
        inline constexpr static vtable this_vtable = {
            {.complete = [](auto &self) noexcept -> io_uring_exec_local& {
                // Note that we use thread storage duration for `local`.
                // Therefore, we need <auto> to make them served by different types.
                thread_local io_uring_exec_local local(self._uring_params, self);
                return local;
            }}
        };
    } _remote_handle;

};

////////////////////////////////////////////////////////////////////// misc

inline
io_uring_exec_local::io_uring_exec_local(
    io_uring_exec_local::constructor_parameters p,
    io_uring_exec &root)
    : underlying_io_uring(p),
      _root(root),
      _root_tid(root._thread_id)
{
    _root._running_local.fetch_add(1, std::memory_order::relaxed);
}

// There may be a UB if `_root` is accessed unconditionally,
// since thread storage duration in main thread is longer than remote exec.
// (Other threads are fine to do so.)
// But no compiler can detect and reproduce this problem.
// In any case, we need a local tid to perform the check.
inline
io_uring_exec_local::~io_uring_exec_local() {
    io_uring_exec_run::transfer_run();
    thread_local auto tid = std::this_thread::get_id();
    if(tid == _root_tid) return;
    _root._running_local.fetch_sub(1, std::memory_order::acq_rel);
}

} // namespace internal
} // namespace uring_exec
