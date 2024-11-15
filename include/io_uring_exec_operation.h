#pragma once
#include <utility>
#include <tuple>
#include <atomic>
#include <stdexec/execution.hpp>
#include "io_uring_exec.h"

using io_uring_exec_operation_base = io_uring_exec::io_uring_exec_operation_base;

template <auto F, stdexec::receiver Receiver, typename ...Args>
struct io_uring_exec_operation: io_uring_exec_operation_base {
    using operation_state_concept = stdexec::operation_state_t;
    io_uring_exec_operation(Receiver receiver,
                            io_uring_exec *uring,
                            std::tuple<Args...> args) noexcept
        : io_uring_exec_operation_base{{}, this_vtable},
          receiver(std::move(receiver)),
          uring(uring),
          args(std::move(args)) {}

    void start() noexcept {
        auto guard = uring->_submit_lock.fastpath_guard(*this);
        if(auto sqe = io_uring_get_sqe(&uring->_underlying_uring)) [[likely]] {
            using op_base = io_uring_exec_operation_base;
            io_uring_sqe_set_data(sqe, static_cast<op_base*>(this));
            std::apply(F, std::tuple_cat(std::tuple(sqe), std::move(args)));
            uring->_inflight.fetch_add(1, std::memory_order::relaxed);
        } else {
            guard.unlock();
            // RETURN VALUE
            // io_uring_get_sqe(3)  returns  a  pointer  to the next submission
            // queue event on success and NULL on failure. If NULL is returned,
            // the SQ ring is currently full and entries must be submitted  for
            // processing before new ones can get allocated.
            auto error = std::make_exception_ptr(
                        std::system_error(EBUSY, std::system_category()));
            stdexec::set_error(std::move(receiver), std::move(error));
        }
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
                auto error = std::make_exception_ptr(
                            std::system_error(-cqe_res, std::system_category()));
                stdexec::set_error(std::move(self->receiver), std::move(error));
            }
        }},
        {.cancel = [](auto *_self) noexcept {
            auto self = static_cast<io_uring_exec_operation*>(_self);
            stdexec::set_stopped(std::move(self->receiver));
        }}
    };

    Receiver receiver;
    io_uring_exec *uring;
    std::tuple<Args...> args;
};
