#pragma once
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <type_traits>
#include <bit>
#include <array>
#include <atomic>
#include <ranges>
namespace uring_exec {
inline namespace utils {

// C-style check for syscall.
// inline void check(int cond, const char *reason) {
//     if(!cond) [[likely]] return;
//     perror(reason);
//     abort();
// }

// C++-style check for syscall.
// Failed on ret < 0 by default.
//
// INT_ASYNC_CHECK: helper for liburing (-ERRNO) and other syscalls (-1).
// It may break generic programming (forced to int).
template <typename Comp = std::less<int>, auto V = 0, bool INT_ASYNC_CHECK = true>
struct nofail {
    std::string_view reason;

    // Examples:
    // fstat(...) | nofail("fstat");        // Forget the if-statement and ret!
    // int fd = open(...) | nofail("open"); // If actually need a ret, here you are!
    friend decltype(auto) operator|(auto &&ret, nofail nf) {
        if(Comp{}(ret, V)) [[unlikely]] {
            // Hack errno.
            if constexpr (INT_ASYNC_CHECK) {
                using T = std::decay_t<decltype(ret)>;
                static_assert(std::is_convertible_v<T, int>);
                // -ERRNO
                if(ret != -1) errno = -ret;
            }
            perror(nf.reason.data());
            std::terminate();
        }
        return std::forward<decltype(ret)>(ret);
    };
};

// Make clang happy.
nofail(...) -> nofail<std::less<int>, 0, true>;

// Go-style, move-safe defer.
[[nodiscard("defer() is not allowed to be temporary.")]]
inline auto defer(auto func) {
    auto _0x1 = std::uintptr_t {0x1};
    // reinterpret_cast is not a constexpr.
    auto make_STL_happy = reinterpret_cast<void*>(_0x1);
    auto make_dtor_happy = [f = std::move(func)](...) { f(); };
    using Defer = std::unique_ptr<void, decltype(make_dtor_happy)>;
    return Defer{make_STL_happy, std::move(make_dtor_happy)};
}

// For make_server().
struct make_server_option_t {
    int port {8848};
    int backlog {128};
    bool nonblock {false};
    bool reuseaddr {true};
    bool reuseport {false};
};

// Do some boring stuff and return a server fd.
inline int make_server(make_server_option_t option) {
    int socket_flag = option.nonblock ? SOCK_NONBLOCK : 0;
    int socket_fd = socket(AF_INET, SOCK_STREAM, socket_flag) | nofail("socket");

    auto setsock = [enable = 1, fd = socket_fd](int optname) {
        setsockopt(fd, SOL_SOCKET, optname, &enable, sizeof(int)) | nofail("setsockopt");
    };
    if(option.reuseaddr) setsock(SO_REUSEADDR);
    if(option.reuseport) setsock(SO_REUSEPORT);

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(option.port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    auto no_alias_addr = reinterpret_cast<const sockaddr*>(&addr);

    bind(socket_fd, no_alias_addr, sizeof(addr)) | nofail("bind");

    listen(socket_fd, option.backlog) | nofail("listen");

    return socket_fd;
}

// For signal_blocker(). Inclusive is default mode.
// Example:
//     // Block all signals except SIGINT and SIGUSR1.
//     auto sb = signal_blocker<sigmask_exclusive>(std::array {SIGINT, SIGUSR1});
struct sigmask_exclusive_t: std::true_type {};
struct sigmask_inclusive_t: std::false_type {};
inline constexpr auto sigmask_exclusive = sigmask_exclusive_t{};
inline constexpr auto sigmask_inclusive = sigmask_inclusive_t{};

// Block all/some signals in ctor.
// Restore previous signals in dtor (or .reset()).
// Examples:
//     // Block all signals.
//     auto sb = signal_blocker();
//
//     // Block a single SIGINT signal.
//     auto sb = signal_blocker(SIGINT);
//
//     // Block SIGINT and SIGUSR1 singls. (std::vector<int> or other ranges are also acceptable.)
//     auto sb = signal_blocker(std::array {SIGINT, SIGUSR1});
//
//     // Block all signals except SIGINT and SIGUSR1.
//     auto sb = signal_blocker<sigmask_exclusive>(std::array {SIGINT, SIGUSR1});
template <auto exclusive = std::false_type { /* using underlying type to clarify default behavior. */ },
          typename Container = std::array<int, 0>> // Can be a range, or a single signal value type.
inline auto signal_blocker(Container &&signals = {}) {
    /// ctor
    sigset_t new_mask, old_mask;
    auto empty_signals_f = [&] {
        if constexpr (std::ranges::range<Container>) return std::size(signals) == 0;
        else return false;
    };
    bool init_fill = empty_signals_f() || exclusive;
    (init_fill ? sigfillset : sigemptyset)(&new_mask);
    auto modify = (exclusive ? sigdelset : sigaddset);
    if constexpr (std::ranges::range<Container>) {
        for(auto signal : signals) modify(&new_mask, signal);
    } else {
        modify(&new_mask, signals);
    }
    sigemptyset(&old_mask);
    // The use of sigprocmask() is unspecified in a multithreaded process; see pthread_sigmask(3).
    pthread_sigmask(SIG_BLOCK, &new_mask, &old_mask);

    /// dtor
    return defer([old_mask] {
        pthread_sigmask(SIG_SETMASK, &old_mask, 0);
    });
}

} // namespace utils
} // namespace uring_exec
