#pragma once
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <type_traits>
#include <bit>
#include <array>
#include <atomic>

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
    // Make STL happy.
    auto dummy = reinterpret_cast<void*>(0x1);
    return std::unique_ptr<void, decltype(func)>{dummy, std::move(func)};
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
