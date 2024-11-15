#include <unistd.h>
#include <fcntl.h>
#include <iostream>
#include <ranges>
#include "uring_exec.hpp"

using namespace std::chrono_literals;
using namespace std::chrono;

auto print_global_clock = [init = std::chrono::steady_clock::now()] {
    auto now = steady_clock::now();
    auto delta = duration_cast<milliseconds>(now - init);
    std::cout << "global:" << delta << std::endl;
};

int main() {
    io_uring_exec uring(512);
    stdexec::scheduler auto scheduler = uring.get_scheduler();

    auto s1 =
        stdexec::schedule(scheduler)
      | stdexec::let_value([scheduler] {
            return async_wait(scheduler, 2s);
        })
      | stdexec::then([](...) {
            std::cout << "s1:2s" << std::endl;
            print_global_clock();
        })
      | stdexec::let_value([scheduler] {
            return async_wait(scheduler, 2s);
        })
      | stdexec::then([](...) {
            std::cout << "s1:4s" << std::endl;
            print_global_clock();
        });

    auto s2 =
       stdexec::schedule(scheduler)
      | stdexec::let_value([scheduler] {
            return async_wait(scheduler, 1s);
        })
      | stdexec::then([](...) {
            std::cout << "s2:1s" << std::endl;
            print_global_clock();
        })
      | stdexec::let_value([scheduler] {
            return async_wait(scheduler, 2s);
        })
      | stdexec::then([](...) {
            std::cout << "s2:3s" << std::endl;
            print_global_clock();
        });
    std::jthread j([&] { uring.run(); });
    stdexec::sync_wait(stdexec::when_all(std::move(s1), std::move(s2)));
    uring.request_stop();
}
