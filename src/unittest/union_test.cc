// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "containers/counted.hpp"
#include "containers/scoped.hpp"
#include "containers/union.hpp"
#include "errors.hpp"
#include "unittest/gtest.hpp"
#include "utils.hpp"

namespace unittest {

struct union_test_visitor_t {
    std::string msg;
    void operator()(double x) {
        msg += strprintf("double %f ", x);
    }
    void operator()(std::string s) {
        msg += strprintf("string <%s> ", s.c_str());
    }
    void operator()(std::vector<int> v) {
        msg += strprintf("vector of %zu ", v.size());
    }
};

class unittest_countable_t {
public:
    std::string blah;
    mutable int refcount = 0;
};

void counted_add_ref(const unittest_countable_t *p) noexcept {
    ++p->refcount;
}

void counted_release(const unittest_countable_t *p) {
    --p->refcount;
    if (p->refcount == 0) {
        delete p;
    }
}


TEST(UnionTest, Move) {
    using u_t = union_t<double, std::string, std::vector<int> >;
    u_t x("hello");

    ASSERT_EQ("hello", get<std::string>(x));

    {
        // Make sure move-construction actually moves, doesn't copy.
        u_t z(std::move(x));
        union_test_visitor_t v;
        z.visit(v);
        x.visit(v);
        ASSERT_EQ("string <hello> string <> ", v.msg);
    }

    {
        // Make sure move-assignment works, doesn't copy.
        x = "hello";
        u_t z;
        z = std::move(x);
        union_test_visitor_t v;
        z.visit(v);
        x.visit(v);
        ASSERT_EQ("string <hello> string <> ", v.msg);
    }


    // Test that non-copyable types work.
    union_t<scoped_ptr_t<std::string> > u = make_scoped<std::string>("heya");
    ASSERT_EQ("heya", *get<scoped_ptr_t<std::string> >(u));

    // (By the way, if you try to copy a union_t<double, std::string,
    // std::vector<int> >, it won't compile because the string and vector types are
    // not nothrow-copyable.)
}


struct union_test_copy_visitor_t {
    union_test_copy_visitor_t(std::string *_msg) : msg(_msg) { }
    void operator()(const counted_t<const unittest_countable_t> &c) const {
        *msg += c.has() ? "notnull " : "null ";
    }
    void operator()(double &d) const {
        *msg += strprintf("double %f ", d);
        d += 1;
    }

    void operator()(const char *s) const {
        *msg += strprintf("string <%s> ", s);
    }

    std::string *msg;
};

// Test stuff with noexcept-copyable types.
TEST(UnionTest, Copy) {
    using u_t = union_t<counted_t<const unittest_countable_t>, double, const char *>;

    std::string msg;
    u_t u(make_counted<unittest_countable_t>());
    u.visit(union_test_copy_visitor_t(&msg));
    EXPECT_EQ("notnull ", msg);

    u_t v(u);
    msg.clear();
    u.visit(union_test_copy_visitor_t(&msg));
    v.visit(union_test_copy_visitor_t(&msg));
    EXPECT_EQ("notnull notnull ", msg);

    u_t w;
    msg.clear();
    w.visit(union_test_copy_visitor_t(&msg));
    EXPECT_EQ("null ", msg);

    w = "hey";
    msg.clear();
    w.visit(union_test_copy_visitor_t(&msg));
    EXPECT_EQ("string <hey> ", msg);

    v = w;
    msg.clear();
    v.visit(union_test_copy_visitor_t(&msg));
    w.visit(union_test_copy_visitor_t(&msg));
    EXPECT_EQ("string <hey> string <hey> ", msg);

    v = std::move(u);
    msg.clear();
    v.visit(union_test_copy_visitor_t(&msg));
    u.visit(union_test_copy_visitor_t(&msg));
    EXPECT_EQ("notnull null ", msg);

    EXPECT_EQ(nullptr, get<double>(&w));
    EXPECT_EQ("hey", std::string(get<const char *>(w)));
    EXPECT_NE(nullptr, get<const char *>(&w));
}


}  // namespace unittest
