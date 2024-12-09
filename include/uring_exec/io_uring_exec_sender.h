#pragma once
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/signalfd.h>
#include <netinet/in.h>
#include <signal.h>
#include <poll.h>
#include <cstring>
#include <chrono>
#include <mutex>
#include <thread>
#include <tuple>
#include <ranges>
#include <liburing.h>
#include <stdexec/execution.hpp>
#include "io_uring_exec.h"
#include "io_uring_exec_operation.h"
namespace uring_exec {

template <auto io_uring_prep_invocable, typename ...Args>
struct io_uring_exec_sender {
    using sender_concept = stdexec::sender_t;
    using completion_signatures = stdexec::completion_signatures<
                                    stdexec::set_value_t(io_uring_exec::operation_base::result_t),
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

////////////////////////////////////////////////////////////////////// Make uring sender

// Make it easier to create user-defined senders.
template <auto io_uring_prep_invocable>
class make_uring_sender_t {
public:
    constexpr
    stdexec::sender_of<
        stdexec::set_value_t(io_uring_exec::operation_base::result_t /* cqe->res */),
        stdexec::set_error_t(std::exception_ptr)>
    auto operator()(io_uring_exec::scheduler s, auto &&...args) const noexcept {
        return operator()(std::in_place, s,
            // Perfectly match the signature. (Arguments are convertible.)
            []<typename R, typename ...Ts>(R(io_uring_sqe*, Ts...), auto &&...args) {
                return std::tuple<Ts...>{static_cast<decltype(args)&&>(args)...};
            }(io_uring_prep_invocable, static_cast<decltype(args)&&>(args)...));
    }

private:
    template <typename ...Args>
    constexpr auto operator()(std::in_place_t,
                              io_uring_exec::scheduler s, std::tuple<Args...> &&t_args)
    const noexcept -> io_uring_exec_sender<io_uring_prep_invocable, Args...> {
        return {s.context, std::move(t_args)};
    }
};

// A [sender factory] factory for `io_uring_prep_*` asynchronous functions.
//
// These asynchronous senders have similar interfaces to `io_uring_prep_*` functions.
// That is, io_uring_prep_*(sqe, ...) -> async_*(scheduler, ...)
//
// Usage example:
//     auto async_close = make_uring_sender_v<io_uring_prep_close>;
//     stdexec::sender auto s = async_close(scheduler, fd);
template <auto io_uring_prep_invocable>
inline constexpr auto make_uring_sender_v = make_uring_sender_t<io_uring_prep_invocable>{};

////////////////////////////////////////////////////////////////////// Asynchronous senders

inline constexpr auto async_oepnat      = make_uring_sender_v<io_uring_prep_openat>;
inline constexpr auto async_readv       = make_uring_sender_v<io_uring_prep_readv>;
inline constexpr auto async_readv2      = make_uring_sender_v<io_uring_prep_readv2>;
inline constexpr auto async_writev      = make_uring_sender_v<io_uring_prep_writev>;
inline constexpr auto async_writev2     = make_uring_sender_v<io_uring_prep_writev2>;
inline constexpr auto async_close       = make_uring_sender_v<io_uring_prep_close>;
inline constexpr auto async_socket      = make_uring_sender_v<io_uring_prep_socket>;
inline constexpr auto async_bind        = make_uring_sender_v<io_uring_prep_bind>;
inline constexpr auto async_accept      = make_uring_sender_v<io_uring_prep_accept>;
inline constexpr auto async_connect     = make_uring_sender_v<io_uring_prep_connect>;
inline constexpr auto async_send        = make_uring_sender_v<io_uring_prep_send>;
inline constexpr auto async_recv        = make_uring_sender_v<io_uring_prep_recv>;
inline constexpr auto async_sendmsg     = make_uring_sender_v<io_uring_prep_sendmsg>;
inline constexpr auto async_recvmsg     = make_uring_sender_v<io_uring_prep_recvmsg>;
inline constexpr auto async_shutdown    = make_uring_sender_v<io_uring_prep_shutdown>;
inline constexpr auto async_poll_add    = make_uring_sender_v<io_uring_prep_poll_add>;
inline constexpr auto async_poll_update = make_uring_sender_v<io_uring_prep_poll_update>;
inline constexpr auto async_poll_remove = make_uring_sender_v<io_uring_prep_poll_remove>;
inline constexpr auto async_timeout     = make_uring_sender_v<io_uring_prep_timeout>;
inline constexpr auto async_futex_wait  = make_uring_sender_v<io_uring_prep_futex_wait>;
inline constexpr auto async_futex_wake  = make_uring_sender_v<io_uring_prep_futex_wake>;

// Debug only. (For example, to verify the correctness of concurrency.)
// The return value makes no sense.
inline constexpr auto async_nop         = make_uring_sender_v<io_uring_prep_nop>;

// `async_open` needs a new version of liburing: https://github.com/axboe/liburing/issues/1100
// inline constexpr auto async_open        = make_uring_sender_v<io_uring_prep_open>;

// On  files  that  support seeking, if the `offset` is set to -1, the read operation commences at the file offset,
// and the file offset is incremented by the number of bytes read. See read(2) for more details. Note that for an
// async API, reading and updating the current file offset may result in unpredictable behavior, unless access to
// the file is serialized. It is **not encouraged** to use this feature, if it's possible to provide the  desired  IO
// offset from the application or library.
inline constexpr stdexec::sender
auto async_read(io_uring_exec::scheduler s, int fd, void *buf, size_t n, uint64_t offset = 0) noexcept {
    return make_uring_sender_v<io_uring_prep_read>(s, fd, buf, n, offset);
}

inline constexpr stdexec::sender
auto async_write(io_uring_exec::scheduler s, int fd, const void *buf, size_t n, uint64_t offset = 0) noexcept {
    return make_uring_sender_v<io_uring_prep_write>(s, fd, buf, n, offset);
}

inline stdexec::sender
auto async_wait(io_uring_exec::scheduler s, std::chrono::steady_clock::duration duration) noexcept {
    using namespace std::chrono;
    auto make_ts_from = [](auto duration) -> struct __kernel_timespec {
        auto duration_s = duration_cast<seconds>(duration);
        auto duration_ns = duration_cast<nanoseconds>(duration - duration_s);
        return {.tv_sec = duration_s.count(), .tv_nsec = duration_ns.count()};
    };
    // `ts` needs safe lifetime within an asynchronous scope.
    return stdexec::let_value(stdexec::just(make_ts_from(duration)), [s](auto &&ts) {
        return make_uring_sender_v<io_uring_prep_timeout>(s, &ts, 0, 0);
    });
}

inline stdexec::sender
auto async_sigwait(io_uring_exec::scheduler scheduler, std::ranges::range auto signals) noexcept {
    struct raii_fd {
        int fd;
        explicit raii_fd(int fd) noexcept: fd(fd) {}
        raii_fd(raii_fd &&rhs) noexcept: fd(rhs.reset()) {}
        ~raii_fd() { if(fd > -1) close(fd); }
        int reset() noexcept { return std::exchange(fd, -1); }
    };
    auto make_signalfd_from = [](auto signals) {
        sigset_t mask;
        sigemptyset(&mask);
        for(auto signal : signals) sigaddset(&mask, signal);
        pthread_sigmask(SIG_BLOCK, &mask, nullptr);
        return raii_fd{signalfd(-1, &mask, 0)};
    };
    return
        stdexec::let_value(stdexec::just(make_signalfd_from(std::move(signals))), [=](auto &fd) {
            // Propagate to upon_error/let_error.
            if(fd.fd < 0) throw std::system_error(errno, std::system_category());
            return
                stdexec::let_value(stdexec::just(fd.fd), [=](auto fd) {
                    // NOTE:
                    // Since signalfd has no ownership of signal, async_poll_add & async_close are incorrect.
                    // We must async_read the signal. Otherwise signal will wakeup another signalfd.
                    return async_poll_add(scheduler, fd, POLLIN);
                })
              | stdexec::let_value([=](auto events) {
                    // FIXME: POLLERR differs from cqe_res < 0. But what is the undocumented difference?
                    if(!(events & POLLIN)) throw std::system_error(errno, std::system_category());
                    return stdexec::just();
                })
              | stdexec::let_value([=, &fd] {
                    return
                        stdexec::just(fd.fd, std::array<std::byte, sizeof(signalfd_siginfo)>())
                      | stdexec::let_value([=](auto fd, auto &buf) {
                            return
                                async_read(scheduler, fd, buf.data(), buf.size())
                              | stdexec::then([&buf](auto) {
                                    // See `man 2 signalfd` for more details.
                                    return std::bit_cast<signalfd_siginfo>(buf);
                                });
                        });
                });
        });
}

} // namespace uring_exec
