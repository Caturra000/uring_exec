#pragma once
#include <mutex>
#include <array>
#include <tuple>
#include <concepts>
#include <ranges>
namespace uring_exec {
namespace detail {

////////////////////////////////////////////////////////////////////// Immovable guarantee

// Operations in stdexec are required to be address-stable.
struct immovable {
    immovable() = default;
    immovable(immovable &&) = delete;
};

////////////////////////////////////////////////////////////////////// Atomic intrusive queue

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
    using node_type = Node;
    using element_type = T;

    void push(T *op) noexcept {
        auto node = _head.load(read_mo);
        do {
            op->*Next = node;
        } while(!_head.compare_exchange_weak(node, op, write_mo, read_mo));
    }

    [[nodiscard]]
    T* move_all() noexcept {
        auto node = _head.load(read_mo);
        while(!_head.compare_exchange_weak(node, nullptr, write_mo, read_mo));
        return static_cast<T*>(node);
    }

    // Push until predicate() == false to queue.
    void push_all(T *first, auto predicate) noexcept {
        Node *last = first;
        if(!predicate(first)) return;
        while(predicate(static_cast<T*>(last->*Next))) last = last->*Next;
        auto node = _head.load(read_mo);
        do {
            last->*Next = node;
        } while(!_head.compare_exchange_weak(node, first, write_mo, read_mo));
    }

    // A unified interface to get the next element.
    inline static T* next(Node *node_or_element) noexcept {
        return static_cast<T*>(node_or_element->*Next);
    }

    // A unified interface to clear the node's metadata.
    inline static void clear(Node *node) noexcept {
        node->*Next = nullptr;
    }

    inline static T* make_fifo(Node *node) noexcept {
        if(node == nullptr) return nullptr;
        Node sentinel {};
        sentinel.*Next = node;
        auto prev = &sentinel, cur = node;
        while(cur) {
            auto next = cur->*Next;
            cur->*Next = prev;
            prev = cur;
            cur = next;
        }
        node->*Next = nullptr;
        return static_cast<T*>(prev);
    }

private:
    inline constexpr static auto read_mo = std::memory_order::relaxed;
    inline constexpr static auto write_mo = std::memory_order::acq_rel;
    alignas(64) std::atomic<Node*> _head {nullptr};
};

////////////////////////////////////////////////////////////////////// Multi lock

// Deprecated.
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

////////////////////////////////////////////////////////////////////// Unified stop source


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

////////////////////////////////////////////////////////////////////// Composable vtable

struct you_are_a_vtable_signature {};

template <typename Signature>
concept vtable_signature = requires {
    typename Signature::sign_off;
    requires std::is_same_v<typename Signature::sign_off,
                            you_are_a_vtable_signature>;
};

// Per-object vtable for (receiver) type erasure.
// Example:
// using vtable = make_vtable<add_cancel_to_vtable<void()>,
//                            add_complete_to_vtable<int(std::string)>>;
template <vtable_signature ...Signatures>
struct make_vtable: Signatures... {};

// Make vtable composable.
template <typename>
struct add_complete_to_vtable;

template <typename>
struct add_cancel_to_vtable;

template <typename>
struct add_restart_to_vtable;

template <typename Ret, typename ...Args>
struct add_complete_to_vtable<Ret(Args...)> {
    using sign_off = you_are_a_vtable_signature;
    Ret (*complete)(Args...) noexcept;
};

template <typename Ret, typename ...Args>
struct add_cancel_to_vtable<Ret(Args...)> {
    using sign_off = you_are_a_vtable_signature;
    Ret (*cancel)(Args...) noexcept;
};

template <typename Ret, typename ...Args>
struct add_restart_to_vtable<Ret(Args...)> {
    using sign_off = you_are_a_vtable_signature;
    Ret (*restart)(Args...) noexcept;
};

} // namespace detail
} // namespace uring_exec
