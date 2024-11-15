#include <iostream>
#include <array>
#include <thread>
#include <algorithm>
#include <numeric>
#include "uring_exec.hpp"

using uring_exec::io_uring_exec;

int main() {
    io_uring_exec uring({.uring_entries=512});
    stdexec::scheduler auto scheduler = uring.get_scheduler();
    exec::async_scope scope;

    constexpr size_t pool_size = 4;
    constexpr size_t user_number = 4;
    constexpr size_t some = 10000;

    std::atomic<size_t> refcnt {};

    auto thread_pool = std::array<std::jthread, pool_size>{};
    for(auto &&j : thread_pool) {
        j = std::jthread([&](auto token) { uring.run(token); });
    }

    auto users = std::array<std::jthread, user_number>{};
    auto user_request = [&refcnt](int i) {
        refcnt.fetch_add(i, std::memory_order::relaxed);
    };
    auto user_frequency = std::views::iota(1) | std::views::take(some);
    auto user_post_requests = [&] {
        for(auto i : user_frequency) {
            stdexec::sender auto s =
                stdexec::schedule(scheduler)
              | stdexec::then([&, i] { user_request(i); })
              | stdexec::let_value([scheduler] {
                    return
                        uring_exec::async_nop(scheduler)
                      | stdexec::then([](...){});
                });
            scope.spawn(std::move(s));
        }
    };

    for(auto &&j : users) j = std::jthread(user_post_requests);
    for(auto &&j : users) j.join();
    // Fire but don't forget.
    stdexec::sync_wait(scope.on_empty());

    assert(refcnt == [&](...) {
        size_t sum = 0;
        for(auto i : user_frequency) sum += i;
        return sum * user_number;
    } ("Check refcnt value."));

    std::cout << "done: " << refcnt << std::endl;
}
