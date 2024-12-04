#include <iostream>
#include <exception>
#include <thread>
#include <string>
#include <vector>
#include <tuple>
#include <format>
#include <asio.hpp>

using asio::ip::tcp;
using asio::use_awaitable;

asio::awaitable<void> pong(asio::ip::tcp::socket client, int block_size) try {
    std::string content(block_size, 'x');
    for(;;) {
        auto n = co_await asio::async_read(client, asio::buffer(content), use_awaitable);
        co_await asio::async_write(client, asio::buffer(content, n), use_awaitable);
    }
} catch(std::exception &e) {
    // Ignore EOF.
}

asio::awaitable<void> server(auto &acceptor, int block_size, int session_count) try {
    auto executor = acceptor.get_executor();
    for(int i = 0; i < session_count; ++i) {
        auto client = co_await acceptor.async_accept(use_awaitable);
        asio::co_spawn(
            executor,
            pong(std::move(client), block_size),
            asio::detached);
    }
} catch(std::exception &e) {
}

// Enable io_uring: -DASIO_HAS_IO_URING -DASIO_DISABLE_EPOLL
int main(int argc, char *argv[]) {
    if(argc <= 4) {
        auto message = std::format(
            "usage: {} <port> <threads> <blocksize> <sessions> <timeout>", argv[0]);
        std::cerr << message << std::endl;
        return -1;
    }
    auto atoies = [&](auto ...idxes) { return std::tuple{atoi(argv[idxes])...}; };
    auto [port, thread_count, block_size, session_count] = atoies(1, 2, 3, 4);
    // auto sb = asio::detail::signal_blocker();
    asio::io_context ioc;
    asio::signal_set s(ioc, SIGINT);
    s.async_wait([](auto, auto) { std::quick_exit(0); });
    tcp::acceptor acceptor(ioc, {tcp::v4(), static_cast<asio::ip::port_type>(port)});
    asio::co_spawn(
        ioc,
        server(acceptor, block_size, session_count),
        asio::detached);

    {
        std::vector<std::jthread> threads(thread_count);
        for(auto &&j : threads) j = std::jthread([&] { ioc.run(); });
    }

    std::cout << "done." << std::endl;
}
