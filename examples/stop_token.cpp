#include <iostream>
#include "uring_exec.hpp"

using uring_exec::io_uring_exec;

int main() {
    // Default behavior: Infinite run().
    // {
    //     io_uring_exec uring({.uring_entries=8});
    //     std::jthread j([&] { uring.run(); });
    // }

    // Per-run() user-defined stop source (external stop token).
    auto user_defined = [](auto stop_source) {
        io_uring_exec uring({.uring_entries=8});
        auto stoppable_run = [&](auto stop_token) { uring.run(stop_token); };
        std::jthread j(stoppable_run, stop_source.get_token());
        stop_source.request_stop();
    };
    user_defined(std::stop_source {});
    user_defined(stdexec::inplace_stop_source {});
    std::cout << "case 1: stopped." << std::endl;

    // Per-io_uring_exec stop source.
    {
        using uring_stop_source_type = io_uring_exec::underlying_stop_source_type;
        static_assert(
            std::is_same_v<uring_stop_source_type, std::stop_source> ||
            std::is_same_v<uring_stop_source_type, stdexec::inplace_stop_source>
        );
        io_uring_exec uring({.uring_entries=8});
        std::jthread j([&] { uring.run(); });
        uring.request_stop();
    }
    std::cout << "case 2: stopped." << std::endl;

    // Per-std::jthread stop source.
    {
        io_uring_exec uring({.uring_entries=8});
        std::jthread j([&](std::stop_token token) { uring.run(token); });
    }
    std::cout << "case 3: stopped." << std::endl;

    // Heuristic algorithm (autoquit).
    {
        io_uring_exec uring({.uring_entries=8});
        constexpr auto autoquit_policy = io_uring_exec::run_policy {.autoquit=true};
        std::jthread j([&] { uring.run<autoquit_policy>(); });
    }
    std::cout << "case 4: stopped." << std::endl;
}
