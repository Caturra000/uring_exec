# uring_exec

[适配 io_uring 异步函数到 C++26 std::execution](https://www.bluepuni.com/archives/porting-liburing-to-stdexec/)

## Introduction

This project attempts to provide [`stdexec`](https://github.com/NVIDIA/stdexec) support for [`liburing`](https://github.com/axboe/liburing). It is also a `std::execution`-based network library.

## Features

+ [sender/receiver](#senderreceiver)
+ [C++20 coroutine](#c20-coroutine)
+ [Thread pool](#thread-pool)
+ [Stop token](#stop-token)
+ [Run loop](#run-loop)
+ [Cancellation](#cancellation)
+ [Signal handling](#signal-handling)
+ [And more!](#and-more)

### sender/receiver

```cpp
// An echo server example.

using uring_exec::io_uring_exec;

// READ -> WRITE -> [CLOSE]
//      <-
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

// ACCEPT -> ACCEPT
//        -> ECHO
stdexec::sender
auto server(io_uring_exec::scheduler scheduler, int server_fd, exec::async_scope &scope) {
    return
        uring_exec::async_accept(scheduler, server_fd, nullptr, nullptr, 0)
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

    // Run infinitely.
    uring.run();
}
```

### C++20 coroutine

```cpp
// C++20 coroutine styles.
using uring_exec::io_uring_exec;

int main() {
    io_uring_exec uring(512);
    stdexec::scheduler auto scheduler = uring.get_scheduler();

    std::jthread j {[&](std::stop_token stop_token) {
        // Run until a stop request is received.
        uring.run(stop_token);
    }};

    auto [n] = stdexec::sync_wait(std::invoke(
        [=](auto scheduler) -> exec::task<int> {
            co_await exec::reschedule_coroutine_on(scheduler);
            // Scheduled to the specified execution context.

            println("hello stdexec! and ...");
            co_await uring_exec::async_wait(scheduler, 2s);

            std::string_view hi = "hello coroutine!\n";
            stdexec::sender auto s =
                uring_exec::async_write(scheduler, STDOUT_FILENO, hi.data(), hi.size());
            co_return co_await std::move(s);
        }, scheduler)
    ).value();

    // Automatically rescheduled to the main thread.

    println("written bytes: ", n);
}
```

### Thread pool

```cpp
// `uring_exec::io_uring_exec` is MT-safe.
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
              | stdexec::then([&, i] { user_request(i); });
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
```

### Stop token

```cpp
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
```

### Run loop

```cpp
/// Policies.
struct run_policy {
    // Informal forward progress guarantees.
    // NOTES:
    // * These are exclusive flags, but using bool (not enum) for simplification.
    // * `weakly_concurrent` is not a C++ standard part, which can make progress eventually
    //   with lower overhead compared to `concurrent`, provided it is used properly.
    // * `parallel` (which makes progress per `step`) is NOT supported for IO operations.
    bool concurrent {true};         // which requires that a thread makes progress eventually.
    bool weakly_parallel {false};   // which does not require that the thread makes progress.
    bool weakly_concurrent {false}; // which requires that a thread may make progress eventually.

    // Event handling.
    // Any combination is welcome.
    bool launch {true};
    bool submit {true};
    bool iodone {true};

    // Behavior details.
    bool busyloop {false};          // No yield.
    bool autoquit {false};          // `concurrent` runs infinitely by default.
    bool waitable {false};          // Submit and wait.
    bool hookable {true};           // Always true beacause of per-object vtable.
    bool detached {false};          // Ignore stop requests from `io_uring_exec`.
    bool progress {false};          // run() returns run_progress_info.
    bool no_delay {false};          // Complete I/O as fast as possible.
    bool blocking {false};          // in-flight operations cannot be interrupted by a stop request.

    bool transfer {false};          // For stopeed local context. Just a tricky restart.
    bool terminal {false};          // For stopped remote context. Cancel All.

    size_t iodone_batch {64};       // (Roughly estimated value) for `io_uring_peek_batch_cqe`.
    size_t iodone_maxnr {512};      // Maximum number of `cqe`s that can be taken in one step.
};

/// Global interface.
// run_policy:       See the comments above.
// any_stop_token_t: Compatible with `std::jthread` and `std::stop_token` for a unified interface.
// Return type:      Either `run_progress_info` or `void`, depending on `run_policy.progress`.
template <run_policy policy = {},
            typename any_stop_token_t = stdexec::never_stop_token>
auto run(any_stop_token_t external_stop_token = {});

/// Example.
int main() {
    uring_exec::io_uring_exec uring({.uring_entries=8});
    // You can also use C++20 designated initializer.
    constexpr auto policy = [] {
        auto policy = run_policy{};
        policy.concurrent = false;
        policy.weakly_parallel = true;
        policy.iodone = false;
        policy.blocking = true;
        return policy;
    } ();
    std::jthread j([&](auto token) {
        uring.run<policy>(token);
    });
}
```

### Cancellation

```cpp
int main() {
    io_uring_exec uring({.uring_entries = 8});
    std::array<std::jthread, 5> threads;
    for(auto &&j : threads) {
        j = std::jthread([&](auto token) { uring.run(token); });
    }
    using namespace std::chrono_literals;
    stdexec::scheduler auto s = uring.get_scheduler();
    stdexec::sender auto _3s = make_sender(s, 3s);
    stdexec::sender auto _9s = make_sender(s, 9s);
    // Waiting for 3 seconds, not 9 seconds.
    stdexec::sender auto any = exec::when_any(std::move(_3s), std::move(_9s));
    stdexec::sync_wait(std::move(any));
}
```

### Signal handling

```cpp
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

    // Async_sigwait for specified signals.
    stdexec::sender auto signal_watchdog =
        uring_exec::async_sigwait(scheduler, std::array {SIGINT, SIGUSR1});

    // Block all/some signals in ctor.
    // Restore previous signals in dtor (or .reset()).
    // Examples:
    //     // Block all signals.
    //     auto sb = signal_blocker();
    //
    //     // Block a single SIGINT signal.
    //     auto sb = signal_blocker(SIGINT);
    //
    //     // Block SIGINT and SIGUSR1 singls.
    //     // (std::vector<int> or other ranges are also acceptable.)
    //     auto sb = signal_blocker(std::array {SIGINT, SIGUSR1});
    //
    //     // Block all signals except SIGINT and SIGUSR1.
    //     auto sb = signal_blocker<sigmask_exclusive>(std::array {SIGINT, SIGUSR1});
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
```

### And more!

See the [`/examples`](/examples) directory for more usage examples.

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

See the [`/benchmarks`](/benchmarks) directory for more details.

## Core APIs (senders)

```cpp
// io_uring_prep_*(sqe, ...) -> async_*(scheduler, ...)
inline constexpr auto async_oepnat      = make_uring_sender_v<io_uring_prep_openat>;
inline constexpr auto async_readv       = make_uring_sender_v<io_uring_prep_readv>;
inline constexpr auto async_readv2      = make_uring_sender_v<io_uring_prep_readv2>;
inline constexpr auto async_writev      = make_uring_sender_v<io_uring_prep_writev>;
inline constexpr auto async_writev2     = make_uring_sender_v<io_uring_prep_writev2>;
inline constexpr auto async_close       = make_uring_sender_v<io_uring_prep_close>;
inline constexpr auto async_socket      = make_uring_sender_v<io_uring_prep_socket>;
inline constexpr auto async_bind        = make_uring_sender_v<io_uring_prep_bind>;
inline constexpr auto async_accept      = make_uring_sender_v<io_uring_prep_accept>;
inline constexpr auto async_connect     = make_uring_sender_v<io_uring_prep_connect>;
inline constexpr auto async_send        = make_uring_sender_v<io_uring_prep_send>;
inline constexpr auto async_recv        = make_uring_sender_v<io_uring_prep_recv>;
inline constexpr auto async_sendmsg     = make_uring_sender_v<io_uring_prep_sendmsg>;
inline constexpr auto async_recvmsg     = make_uring_sender_v<io_uring_prep_recvmsg>;
inline constexpr auto async_shutdown    = make_uring_sender_v<io_uring_prep_shutdown>;
inline constexpr auto async_poll_add    = make_uring_sender_v<io_uring_prep_poll_add>;
inline constexpr auto async_poll_update = make_uring_sender_v<io_uring_prep_poll_update>;
inline constexpr auto async_poll_remove = make_uring_sender_v<io_uring_prep_poll_remove>;
inline constexpr auto async_timeout     = make_uring_sender_v<io_uring_prep_timeout>;
inline constexpr auto async_futex_wait  = make_uring_sender_v<io_uring_prep_futex_wait>;
inline constexpr auto async_futex_wake  = make_uring_sender_v<io_uring_prep_futex_wake>;
inline constexpr auto async_nop         = /* (scheduler) */;
inline constexpr auto async_read        = /* (scheduler, int fd, void *buf, size_t n) */;
inline constexpr auto async_write       = /* (scheduler, int fd, const void *buf, size_t n) */;
inline constexpr auto async_wait        = /* (scheduler, steady_clock::duration duration) */;
inline constexpr auto async_sigwait     = /* (scheduler, std::ranges::range auto signals) */;
```

See the [`io_uring_exec_sender.h`](/include/uring_exec/io_uring_exec_sender.h) file for more details.

## Notes

+ This project was originally a subproject of [io_uring-examples-cpp](https://github.com/Caturra000/io_uring-examples-cpp).
+ Although `stdexec` provides official io_uring examples, it does not support any I/O operations.
