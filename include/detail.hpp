#pragma once
#include <mutex>
#include <array>
#include <tuple>
#include <concepts>
#include <ranges>

namespace detail {

// Operations in stdexec are required to be address-stable.
struct immovable {
    immovable() = default;
    immovable(immovable &&) = delete;
};

template <typename Mutex = std::mutex,
          size_t N_way_concurrency = 4>
struct multi_lock {
    auto fastpath_guard(const std::derived_from<immovable> auto &stable_object) {
        using T = std::decay_t<decltype(stable_object)>;
        auto to_value = std::bit_cast<std::uintptr_t, const T*>;
        auto no_align = [&](auto v) { return v / alignof(T); };
        auto to_index = [&](auto v) { return v % N_way_concurrency; };
        auto then = std::views::transform;
        auto view = std::views::single(&stable_object)
                  | then(to_value)
                  | then(no_align)
                  | then(to_index);
        return std::unique_lock{_mutexes[view[0]]};
    }

    auto slowpath_guard() {
        auto make_scoped_lock = [](auto &&...mutexes) {
            return std::scoped_lock{mutexes...};
        };
        return std::apply(make_scoped_lock, _mutexes);
    }

    std::array<Mutex, N_way_concurrency> _mutexes;
};

template <typename T, typename Node>
concept intrusive =
    std::derived_from<T, immovable>
 && std::derived_from<T, Node>;

template <typename, auto>
struct intrusive_queue;

template <typename Node,
          intrusive<Node> T,
          Node* Node::*Next>
    requires requires(T t) { t.*Next; }
struct intrusive_queue<T, Next> {
    inline constexpr static auto read_mo = std::memory_order::relaxed;
    inline constexpr static auto write_mo = std::memory_order::acq_rel;

    void push(T *op) noexcept {
        auto node = _head.load(read_mo);
        do {
            op->*Next = node;
        } while(!_head.compare_exchange_weak(node, op, write_mo, read_mo));
    }

    T* move_all() noexcept {
        auto node = _head.load(read_mo);
        while(!_head.compare_exchange_weak(node, nullptr, write_mo, read_mo));
        return static_cast<T*>(node);
    }

    // A unified interface to get the next element.
    inline static T* next(intrusive<Node> auto *node_or_element) noexcept {
        return static_cast<T*>(node_or_element->*Next);
    }

private:
    alignas(64) std::atomic<Node*> _head {nullptr};
};

struct you_are_a_vtable_signature {};

template <typename Signature>
concept vtable_signature = requires {
    typename Signature::sign_off;
    requires std::is_same_v<typename Signature::sign_off,
                            you_are_a_vtable_signature>;
};

// Make vtable composable.
template <typename>
struct add_complete_to_vtable;

// Make vtable composable.
template <typename>
struct add_cancel_to_vtable;

template <typename Ret, typename ...Args>
struct add_complete_to_vtable<Ret(Args...)> {
    using sign_off = you_are_a_vtable_signature;
    Ret (*complete)(Args...);
};

template <typename Ret, typename ...Args>
struct add_cancel_to_vtable<Ret(Args...)> {
    using sign_off = you_are_a_vtable_signature;
    Ret (*cancel)(Args...);
};

// Per-object vtable for (receiver) type erasure.
// Example:
// using vtable = make_vtable<add_cancel_to_vtable<void()>,
//                            add_complete_to_vtable<int(std::string)>>;
template <vtable_signature ...Signatures>
struct make_vtable: Signatures... {};

// Make different stop sources more user-facing.
template <typename> struct unified_stop_source;
using std_stop_source     = unified_stop_source<std::stop_source>;
using stdexec_stop_source = unified_stop_source<stdexec::inplace_stop_source>;

template <typename self_t, typename stop_source_impl>
struct unified_stop_source_base: protected stop_source_impl {
    using stop_source_type = self_t;
    using underlying_stop_source_type = stop_source_impl;
};

template <>
struct unified_stop_source<std::stop_source>
: protected unified_stop_source_base<std_stop_source,
                                     std::stop_source> {
    using underlying_stop_source_type::request_stop;
    using underlying_stop_source_type::stop_requested;
    using underlying_stop_source_type::stop_possible;
    auto get_stop_token() const noexcept { return underlying_stop_source_type::get_token(); }
};

template <>
struct unified_stop_source<stdexec::inplace_stop_source>
: protected unified_stop_source_base<stdexec_stop_source,
                                     stdexec::inplace_stop_source> {
    using underlying_stop_source_type::request_stop;
    using underlying_stop_source_type::stop_requested;
    // Associated stop-state is always available in our case.
    constexpr auto stop_possible() const noexcept { return true; }
    auto get_stop_token() const noexcept { return underlying_stop_source_type::get_token(); }
};

} // namespace detail
