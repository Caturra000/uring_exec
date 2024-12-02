#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <iostream>
#include <ranges>
#include <format>
#include <exec/repeat_n.hpp>
#include <exec/repeat_effect_until.hpp>
#include "uring_exec.hpp"

using uring_exec::io_uring_exec;
constexpr auto noop = [](auto &&...) {};

stdexec::sender
auto pong(io_uring_exec::scheduler scheduler, int client_fd, int blocksize) {
    return
        stdexec::just(std::vector<char>(blocksize))
      | stdexec::let_value([=](auto &buf) {
            return
                uring_exec::async_read(scheduler, client_fd, buf.data(), buf.size())
              | stdexec::let_value([=, &buf](int read_bytes) {
                    return uring_exec::async_write(scheduler, client_fd, buf.data(), read_bytes);
                })
              | stdexec::let_value([=](int written_bytes) {
                    return stdexec::just(written_bytes == 0);
                })
              | exec::repeat_effect_until();
        })
      | stdexec::upon_error(noop)
      | stdexec::let_value([=] {
            return
                uring_exec::async_close(scheduler, client_fd);
        })
      | stdexec::then(noop);
}

stdexec::sender
auto server(io_uring_exec::scheduler scheduler, exec::async_scope &scope,
            int server_fd, int blocksize, int sessions) {
    return
        stdexec::just()
      | stdexec::let_value([=, &scope] {
            return
                uring_exec::async_accept(scheduler, server_fd, nullptr, nullptr, 0)
              | stdexec::then([=, &scope](int client_fd) mutable {
                    scope.spawn(pong(scheduler, client_fd, blocksize));
                })
              | exec::repeat_n(sessions);
        })
      | stdexec::upon_error(noop);
}

int main(int argc, char *argv[]) {
    if(argc <= 4) {
        auto message = std::format(
            "usage: {} <port> <threads> <blocksize> <sessions>", argv[0]);
        std::cerr << message << std::endl;
        return -1;
    }
    auto atoies = [&](auto ...idxes) { return std::tuple{atoi(argv[idxes])...}; };
    auto [port, threads, blocksize, sessions] = atoies(1, 2, 3, 4);

    auto sb = uring_exec::signal_blocker<uring_exec::sigmask_exclusive>(SIGINT);
    auto server_fd = uring_exec::make_server({.port=port});
    io_uring_exec uring({.uring_entries=512});
    exec::async_scope scope;

    std::vector<std::jthread> thread_pool(threads);
    for(auto &&j : thread_pool) {
        j = std::jthread([&](auto stop_token) { uring.run(stop_token); });
    }

    stdexec::scheduler auto scheduler = uring.get_scheduler();
    stdexec::sender auto s = stdexec::starts_on(scheduler,
        server(scheduler, scope, server_fd, blocksize, sessions));
    stdexec::sender auto f = stdexec::starts_on(scheduler,
        uring_exec::async_close(scheduler, server_fd));
    auto sequence = [](stdexec::sender auto ...senders) {
        (stdexec::sync_wait(std::move(senders)), ...);
    };
    sequence(std::move(s), scope.on_empty(), std::move(f));
    std::cout << "done." << std::endl;
}
