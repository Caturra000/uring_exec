#pragma once
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <cstring>
#include <chrono>
#include <mutex>
#include <thread>
#include <tuple>
#include <liburing.h>
#include <stdexec/execution.hpp>
#include "io_uring_exec.h"
#include "io_uring_exec_operation.h"

template <auto io_uring_prep_invocable, typename ...Args>
struct io_uring_exec_sender {
    using sender_concept = stdexec::sender_t;
    using completion_signatures = stdexec::completion_signatures<
                                    stdexec::set_value_t(io_uring_exec_operation_base::result_t),
                                    stdexec::set_error_t(std::exception_ptr),
                                    stdexec::set_stopped_t()>;

    template <stdexec::receiver Receiver>
    io_uring_exec_operation<io_uring_prep_invocable, Receiver, Args...>
    connect(Receiver receiver) noexcept {
        return {std::move(receiver), uring, std::move(args)};
    }

    io_uring_exec *uring;
    std::tuple<Args...> args;
};

template <auto io_uring_prep_invocable, typename ...Args>
inline stdexec::sender_of<
    stdexec::set_value_t(io_uring_exec_operation_base::result_t /* cqe->res */),
    stdexec::set_error_t(std::exception_ptr)>
auto make_uring_sender(std::in_place_t,
                       io_uring_exec::scheduler s, std::tuple<Args...> &&t_args) noexcept {
    return io_uring_exec_sender<io_uring_prep_invocable, Args...>{s.uring, std::move(t_args)};
}

// A sender factory.
template <auto io_uring_prep_invocable>
inline auto make_uring_sender(io_uring_exec::scheduler s, auto &&...args) noexcept {
    return make_uring_sender<io_uring_prep_invocable>
        (std::in_place, s, std::tuple(static_cast<decltype(args)&&>(args)...));
}

// On  files  that  support seeking, if the `offset` is set to -1, the read operation commences at the file offset,
// and the file offset is incremented by the number of bytes read. See read(2) for more details. Note that for an
// async API, reading and updating the current file offset may result in unpredictable behavior, unless access to
// the file is serialized. It is **not encouraged** to use this feature, if it's possible to provide the  desired  IO
// offset from the application or library.
inline stdexec::sender
auto async_read(io_uring_exec::scheduler s, int fd, void *buf, size_t n, uint64_t offset = 0) noexcept {
    return make_uring_sender<io_uring_prep_read>(s, fd, buf, n, offset);
}

inline stdexec::sender
auto async_write(io_uring_exec::scheduler s, int fd, const void *buf, size_t n, uint64_t offset = 0) noexcept {
    return make_uring_sender<io_uring_prep_write>(s, fd, buf, n, offset);
}

inline stdexec::sender
auto async_close(io_uring_exec::scheduler s, int fd) noexcept {
    return make_uring_sender<io_uring_prep_close>(s, fd);
}

inline stdexec::sender
auto async_accept(io_uring_exec::scheduler s,int fd, int flags) noexcept {
    return make_uring_sender<io_uring_prep_accept>(s, fd, nullptr, nullptr, flags);
}

inline stdexec::sender
auto async_wait(io_uring_exec::scheduler s, std::chrono::milliseconds duration) noexcept {
    using namespace std::chrono;
    auto duration_s = duration_cast<seconds>(duration);
    auto duration_ns = duration_cast<nanoseconds>(duration - duration_s);
    return
        // `ts` needs safe lifetime within an asynchronous scope.
        stdexec::just(__kernel_timespec {
            .tv_sec = duration_s.count(),
            .tv_nsec = duration_ns.count()
        })
      | stdexec::let_value([s](auto &&ts) {
            return make_uring_sender<io_uring_prep_timeout>(std::in_place, s,
                []<typename R, typename ...Ts>(R(io_uring_sqe*, Ts...), auto &&...args) {
                    return std::tuple<Ts...>{static_cast<decltype(args)&&>(args)...};
                }(io_uring_prep_timeout, &ts, 0, 0));
        });
}

// Debug only.
// The return value makes no sense at all.
inline stdexec::sender
auto async_nop(io_uring_exec::scheduler s, ...) noexcept {
    return make_uring_sender<io_uring_prep_nop>(s);
}
