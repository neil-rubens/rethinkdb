// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef CONTAINERS_UNION_HPP_
#define CONTAINERS_UNION_HPP_

#include <type_traits>
#include <utility>

#include "errors.hpp"

namespace union_details {

template <class... Args>
union wrap_t;

template <class... Args>
struct copy_t {
    copy_t(int _tag, const wrap_t<Args...> *_wrap)
        : tag(_tag), wrap(_wrap) { }

    int tag;
    const wrap_t<Args...> *wrap;
};



template <class First>
union wrap_t<First> {
    First first;

    wrap_t() noexcept { }

    wrap_t(First &&x) noexcept : first(std::move(x)) {
        static_assert(std::is_nothrow_move_constructible<First>::value,
                      "Type is not nothrow constructible.");
    }

    ~wrap_t() noexcept {
        // union_t does the actual member destructing.
    }
};

template <class First, class... Rest>
union wrap_t<First, Rest...> {
    First first;
    wrap_t<Rest...> rest;

    wrap_t() noexcept { }

    wrap_t(First &&x) noexcept : first(std::move(x)) {
        static_assert(std::is_nothrow_move_constructible<First>::value,
                      "Type is not nothrow constructible.");
    }

    template <class T>
    wrap_t(T &&x) noexcept : rest(std::forward<T>(x)) { }

    ~wrap_t() noexcept {
        // union_t does the actual member destructing.
    }
};




template <int I, class T, class... Args>
struct type_index_t;

template <int I, class T>
struct type_index_t<I, T> : public std::integral_constant<int, -1> { };

template <int I, class T, class First, class... Rest>
struct type_index_t<I, T, First, Rest...>
    : public std::conditional<std::is_same<T, First>::value,
                              std::integral_constant<int, I>,
                              type_index_t<I + 1, T, Rest...> >::type { };

template <int I, class T>
struct type_constructible_constant_t : public std::integral_constant<int, I> {
    using type = T;
};

template <int I, class T, class... Args>
struct type_constructible_index_t;

template <int I, class T>
struct type_constructible_index_t<I, T> : public std::integral_constant<int, -1> { };

template <int I, class T, class First, class... Rest>
struct type_constructible_index_t<I, T, First, Rest...>
    : public std::conditional<
        std::is_constructible<First, T>::value,
    typename std::conditional<type_constructible_index_t<I + 1, T, Rest...>::value == -1,
                              type_constructible_constant_t<I, First>,
                              std::integral_constant<int, -2> >::type,
        type_constructible_index_t<I + 1, T, Rest...> >::type { };


template <class... Args>
struct all_different_t;

template <class T>
struct all_different_t<T> : std::true_type { };

template <class First, class... Rest>
struct all_different_t<First, Rest...>
    : public std::conditional<type_index_t<0, First, Rest...>::value == -1,
                              all_different_t<Rest...>,
                              std::false_type>::type { };


template <class T, class... Args>
struct get_t;

template <class T, class... Rest>
struct get_t<T, T, Rest...> {
    static inline T *get(wrap_t<T, Rest...> *u) noexcept {
        return &u->first;
    }
};

template <class T, class First, class... Rest>
struct get_t<T, First, Rest...> {
    static inline T *get(wrap_t<First, Rest...> *u) noexcept {
        return get_t<T, Rest...>::get(&u->rest);
    }
};

template <class T, class... Args>
void destroy(wrap_t<Args...> *u) noexcept {
    // std::is_nothrow_destructible is not universally supported.
    static_assert(noexcept(get_t<T, Args...>::get(u)->~T()),
                  "Type is not nothrow destructible.");

    get_t<T, Args...>::get(u)->~T();
}

template <class V, class T, class... Args>
void visit(V &&visitor, wrap_t<Args...> *u) {
    std::forward<V>(visitor)(*get_t<T, Args...>::get(u));
}

template <class V, class T, class... Args>
void visit_const(V &&visitor, const wrap_t<Args...> *u) {
    const T *const p = get_t<T, Args...>::get(const_cast<wrap_t<Args...> *>(u));
    std::forward<V>(visitor)(*p);
}

template <class T, class... Args>
void copy(wrap_t<Args...> *dest, const wrap_t<Args...> *source) {
    static_assert(std::is_nothrow_copy_constructible<T>::value,
                  "Type T is not nothrow copy constructible.  "
                  "(Many types aren't, because they "
                  "can have allocation errors.)");
    const T *p = get_t<T, Args...>::get(const_cast<wrap_t<Args...> *>(source));
    new (get_t<T, Args...>::get(dest)) T(*p);
}

template <class T, class... Args>
void move(wrap_t<Args...> *dest, wrap_t<Args...> *source) {
    static_assert(std::is_nothrow_move_constructible<T>::value,
                  "Type T is not nothrow move constructible.");
    T *p = get_t<T, Args...>::get(source);
    new (get_t<T, Args...>::get(dest)) T(std::move(*p));
}

}  // namespace union_details

template <class... Args>
class union_t {
public:
    static_assert(union_details::all_different_t<Args...>::value,
                  "A union_t has two identical types.");

