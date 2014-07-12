// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "rdb_protocol/env.hpp"

#include "errors.hpp"
#include <boost/bind.hpp>

#include "clustering/administration/database_metadata.hpp"
#include "clustering/administration/metadata.hpp"
#include "concurrency/cross_thread_watchable.hpp"
#include "extproc/js_runner.hpp"
#include "rdb_protocol/counted_term.hpp"
#include "rdb_protocol/func.hpp"
#include "rdb_protocol/minidriver.hpp"
#include "rdb_protocol/term_walker.hpp"

namespace ql {

/* Checks that divisor is indeed a divisor of multiple. */
template <class T>
bool is_joined(const T &multiple, const T &divisor) {
    T cpy = multiple;

    semilattice_join(&cpy, divisor);
    return cpy == multiple;
}

counted_t<const datum_t> static_optarg(const std::string &key, protob_t<Query> q) {
    // need to parse these to figure out what user wants; resulting
    // bootstrap problem is a headache.  Just use default.
    const configured_limits_t limits;
    for (int i = 0; i < q->global_optargs_size(); ++i) {
        const Query::AssocPair &ap = q->global_optargs(i);
        if (ap.key() == key && ap.val().type() == Term_TermType_DATUM) {
            return make_counted<const datum_t>(&ap.val().datum(), limits);
        }
    }

    return counted_t<const datum_t>();
}

wire_func_t construct_optarg_wire_func(const Term &val) {
    protob_t<Term> arg = r::fun(r::expr(val)).release_counted();
    propagate_backtrace(arg.get(), &val.GetExtension(ql2::extension::backtrace));

    compile_env_t empty_compile_env((var_visibility_t()));
    counted_t<func_term_t> func_term
        = make_counted<func_term_t>(&empty_compile_env, arg);
    counted_t<func_t> func = func_term->eval_to_func(var_scope_t());
    return wire_func_t(func);
}

std::map<std::string, wire_func_t> global_optargs(protob_t<Query> q) {
    rassert(q.has());

    Term *t = q->mutable_query();
    preprocess_term(t);

    std::map<std::string, wire_func_t> optargs;

    for (int i = 0; i < q->global_optargs_size(); ++i) {
        const Query::AssocPair &ap = q->global_optargs(i);
        auto insert_res
            = optargs.insert(std::make_pair(ap.key(),
                                            construct_optarg_wire_func(ap.val())));
        if (!insert_res.second) {
            rfail_toplevel(
                    base_exc_t::GENERIC,
                    "Duplicate global optarg: %s", ap.key().c_str());
        }
    }

    // Supply a default db of "test" if there is no "db" optarg.
    if (!optargs.count("db")) {
        Term arg = r::db("test").get();
        Backtrace *t_bt = t->MutableExtension(ql2::extension::backtrace);
        propagate_backtrace(&arg, t_bt); // duplicate toplevel backtrace
        optargs["db"] = construct_optarg_wire_func(arg);
    }

    return optargs;
}

global_optargs_t::global_optargs_t() { }

global_optargs_t::global_optargs_t(std::map<std::string, wire_func_t> _optargs)
    : optargs(_optargs) { }

void global_optargs_t::init_optargs(
    const std::map<std::string, wire_func_t> &_optargs) {
    r_sanity_check(optargs.size() == 0);
    optargs = _optargs;
}

counted_t<val_t> global_optargs_t::get_optarg(env_t *env, const std::string &key){
    if (!optargs.count(key)) {
        return counted_t<val_t>();
    }
    return optargs[key].compile_wire_func()->call(env);
}
const std::map<std::string, wire_func_t> &global_optargs_t::get_all_optargs() {
    return optargs;
}

void env_t::set_eval_callback(eval_callback_t *callback) {
    eval_callback = callback;
}

void env_t::do_eval_callback() {
    if (eval_callback != NULL) {
        eval_callback->eval_callback();
    }
}

profile_bool_t env_t::profile() const {
    return trace.has() ? profile_bool_t::PROFILE : profile_bool_t::DONT_PROFILE;
}

void env_t::join_and_wait_to_propagate(
        const cluster_semilattice_metadata_t &metadata_to_join)
    THROWS_ONLY(interrupted_exc_t) {
    r_sanity_check(rdb_ctx->cluster_metadata);
    rdb_ctx->cluster_metadata->assert_thread();
    rdb_ctx->cluster_metadata->join(metadata_to_join);
    cluster_semilattice_metadata_t sl_metadata
        = rdb_ctx->cluster_metadata->get();

    {
        on_thread_t switcher(home_thread());
        clone_ptr_t<watchable_t<cow_ptr_t<namespaces_semilattice_metadata_t> > >
            ns_watchable = rdb_ctx->get_namespaces_watchable();

        ns_watchable->run_until_satisfied(
                boost::bind(&is_joined<cow_ptr_t<namespaces_semilattice_metadata_t > >,
                      _1,
                      sl_metadata.rdb_namespaces),
                interruptor);

        clone_ptr_t< watchable_t<databases_semilattice_metadata_t> >
            db_watchable = rdb_ctx->get_databases_watchable();
        db_watchable->run_until_satisfied(
                boost::bind(&is_joined<databases_semilattice_metadata_t>,
                            _1,
                            sl_metadata.databases),
                interruptor);
    }
}

base_namespace_repo_t *env_t::ns_repo() {
    r_sanity_check(rdb_ctx != NULL);
    return rdb_ctx->ns_repo;
}

const boost::shared_ptr< semilattice_readwrite_view_t<
                             cluster_semilattice_metadata_t> > &
env_t::cluster_metadata() {
    r_sanity_check(rdb_ctx != NULL);
    r_sanity_check(rdb_ctx->cluster_metadata);
    return rdb_ctx->cluster_metadata;
}

directory_read_manager_t<cluster_directory_metadata_t> *
env_t::directory_read_manager() {
    r_sanity_check(rdb_ctx != NULL);
    r_sanity_check(rdb_ctx->directory_read_manager != NULL);
    return rdb_ctx->directory_read_manager;
}

uuid_u env_t::this_machine() {
    r_sanity_check(rdb_ctx != NULL);
    r_sanity_check(!rdb_ctx->machine_id.is_unset());
    return rdb_ctx->machine_id;
}

changefeed::client_t *env_t::get_changefeed_client() {
    r_sanity_check(rdb_ctx != NULL);
    r_sanity_check(rdb_ctx->changefeed_client.has());
    return rdb_ctx->changefeed_client.get();
}

std::string env_t::get_reql_http_proxy() {
    r_sanity_check(rdb_ctx != NULL);
    return rdb_ctx->reql_http_proxy;
}

extproc_pool_t *env_t::get_extproc_pool() {
    assert_thread();
    r_sanity_check(rdb_ctx != NULL);
    r_sanity_check(rdb_ctx->extproc_pool != NULL);
    return rdb_ctx->extproc_pool;
}

js_runner_t *env_t::get_js_runner() {
    assert_thread();
    extproc_pool_t *extproc_pool = get_extproc_pool();
    if (!js_runner.connected()) {
        js_runner.begin(extproc_pool, interruptor);
    }
    return &js_runner;
}

cow_ptr_t<namespaces_semilattice_metadata_t>
env_t::get_namespaces_metadata() {
    return rdb_ctx->get_namespaces_metadata();
}

void env_t::get_databases_metadata(databases_semilattice_metadata_t *out) {
    rdb_ctx->get_databases_metadata(out);
}


env_t::env_t(rdb_context_t *ctx, signal_t *_interruptor,
             std::map<std::string, wire_func_t> optargs,
             profile_bool_t profile)
    : evals_since_yield(0),
      global_optargs(std::move(optargs)),
      interruptor(_interruptor),
      trace(profile == profile_bool_t::PROFILE
            ? make_scoped<profile::trace_t>()
            : scoped_ptr_t<profile::trace_t>()),
      rdb_ctx(ctx),
      eval_callback(NULL) {
    rassert(ctx != NULL);
    rassert(interruptor != NULL);
}


// Used in constructing the env for rdb_update_single_sindex and many unit tests.
env_t::env_t(signal_t *_interruptor)
    : evals_since_yield(0),
      global_optargs(),
      interruptor(_interruptor),
      trace(),
      rdb_ctx(NULL),
      eval_callback(NULL) {
    rassert(interruptor != NULL);
}

profile_bool_t profile_bool_optarg(const protob_t<Query> &query) {
    rassert(query.has());
    counted_t<const datum_t> profile_arg = static_optarg("profile", query);
    if (profile_arg.has() && profile_arg->get_type() == datum_t::type_t::R_BOOL &&
        profile_arg->as_bool()) {
        return profile_bool_t::PROFILE;
    } else {
        return profile_bool_t::DONT_PROFILE;
    }
}

env_t::~env_t() { }

void env_t::maybe_yield() {
    if (++evals_since_yield > EVALS_BEFORE_YIELD) {
        evals_since_yield = 0;
        coro_t::yield();
    }
}


} // namespace ql
