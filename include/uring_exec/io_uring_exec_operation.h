#pragma once
#include <utility>
#include <tuple>
#include <atomic>
#include <stdexec/execution.hpp>
#include "io_uring_exec.h"
namespace uring_exec {

template <auto F, stdexec::receiver Receiver, typename ...Args>
struct io_uring_exec_operation: io_uring_exec::operation_base {
    using operation_state_concept = stdexec::operation_state_t;
    io_uring_exec_operation(Receiver receiver,
                            io_uring_exec *uring_control,
                            std::tuple<Args...> args) noexcept
        : io_uring_exec::operation_base{{}, this_vtable},
          receiver(std::move(receiver)),
          uring_control(uring_control),
          args(std::move(args)) {}

    void start() noexcept {
        // NOTE: Don't store the thread_local value to a class member.
        // It might be transferred to a different thread.
        // See restart()/exec_run::transfer_run() for more details.
        auto &local = uring_control->get_local();
        if(auto sqe = io_uring_get_sqe(&local)) {
            using op_base = io_uring_exec::operation_base;
            io_uring_sqe_set_data(sqe, static_cast<op_base*>(this));
            std::apply(F, std::tuple_cat(std::tuple(sqe), std::move(args)));
            local._inflight++;
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
    void async_restart() {
        // It can be the same thread, or other threads.
        stdexec::scheduler auto scheduler = uring_control->get_scheduler();
        stdexec::sender auto restart_sender =
            stdexec::schedule(scheduler)
          | stdexec::then([self = this] {
                self->start();
            });
        uring_control->_transfer_scope.spawn(std::move(restart_sender));
    }

    inline constexpr static vtable this_vtable {
        {.complete = [](auto *_self, result_t cqe_res) noexcept {
            auto self = static_cast<io_uring_exec_operation*>(_self);

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
                    stdexec::set_value(std::move(self->receiver), cqe_res);
                    return;
                }
            }

            if(cqe_res >= 0) [[likely]] {
                stdexec::set_value(std::move(self->receiver), cqe_res);
            } else if(cqe_res == -ECANCELED) {
                stdexec::set_stopped(std::move(self->receiver));
            } else {
            #ifdef __clang__
                // FIXME: set_error() generates ud2 instructions. wtf?
                stdexec::set_stopped(std::move(self->receiver));
            #else
                auto error = std::make_exception_ptr(
                            std::system_error(-cqe_res, std::system_category()));
                stdexec::set_error(std::move(self->receiver), std::move(error));
            #endif
            }
        }},

        {.cancel = [](auto *_self) noexcept {
            auto self = static_cast<io_uring_exec_operation*>(_self);
            stdexec::set_stopped(std::move(self->receiver));
        }},

        {.restart = [](auto *_self) noexcept {
            auto self = static_cast<io_uring_exec_operation*>(_self);
            self->async_restart();
        }}
    };

    Receiver receiver;
    io_uring_exec *uring_control;
    std::tuple<Args...> args;
};

} // namespace uring_exec