    ~union_t() noexcept {
        destroy_self();
    }

    union_t() noexcept : tag_(0) {
        new (&u_.first) decltype(u_.first)();
    }

    union_t(const union_t &copyee) noexcept : tag_(copyee.tag_) {
        init_copy_from(copyee);
    }

    union_t(union_t &&movee) noexcept : tag_(movee.tag_) {
        init_move_from(std::move(movee));
    }

    template <class T,
              typename std::enable_if<union_details::type_constructible_index_t<0, T &&, Args...>::value >= 0>::type * = nullptr>
    union_t(T &&x) noexcept
    : tag_(union_details::type_constructible_index_t<0, T &&, Args...>::value) {
        static_assert(union_details::type_constructible_index_t<0, T &&, Args...>::value >= 0,
                      "A union_t constructed with unrecognized type.");

        using real_type_t = typename union_details::type_constructible_index_t<0, T &&, Args...>::type;

        new (union_details::get_t<real_type_t, Args...>::get(&u_))
            real_type_t(std::forward<T>(x));
    }

    union_t &operator=(const union_t &other) noexcept {
        if (this != &other) {
            // We destroy ourself, then reinitialize -- noexcept guarantees let us do
            // this.
            destroy_self();
            tag_ = other.tag_;
            init_copy_from(other);
        }
        return *this;
    }
    union_t &operator=(union_t &&other) noexcept {
        if (this != &other) {
            // We destroy ourself, then reinitialize -- noexcept guarantees let us do
            // this.
            destroy_self();
            tag_ = other.tag_;
            init_move_from(std::move(other));
        }
        return *this;
    }

    template <class V>
    void visit(V &&visitor) {
        typedef void (*visitor_t)(V &&, union_details::wrap_t<Args...> *);
        visitor_t arr[] = { &union_details::visit<V, Args, Args...>... };
        (*arr[tag_])(std::forward<V>(visitor), &u_);
    }

    template <class V>
    void visit(V &&visitor) const {
        typedef void (*visitor_t)(V &&, const union_details::wrap_t<Args...> *);
        visitor_t arr[] = { &union_details::visit_const<V, Args, Args...>... };
        (*arr[tag_])(std::forward<V>(visitor), &u_);
    }

    int tag() const { return tag_; }

private:
    template <class T, class... Args2>
    friend T &get(union_t<Args2...> &u);

    template <class T, class... Args2>
    friend const T &get(const union_t<Args2...> &u);

    template <class T, class... Args2>
    friend T &&get(union_t<Args2...> &&u);

    template <class T, class... Args2>
    friend T *get(union_t<Args2...> *u);

    template <class T, class... Args2>
    friend const T *get(const union_t<Args2...> *u);

    void destroy_self() noexcept {
        typedef void (*destructor_t)(union_details::wrap_t<Args...> *);
        destructor_t arr[] = { &union_details::destroy<Args, Args...>... };
        (*arr[tag_])(&u_);
    }

    void init_copy_from(const union_t &copyee) noexcept {
        rassert(tag_ == copyee.tag_);
        typedef void (*copier_t)(union_details::wrap_t<Args...> *,
                                 const union_details::wrap_t<Args...> *);
        copier_t arr[] { &union_details::copy<Args, Args...>... };
        (*arr[copyee.tag_])(&u_, &copyee.u_);
    }

    void init_move_from(union_t &&movee) noexcept {
        rassert(tag_ == movee.tag_);
        typedef void (*mover_t)(union_details::wrap_t<Args...> *,
                                union_details::wrap_t<Args...> *);
        mover_t arr[] { &union_details::move<Args, Args...>... };
        (*arr[movee.tag_])(&u_, &movee.u_);
    }

    template <class T>
    T *try_get() noexcept {
        static_assert(union_details::type_index_t<0, T, Args...>::value >= 0,
                      "A union_t::get with unrecognized type.");

        if (tag_ == union_details::type_index_t<0, T, Args...>::value) {
            return union_details::get_t<T, Args...>::get(&u_);
        } else {
            return nullptr;
        }
    }

    template <class T>
    T *do_get() noexcept {
        T *ret = try_get<T>();
        guarantee(ret != nullptr,
                  "union_t::do_get failed: tag = %d, T's type index = %d",
                  tag_, union_details::type_index_t<0, T, Args...>::value);

        return ret;
    }

    // 0 <= tag_ < sizeof...(Args).
    int tag_;
    union_details::wrap_t<Args...> u_;
};

template <class T, class... Args>
T &get(union_t<Args...> &u) {
    return *u.template do_get<T>();
}

template <class T, class... Args>
const T &get(const union_t<Args...> &u) {
    return get<T>(const_cast<union_t<Args...> &>(u));
}

template <class T, class... Args>
T &&get(union_t<Args...> &&u) {
    return std::move(*u.template do_get<T>());
}

template <class T, class... Args>
T *get(union_t<Args...> *u) {
    return u->template try_get<T>();
}

template <class T, class... Args>
const T *get(const union_t<Args...> *u) {
    return get<T>(const_cast<union_t<Args...> *>(u));
}

#endif  // CONTAINERS_UNION_HPP_
