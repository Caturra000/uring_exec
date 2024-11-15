#pragma once
#include <liburing.h>
#include "detail.h"
namespace uring_exec {

// Is-a immovable io_uring.
struct underlying_io_uring: public detail::immovable,
                            public ::io_uring
{
    underlying_io_uring(unsigned uring_entries, int uring_flags = 0) {
        if(int err = io_uring_queue_init(uring_entries, this, uring_flags)) {
            throw std::system_error(-err, std::system_category());
        }
    }

    struct constructor_parameters {
        unsigned uring_entries;
        int uring_flags = 0;
    };

    // Example: io_uring_exec uring({.uring_entries=512});
    underlying_io_uring(constructor_parameters p)
        : underlying_io_uring(p.uring_entries, p.uring_flags)
    {}

    ~underlying_io_uring() {
        io_uring_queue_exit(this);
    }
};

} // namespace uring_exec
