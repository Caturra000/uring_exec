#pragma once
#include <utility>
#include <tuple>
#include <atomic>
#include <optional>
#include <stdexec/execution.hpp>
#include "io_uring_exec.h"
namespace uring_exec {

namespace hidden {
struct nop_io_uring_exec_operation: io_uring_exec::operation_base {
    // NOT a real operation state in stdexec.
    constexpr nop_io_uring_exec_operation() noexcept
        : io_uring_exec::operation_base({}, this_vtable) {}
    inline constexpr static vtable this_vtable {
        {.complete = [](auto, auto) noexcept {}},
        {.cancel   = [](auto) noexcept {}},
        {.restart  = [](auto) noexcept {}},
    };
};
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
            auto stop_token = stdexec::get_stop_token(stdexec::get_env(receiver));
            if(stop_token.stop_requested()) {
                // Changed to a static noop.
                io_uring_sqe_set_data(sqe, static_cast<op_base*>(&hidden::noop));
                stdexec::set_stopped(std::move(receiver));
                return;
            }
            // For early stopping only.
            list_head = &local._attached_queue;
            stop_callback.emplace(std::move(stop_token), early_stopping{this});
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
        uring_control->_transfer_scope.spawn(std::move(restart_sender));
    } catch(...) {
        // exec::scope.spawn() is throwable.
        stdexec::set_error(std::move(receiver), std::current_exception());
    }

    inline constexpr static vtable this_vtable {
        {.complete = [](auto *_self, result_t cqe_res) noexcept {
            auto self = static_cast<io_uring_exec_operation*>(_self);
            auto &receiver = self->receiver;

            self->stop_callback.reset();
            // Callback may be executing (started before reset). So we need a check.
            // FIXME:
            // Rougly read the source code of libstdc++,
            // it seems that ~callback will synchronize with callback.operator()? (Not sure.)
            constexpr auto mo = std::memory_order::acq_rel;
            while(true) {
                bool _false = false;
                if(self->owned.compare_exchange_weak(_false, true, mo)) [[likely]] {
                    break;
                }
                std::this_thread::yield();
            }

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
            stdexec::set_stopped(std::move(self->receiver));
        }},

        {.restart = [](auto *_self) noexcept {
            auto self = static_cast<io_uring_exec_operation*>(_self);
            self->async_restart();
        }}
    };

    struct early_stopping {
        // Try to send a cancel request to io_uring.
        // NOTES:
        // 1. This function may be called from the requesting side.
        //   For example:
        //   #main    is on thread: 129149252081920
        //   #sender1 is on thread: 129149219882688
        //   #sender2 is on thread: 129149245060800
        //   =====
        //   #main: sync_wait(when_any(#sender1, #sender2))
        //   =====
        //   #sender1 completes and requests #sender2 to stop working.
        //   operator()(): 129149219882688
        //   async_stop(): 129149245060800
        // 2. On policy.transfer mode, this function makes no effect.
        void operator()() const noexcept {
            constexpr auto mo = std::memory_order::acq_rel;
            bool _false = false;
            auto self = _self;
            // It is only accessible before runloop resets its callback.
            // (Started before reset.)
            // NOTE: Spurious failure is not allowed here. Use strong version.
            if(!self->owned.compare_exchange_strong(_false, true, mo)) {
                // Runloop wins. It is completing with result.
                return;
            }

            // Callback wins. Send the request and synchronize with runloop.
            auto uring = self->uring_control;
            auto q = self->list_head;
            assert(q);
            // Attach to the same local runloop.
            stdexec::scheduler auto scheduler = uring->get_scheduler();
            stdexec::scheduler auto attached = uring->bind(scheduler, *q);
            stdexec::sender auto stop_sender =
                stdexec::schedule(attached)
              | stdexec::then([self] { async_stop(self); });
            uring->_transfer_scope.spawn(std::move(stop_sender));
            self->owned.store(false, mo);
        }

        static void async_stop(io_uring_exec_operation *self) {
            auto &local = self->uring_control->get_local();
            if(auto sqe = io_uring_get_sqe(&local)) {
                using op_base = io_uring_exec::operation_base;
                // hidden::noop is a static operation.
                io_uring_sqe_set_data(sqe, static_cast<op_base*>(&hidden::noop));
                // `self` operation is unique in urings.
                io_uring_prep_cancel(sqe, static_cast<op_base*>(self), {});
                // Then `self` operation will complete(), and cqe->res = -ECANCELED.
                // `noop` operation will also complete(), but do nothing.
                local._inflight++;
            } else {
                // Full. Ignore stop request.
            }
        }
        io_uring_exec_operation *_self;
    };
    using stop_callback_type = typename stop_token_type::template callback_type<early_stopping>;

    Receiver receiver;
    io_uring_exec *uring_control;
    std::tuple<Args...> args;
    io_uring_exec::task_queue *list_head {};
    std::atomic<bool> owned {false};
    bool stop_requested {false};
    std::optional<stop_callback_type> stop_callback;
};

} // namespace uring_exec
