#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <iostream>
#include <ranges>
#include <format>
#include <cassert>
#include <exec/repeat_n.hpp>
#include <exec/repeat_effect_until.hpp>
#include "uring_exec.hpp"

using uring_exec::io_uring_exec;
constexpr auto noop = [](auto &&...) {};

stdexec::sender
auto ping(io_uring_exec::scheduler scheduler,
          int client_fd, int blocksize, auto stop_token) {
    return
        stdexec::just(std::vector<char>(blocksize, 'x'), size_t{}, size_t{})
      | stdexec::let_value([=](auto &buf, auto &r, auto &w) {
            return
                stdexec::just()
              | stdexec::let_value([=, &buf] {
                    return uring_exec::async_write(scheduler, client_fd, buf.data(), buf.size());
                })
              | stdexec::let_value([=, &buf, &w](int written_bytes) {
                    w += written_bytes;
                    return uring_exec::async_read(scheduler, client_fd, buf.data(), written_bytes);
                })
              | stdexec::let_value([=, &r](int read_bytes) {
                    r += read_bytes;
                    return stdexec::just(stop_token.stop_requested());
                })
              | exec::repeat_effect_until()
              | stdexec::upon_error(noop)
              | stdexec::let_value([&] {
                    return stdexec::just(r, w);
                });
        });
}

stdexec::sender
auto client(io_uring_exec::scheduler scheduler,
            auto endpoint, int blocksize,
            std::atomic<size_t> &r, std::atomic<size_t> &w,
            auto stop_token) {
    auto make_addr = [](auto endpoint) {
        auto [host, port] = endpoint;
        sockaddr_in addr_in {};
        addr_in.sin_family = AF_INET;
        inet_pton(AF_INET, host, &addr_in.sin_addr);
        addr_in.sin_port = htons(port);
        auto addr = std::bit_cast<sockaddr>(addr_in);
        return addr;
    };
    return
        stdexec::just()
      | stdexec::let_value([=] {
            return uring_exec::async_socket(scheduler, AF_INET, SOCK_STREAM, IPPROTO_TCP, 0);
        })
      | stdexec::let_value([=, addr = make_addr(endpoint)](int client_fd) {
            return uring_exec::async_connect(scheduler, client_fd, &addr, sizeof addr)
                 | stdexec::then([=](auto&&) { return client_fd; });
        })
      | stdexec::let_value([=, &r, &w](int client_fd) {
            return ping(scheduler, client_fd, blocksize, stop_token)
                 | stdexec::then([=, &r, &w](size_t read_bytes, size_t written_bytes) {
                       r.fetch_add(read_bytes);
                       w.fetch_add(written_bytes);
                       return client_fd;
                   });
        })
      | stdexec::let_value([=](int client_fd) {
            return uring_exec::async_close(scheduler, client_fd);
        })
      | stdexec::upon_error(noop)
      | stdexec::then(noop);
}

int main(int argc, char *argv[]) {
    if(argc <= 5) {
        auto message = std::format(
            "usage: {} <port> <threads> <blocksize> <sessions> <timeout>", argv[0]);
        std::cerr << message << std::endl;
        return -1;
    }
    auto host = "127.0.0.1";
    auto atoies = [&](auto ...idxes) { return std::tuple{atoi(argv[idxes])...}; };
    auto [port, threads, blocksize, sessions, timeout] = atoies(1, 2, 3, 4, 5);
    assert(timeout >= 1);

    auto sb = uring_exec::signal_blocker<uring_exec::sigmask_exclusive>(SIGINT);
    io_uring_exec uring({.uring_entries=512});

    std::vector<std::jthread> thread_pool(threads);
    for(auto &&j : thread_pool) {
        j = std::jthread([&](auto stop_token) { uring.run(stop_token); });
    }

    stdexec::inplace_stop_source iss;
    exec::async_scope scope;
    stdexec::scheduler auto scheduler = uring.get_scheduler();
    auto endpoint = std::tuple(host, port);
    std::atomic<size_t> r {};
    std::atomic<size_t> w {};

    stdexec::sender auto deadline =
        stdexec::schedule(scheduler)
      | stdexec::let_value([=] {
            return uring_exec::async_wait(scheduler, std::chrono::seconds(timeout));
        })
      | stdexec::then([&](auto&&) { iss.request_stop(); });
    scope.spawn(std::move(deadline));

    for(auto n = sessions; n--;) {
        stdexec::sender auto s =
            stdexec::schedule(scheduler)
          | stdexec::let_value([=, &r, &w, &iss] {
                return client(scheduler, endpoint, blocksize, r, w, iss.get_token());
            })
          | stdexec::then(noop);
        scope.spawn(s);
    }

    stdexec::sync_wait(scope.on_empty());

    double read_bytes = r.load();
    double written_bytes = w.load();

    auto stringify = [](double bytes, int timeout) {
        bytes /= timeout;
        auto conv = [&, base = 1024] {
            for(auto i = 0; i < 6; i++) {
                if(bytes < base) return std::tuple(bytes, "BKMGTP"[i]);
                bytes /= base;
            }
            return std::tuple(bytes, 'E');
        };
        auto [good_bytes, unit] = conv();
        auto suffix = ('B'==unit ? "" : "iB");
        return std::format("{:.3f} {}{}", good_bytes, unit, suffix);
    };
    auto println = [](auto ...args) {
        (std::cout << ... << args) << std::endl;
    };

    println("done.");
    println("read: ",               stringify(read_bytes,    1));
    println("write: ",              stringify(written_bytes, 1));
    println("throughput (read): ",  stringify(read_bytes,    timeout), "/s");
    println("throughput (write): ", stringify(written_bytes, timeout), "/s");
}
