#include <unistd.h>
#include <fcntl.h>
#include <iostream>
#include <ranges>
#include "uring_exec.hpp"

int main() {
    constexpr auto test_file_name = "/tmp/jojo";
    int fd = (::unlink(test_file_name), ::open(test_file_name, O_RDWR|O_TRUNC|O_CREAT, 0666));
    if(fd < 0) {
        std::cerr << ::strerror(errno) , std::abort();
    }

    io_uring_exec uring(512);
    stdexec::scheduler auto scheduler = uring.get_scheduler();
    std::jthread j {[&](std::stop_token stop_token) { uring.run(stop_token); }};

    auto s1 =
        stdexec::schedule(scheduler)
      | stdexec::then([] {
            std::cout << "hello!" << std::endl;
            return 19260816;
        })
      | stdexec::then([](int v) {
            return v+1;
        });

    auto s2 =
        stdexec::schedule(scheduler)
      | stdexec::let_value([] {
            std::cout << "world!" << std::endl;
            return stdexec::just(std::array<char, 4> {"dio"}); // '\n'
        })
      | stdexec::let_value([scheduler, fd](auto &&buf) {
            return
                async_write(scheduler, fd, buf.data(), buf.size() - 1)
              | stdexec::let_value([&](auto written_bytes) {
                    return async_read(scheduler, fd, buf.data(), written_bytes);
                })
              | stdexec::then([&buf](auto read_bytes) {
                    auto iter = std::ostream_iterator<char>{std::cout};
                    [&](auto &&...views) { (std::ranges::copy(views, iter), ...); }
                        ("read: [", buf | std::views::take(read_bytes), "]\n");
                    return read_bytes;
                });
        });

    // exec::async_scope scope;
    // scope.spawn(std::move(s1) | stdexec::then([](...) {}));
    // scope.spawn(std::move(s2) | stdexec::then([](...) {}));
    // stdexec::sync_wait(scope.on_empty());

    auto a =
        stdexec::when_all(std::move(s1), std::move(s2));
    auto [v1, v2] = stdexec::sync_wait(std::move(a)).value();
    std::cout << "s1: " << v1 << std::endl
              << "s2: " << v2 << std::endl;
}
