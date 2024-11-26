# uring_exec

## Introduction

This project attempts to provide [`stdexec`](https://github.com/NVIDIA/stdexec) support for [`liburing`](https://github.com/axboe/liburing).

```cpp
// An echo server example.

using uring_exec::io_uring_exec;

stdexec::sender
auto echo(io_uring_exec::scheduler scheduler, int client_fd) {
    return
        stdexec::just(std::array<char, 512>{})
      | stdexec::let_value([=](auto &buf) {
            return
                uring_exec::async_read(scheduler, client_fd, buf.data(), buf.size())
              | stdexec::then([=, &buf](int read_bytes) {
                    auto copy = std::ranges::copy;
                    auto view = buf | std::views::take(read_bytes);
                    auto to_console = std::ostream_iterator<char>{std::cout};
                    copy(view, to_console);
                    return read_bytes;
                })
              | stdexec::let_value([=, &buf](int read_bytes) {
                    return uring_exec::async_write(scheduler, client_fd, buf.data(), read_bytes);
                })
              | stdexec::let_value([=, &buf](int written_bytes) {
                    return stdexec::just(written_bytes == 0 || buf[0] == '@');
                })
              | exec::repeat_effect_until();
        })
      | stdexec::let_value([=] {
            std::cout << "Closing client..." << std::endl;
            return uring_exec::async_close(scheduler, client_fd) | stdexec::then([](...){});
        });
}

stdexec::sender
auto server(io_uring_exec::scheduler scheduler, int server_fd, exec::async_scope &scope) {
    return
        uring_exec::async_accept(scheduler, server_fd, {})
      | stdexec::let_value([=, &scope](int client_fd) {
            scope.spawn(echo(scheduler, client_fd));
            return stdexec::just(false);
        })
      | exec::repeat_effect_until();
}

int main() {
    auto server_fd = uring_exec::utils::make_server({.port=8848});
    auto server_fd_cleanup = uring_exec::utils::defer([=] { close(server_fd); });

    io_uring_exec uring({.uring_entries=512});
    exec::async_scope scope;

    stdexec::scheduler auto scheduler = uring.get_scheduler();

    scope.spawn(
        stdexec::schedule(scheduler)
      | stdexec::let_value([=, &scope] {
          return server(scheduler, server_fd, scope);
      })
    );

    uring.run();
}
```

See the `/examples` directory for more usage examples.

## Build

This is a C++20 header-only library; simply include it in your project.

If you want to try some examples or benchmark tests, use `xmake`:
* `xmake build examples`: Build all example files.
* `xmake run <example_name>`: Run a specified example application. (For example, `xmake run hello_coro`.)
* `xmake build benchmarks && xmake run benchmarks`: Build and run the ping-pong test.

`make` is also supported, but you should ensure that:
* Both `stdexec` and `liburing` are available locally.
* `asio` is optional.

Then you can:
* `make all`: Build all examples and benchmarks.
* `make <example_name>`: Build a specified example file.
* `make <benchmark_name>`: Build a specified benchmark file.
* `make benchmark_script`: Run the ping-pong test.

It is recommended to use at least Linux kernel version 6.1.

## Benchmark

Here is my benchmark report on:
* {Linux v6.4.8}
* {AMD 5800H, 16 GB}
* {uring_exec 22a6674, asio 62481a2}
* {gcc v13.2.0 -O3}
* {ping-pong: blocksize = 16384, timeout = 5s, throughput unit = GiB/s}

| threads / sessions | asio (io_uring) | uring_exec |
| ------------------ | --------------- | ---------- |
| 2 / 10             | 1.868           | 3.409      |
| 2 / 100            | 2.744           | 3.870      |
| 2 / 1000           | 1.382           | 2.270      |
| 4 / 10             | 1.771           | 3.164      |
| 4 / 100            | 2.694           | 3.477      |
| 4 / 1000           | 1.275           | 4.411      |
| 8 / 10             | 0.978           | 2.522      |
| 8 / 100            | 2.107           | 2.676      |
| 8 / 1000           | 1.177           | 3.956      |

See the `/benchmarks` directory for more details.

## Notes

+ This project was originally a subproject of [io_uring-examples-cpp](https://github.com/Caturra000/io_uring-examples-cpp).
+ Although `stdexec` provides official io_uring examples, it does not support any I/O operations.
