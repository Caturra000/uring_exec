#include <iostream>
#include <tuple>
#include <chrono>
#include <string_view>
#include <exec/task.hpp>
#include "uring_exec.hpp"

using uring_exec::io_uring_exec;
using namespace std::chrono_literals;

int main() {
    io_uring_exec uring(512);
    stdexec::scheduler auto scheduler = uring.get_scheduler();

    auto println = [](auto &&...args) {
        (std::cout << ... << args) << std::endl;
    };
    auto where = [=](const char *flag) {
        println(flag, "\tYou are here: ", std::this_thread::get_id());
    };

    where("(#main thread)");

    std::jthread j {[&](std::stop_token stop_token) {
        where("(#jthread)");
        uring.run(stop_token);
    }};

    std::this_thread::sleep_for(2s);

    auto next = [=] { println(); };
    auto line = [=] { println("===================="); };
    auto reader_friendly = [=] { next(), line(), next(); };

    reader_friendly();

    auto [n] = stdexec::sync_wait(std::invoke(
        [=](auto scheduler) -> exec::task<int> {
            where("(#before...)");
            co_await exec::reschedule_coroutine_on(scheduler);
            where("(#after...)");

            next();

            println("hello stdexec! and ...");
            co_await uring_exec::async_wait(scheduler, 2s);

            std::string_view hi = "hello coroutine!\n";
            stdexec::sender auto s =
                uring_exec::async_write(scheduler, STDOUT_FILENO, hi.data(), hi.size());
            co_return co_await std::move(s);
        }, scheduler)
    ).value();

    reader_friendly();
    println("written bytes: ", n);
    where("(#goodbye)");
}
