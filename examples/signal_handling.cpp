#include <unistd.h>
#include <fcntl.h>
#include <iostream>
#include <ranges>
#include "uring_exec.hpp"
#include "exec/when_any.hpp"

using uring_exec::io_uring_exec;
using namespace std::chrono_literals;
using namespace std::chrono;

// Modified from `examples/timer.cpp`.
int main() {
    io_uring_exec uring(512);
    stdexec::scheduler auto scheduler = uring.get_scheduler();

    std::cout << "start." << std::endl;

    auto s1 =
        stdexec::schedule(scheduler)
      | stdexec::let_value([=](auto &&...) {
            return uring_exec::async_wait(scheduler, 2s);
        })
      | stdexec::let_value([=](auto &&...) {
            std::cout << "s1:2s" << std::endl;
            return stdexec::just();
        })
      | stdexec::let_value([=](auto &&...) {
            return uring_exec::async_wait(scheduler, 2s);
        })
      | stdexec::let_value([=](auto &&...) {
            std::cout << "s1:4s" << std::endl;
            return stdexec::just();
        });

    auto s2 =
       stdexec::schedule(scheduler)
      | stdexec::let_value([=](auto &&...) {
            return uring_exec::async_wait(scheduler, 1s);
        })
      | stdexec::let_value([=](auto &&...){
            std::cout << "s2:1s" << std::endl;
            return stdexec::just();
        })
      | stdexec::let_value([=](auto &&...) {
            return uring_exec::async_wait(scheduler, 2s);
        })
      | stdexec::let_value([=](auto &&...) {
            std::cout << "s2:3s" << std::endl;
            return stdexec::just();
        });

    stdexec::sender auto signal_watchdog =
        uring_exec::async_sigwait(scheduler, std::array {SIGINT, SIGUSR1});

    auto sb = uring_exec::signal_blocker();

    std::jthread j([&](auto token) { uring.run(token); });
    // $ kill -USR1 $(pgrep signal_handling)
    stdexec::sync_wait(
        exec::when_any(
            stdexec::when_all(std::move(s1), std::move(s2))
            | stdexec::then([](auto &&...) { std::cout << "timer!" << std::endl; }),
            stdexec::starts_on(scheduler, std::move(signal_watchdog))
            | stdexec::then([](auto &&...) { std::cout << "signal!" << std::endl; })
        )
    );
    std::cout << "bye." << std::endl;
}
