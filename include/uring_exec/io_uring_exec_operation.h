#pragma once
#include <utility>
#include <tuple>
#include <atomic>
#include <stdexec/execution.hpp>
#include "io_uring_exec.h"
namespace uring_exec {

namespace hidden {

struct nop_io_uring_exec_operation: io_uring_exec::operation_base {
    // NOT a real operation state in stdexec.
    constexpr nop_io_uring_exec_operation() noexcept
        : io_uring_exec::operation_base(this_vtable) {}
    inline constexpr static vtable this_vtable {
        {.complete = [](auto, auto) noexcept {}},
        {.cancel   = [](auto) noexcept {}},
        {.restart  = [](auto) noexcept {}},
    };
};

// TODO: constexpr.
inline constinit static nop_io_uring_exec_operation noop;

} // namespace hidden

template <auto F, stdexec::receiver Receiver, typename ...Args>
struct io_uring_exec_operation: io_uring_exec::operation_base {
    using operation_state_concept = stdexec::operation_state_t;
    using stop_token_type = stdexec::stop_token_of_t<stdexec::env_of_t<Receiver>>;
    // using stop_callback_type = ...; // See below.

    io_uring_exec_operation(Receiver receiver,
                            io_uring_exec *uring_control,
                            std::tuple<Args...> args) noexcept
        : io_uring_exec::operation_base(this_vtable),
          receiver(std::move(receiver)),
          uring_control(uring_control),
          args(std::move(args)) {}

    void start() noexcept {
        if(stop_requested()) [[unlikely]] {
            stdexec::set_stopped(std::move(receiver));
            return;
        }
        // NOTE: Don't store the thread_local value to a class member.
        // It might be transferred to a different thread.
        // See restart()/exec_run::transfer_run() for more details.
        auto &local = uring_control->get_local();
        if(auto sqe = io_uring_get_sqe(&local)) [[likely]] {
            using op_base = io_uring_exec::operation_base;
            local.add_inflight();
            if(false /* temporarily disabled && have_set_stopped() */) {
                // Changed to a static noop.
                io_uring_sqe_set_data(sqe, static_cast<op_base*>(&hidden::noop));
                io_uring_prep_nop(sqe);
            } else {
                io_uring_sqe_set_data(sqe, static_cast<op_base*>(this));
                std::apply(F, std::tuple_cat(std::tuple(sqe), std::move(args)));
                install_stoppable_callback(local);
            }
        } else {
            // The SQ ring is currently full.
            //
            // One way to solve this is to make an inline submission here.
            // But I don't want to submit operations at separate execution points.
            //
            // Another solution is to make it never fails by deferred processing.
            // TODO: We might need some customization points for minor operations (cancel).
            async_restart();
        }
    }

    // Since operations are stable (until stdexec::set_xxx(receiver)),
    // we can restart again.
    void async_restart() noexcept try {
        // It can be the same thread, or other threads.
        stdexec::scheduler auto scheduler = uring_control->get_scheduler();
        stdexec::sender auto restart_sender =
            stdexec::schedule(scheduler)
          | stdexec::then([self = this] {
                self->start();
            });
        uring_control->get_async_scope().spawn(std::move(restart_sender));
    } catch(...) {
        // exec::async_scope.spawn() is throwable.
        stdexec::set_error(std::move(receiver), std::current_exception());
    }

    bool stop_requested() noexcept {
        // Don't use stdexec::stoppable_token, it has BUG on g++/nvc++.
        if constexpr (not stdexec::unstoppable_token<stop_token_type>) {
            auto stop_token = stdexec::get_stop_token(stdexec::get_env(receiver));
            return stop_token.stop_requested();
        }
        return false;
    }

    void install_stoppable_callback(auto &local) noexcept {
        if constexpr (not stdexec::unstoppable_token<stop_token_type>) {
            local_scheduler = local.get_scheduler();
            auto stop_token = stdexec::get_stop_token(stdexec::get_env(receiver));
            stop_callback.emplace(std::move(stop_token), cancellation{this});
        }
    }

    // Called by the local thread.
    void uninstall_stoppable_callback() noexcept {
        if constexpr (not stdexec::unstoppable_token<stop_token_type>) {
            stop_callback.reset();
            auto &q = local_scheduler.context->get_stopping_queue(this);
            auto op = q.move_all();
            if(!op) return;
            // See the comment in `io_uring_exec_operation_base`.
            q.push_all(op, [this](auto node) { return node && node != this; });
            q.push_all(q.next(this), [](auto node) { return node; });
        }
    }

    struct cancellation {
        // May be called by the requesting thread.
        // So we need an atomic operation.
        void operator()() noexcept {
            auto local = _self->local_scheduler.context;
            auto &q = local->get_stopping_queue(_self);
            // `q` and `self` are stable.
            q.push(_self);
        }
        io_uring_exec_operation *_self;
    };

    // For stdexec.
    using stop_callback_type = typename stop_token_type::template callback_type<cancellation>;

    inline constexpr static vtable this_vtable {
        {.complete = [](auto *_self, result_t cqe_res) noexcept {
            auto self = static_cast<io_uring_exec_operation*>(_self);
            auto &receiver = self->receiver;

            self->uninstall_stoppable_callback();

            constexpr auto is_timer = [] {
                // Make GCC happy.
                if constexpr (requires { F == &io_uring_prep_timeout; })
                    return F == &io_uring_prep_timeout;
                return false;
            } ();

            // Zero overhead for regular operations.
            if constexpr (is_timer) {
                auto good = [cqe_res](auto ...errors) { return ((cqe_res == errors) || ...); };
                // Timed out is not an error.
                if(good(-ETIME, -ETIMEDOUT)) [[likely]] {
                    stdexec::set_value(std::move(receiver), cqe_res);
                    return;
                }
            }

            if(cqe_res >= 0) [[likely]] {
                stdexec::set_value(std::move(receiver), cqe_res);
            } else if(cqe_res == -ECANCELED) {
                stdexec::set_stopped(std::move(receiver));
            } else {
                auto error = std::make_exception_ptr(
                            std::system_error(-cqe_res, std::system_category()));
                stdexec::set_error(std::move(receiver), std::move(error));
            }
        }},

        {.cancel = [](auto *_self) noexcept {
            auto self = static_cast<io_uring_exec_operation*>(_self);
            self->uninstall_stoppable_callback();
            stdexec::set_stopped(std::move(self->receiver));
        }},

        {.restart = [](auto *_self) noexcept {
            auto self = static_cast<io_uring_exec_operation*>(_self);
            self->uninstall_stoppable_callback();
            self->async_restart();
        }}
    };

    Receiver receiver;
    io_uring_exec *uring_control;
    std::tuple<Args...> args;

    io_uring_exec::local_scheduler local_scheduler;
    std::optional<stop_callback_type> stop_callback;
};

} // namespace uring_exec
