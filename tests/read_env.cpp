#include <iostream>
#include <tuple>
#include <string_view>
#include "uring_exec.hpp"

using uring_exec::io_uring_exec;

int main() {
    io_uring_exec uring(512);
    auto where = [](std::string_view prefix) {
        auto id = std::this_thread::get_id();
        std::cout << prefix << ":\t"
                  << id
                  << std::endl;
        return id;
    };
    auto main_id = where("#main_thread");
    std::jthread j([&](auto token) {
        where("#context");
        uring.run(token);
    });

    stdexec::sender auto s =
        stdexec::starts_on(uring.get_scheduler(), stdexec::just())
      | stdexec::let_value([where] {
            return
                stdexec::read_env(stdexec::get_scheduler)
              | stdexec::let_value([where](auto scheduler) {
                    return stdexec::starts_on(scheduler, stdexec::just(where("#nested")));
                });
        });
    auto [id] = stdexec::sync_wait(std::move(s)).value();
    assert(j.get_id() == id);
    assert(main_id != id);
}
