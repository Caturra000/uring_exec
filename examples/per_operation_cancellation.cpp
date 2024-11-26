#include <iostream>
#include <syncstream>
#include <exec/when_any.hpp>
#include "uring_exec.hpp"

using uring_exec::io_uring_exec;

auto make_sender(io_uring_exec::scheduler scheduler,
                 std::chrono::steady_clock::duration duration)
{
    return
        stdexec::schedule(scheduler)
      | stdexec::let_value([=] {
            auto sout = std::osyncstream{std::cout};
            sout << duration.count() << " is on thread:"
                 << std::this_thread::get_id() << std::endl;
            return uring_exec::async_wait(scheduler, duration);
        });
}

int main() {
    io_uring_exec uring({.uring_entries = 8});
    std::array<std::jthread, 5> threads;
    for(auto &&j : threads) {
        j = std::jthread([&](auto token) { uring.run(token); });
    }
    using namespace std::chrono_literals;
    std::cout << "#main is on thread: "
              << std::this_thread::get_id() << std::endl;
    stdexec::scheduler auto s = uring.get_scheduler();
    stdexec::sender auto _3s = make_sender(s, 3s);
    stdexec::sender auto _9s = make_sender(s, 9s);
    // Waiting for 3 seconds, not 9 seconds.
    stdexec::sender auto any = exec::when_any(std::move(_3s), std::move(_9s));
    stdexec::sync_wait(std::move(any));
    std::cout << "bye" << std::endl;
}
