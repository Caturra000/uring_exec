#include <unistd.h>
#include <fcntl.h>
#include <iostream>
#include <ranges>
#include "uring_exec.hpp"

using uring_exec::io_uring_exec;
using namespace std::chrono_literals;
using namespace std::chrono;

auto global_clock() {
    static auto init = steady_clock::now();
    auto now = steady_clock::now();
    auto delta = duration_cast<milliseconds>(now - init);
    return [=] { std::cout << "global:" << delta << std::endl; };
};

int main() {
    io_uring_exec uring(512);
    stdexec::scheduler auto scheduler = uring.get_scheduler();

    std::cout << "start." << std::endl;
    global_clock();

    auto s1 =
        stdexec::schedule(scheduler)
      | stdexec::let_value([=] {
            return uring_exec::async_wait(scheduler, 2s);
        })
      | stdexec::then([](...) {
            std::cout << "s1:2s" << std::endl;
            global_clock()();
        })
      | stdexec::let_value([=] {
            return uring_exec::async_wait(scheduler, 2s);
        })
      | stdexec::then([](...) {
            std::cout << "s1:4s" << std::endl;
            global_clock()();
        });

    auto s2 =
       stdexec::schedule(scheduler)
      | stdexec::let_value([=] {
            return uring_exec::async_wait(scheduler, 1s);
        })
      | stdexec::then([](...) {
            std::cout << "s2:1s" << std::endl;
            global_clock()();
        })
      | stdexec::let_value([=] {
            return uring_exec::async_wait(scheduler, 2s);
        })
      | stdexec::then([](...) {
            std::cout << "s2:3s" << std::endl;
            global_clock()();
        });
    std::jthread j([&] { uring.run(); });
    stdexec::sync_wait(stdexec::when_all(std::move(s1), std::move(s2)));
    uring.request_stop();
}
