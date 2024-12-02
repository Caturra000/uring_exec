#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <iostream>
#include <ranges>
#include <format>
#include <cassert>
#include <exec/when_any.hpp>
#include <exec/repeat_effect_until.hpp>
#include "uring_exec.hpp"

using uring_exec::io_uring_exec;
constexpr auto noop = [](auto &&...) {};

stdexec::sender
auto ping(io_uring_exec::scheduler scheduler,
          int client_fd, int blocksize) {
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
              | stdexec::let_value([&r, &w](int read_bytes) {
                    r += read_bytes;
                    return stdexec::just(r, w);
                })
              | stdexec::let_error([&](auto &&) {
                    return stdexec::just(r, w);
                })
              | stdexec::let_stopped([&] {
                    return stdexec::just(r, w);
                });
        });
}

stdexec::sender
auto client(io_uring_exec::scheduler scheduler,
            auto endpoint, int blocksize,
            std::atomic<size_t> &r, std::atomic<size_t> &w) {
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
                 | stdexec::let_value([=](auto&&) {
                       return stdexec::just(client_fd, size_t{}, size_t{});
                   });
        })
      | stdexec::let_value([=, &r, &w](int client_fd, size_t &client_read, size_t &client_written) {
            auto collect = [&, client_fd](auto &&...) {
                constexpr auto mo = std::memory_order::relaxed;
                r.fetch_add(client_read, mo);
                w.fetch_add(client_written, mo);
                return stdexec::just(client_fd);
            };
            return
                ping(scheduler, client_fd, blocksize)
              | stdexec::let_value([&](size_t read_bytes, size_t written_bytes) {
                    client_read += read_bytes;
                    client_written += written_bytes;
                    return stdexec::just(false);
                })
              | exec::repeat_effect_until()
              | stdexec::let_value(collect)
              | stdexec::let_stopped(collect)
              | stdexec::let_error(collect);
        })
      | stdexec::let_value([=](int client_fd) {
            return uring_exec::async_close(scheduler, client_fd);
        })
      | stdexec::upon_error(noop)
      | stdexec::upon_stopped(noop)
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

    exec::async_scope scope;
    stdexec::scheduler auto scheduler = uring.get_scheduler();
    auto endpoint = std::tuple(host, port);
    std::atomic<size_t> r {};
    std::atomic<size_t> w {};

    for(auto n = sessions; n--;) {
        stdexec::sender auto s =
            stdexec::starts_on(scheduler, client(scheduler, endpoint, blocksize, r, w));
        scope.spawn(std::move(s));
    }

    stdexec::sender auto deadline =
        stdexec::schedule(scheduler)
      | stdexec::let_value([=] {
            return uring_exec::async_wait(scheduler, std::chrono::seconds(timeout));
        })
      | stdexec::then([&](auto&&) { scope.request_stop(); });

    stdexec::sender auto timed_execution = exec::when_any(std::move(deadline), scope.on_empty());
    stdexec::sync_wait(std::move(timed_execution));

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
