#pragma once
#include <liburing.h>
#include "detail.h"
namespace uring_exec {

// Is-a immovable io_uring.
struct underlying_io_uring: public detail::immovable,
                            public ::io_uring
{
    struct constructor_parameters {
        unsigned uring_entries;
        int uring_flags = 0;
        bool sticky = false; // Use the provided flags exactly as-is.
        int ring_fd = -1; // Shared work queues.
    };

    // Example: io_uring_exec uring({.uring_entries=512});
    underlying_io_uring(constructor_parameters p) {
        initiate(p);
    }

    underlying_io_uring(unsigned uring_entries, int uring_flags = 0)
        : underlying_io_uring({.uring_entries = uring_entries, .uring_flags = uring_flags})
    {}

    ~underlying_io_uring() {
        io_uring_queue_exit(this);
    }

private:
    void initiate(constructor_parameters params) {
        io_uring_params underlying_params {};
        auto as_is = params.uring_flags;
        if(!params.sticky) {
            as_is = as_is
                  // We're using thread-local urings.
                  | IORING_SETUP_SINGLE_ISSUER
                  // Improve throughput for server usage.
                  | IORING_SETUP_COOP_TASKRUN;
        }
        if(!params.sticky && params.ring_fd > -1) {
            as_is = as_is
                  // Shared backend.
                  | IORING_SETUP_ATTACH_WQ;
        }
        underlying_params.flags = as_is;
        underlying_params.wq_fd = params.ring_fd;

        if(int err = io_uring_queue_init_params(params.uring_entries, this, &underlying_params)) {
            throw std::system_error(-err, std::system_category());
        }
    }
};

} // namespace uring_exec
