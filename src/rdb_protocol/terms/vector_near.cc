// Copyright 2026 RethinkDB, all rights reserved.
#include "rdb_protocol/terms/terms.hpp"

#include <string>
#include <vector>

#include "clustering/administration/admin_op_exc.hpp"
#include "rdb_protocol/datum.hpp"
#include "rdb_protocol/env.hpp"
#include "rdb_protocol/error.hpp"
#include "rdb_protocol/func.hpp"
#include "rdb_protocol/op.hpp"
#include "rdb_protocol/val.hpp"

namespace ql {

/* `vector_near` queries a vector index for nearest neighbor search.
 * Usage: r.table("items").vector_near("my_vector_idx",
 *                                       r.vector([1.0, 2.0, 3.0]),
 *                                       {k: 10})
 * Returns an array of matching documents, or empty array for now (VECTOR-7). */
class vector_near_term_t : public op_term_t {
public:
    vector_near_term_t(compile_env_t *env, const raw_term_t &term)
        : op_term_t(env, term, argspec_t(3, 3), optargspec_t({"k"})) { }

    virtual scoped_ptr_t<val_t> eval_impl(
        scope_env_t *env, args_t *args, eval_flags_t) const {
        /* Parse arguments */
        counted_t<table_t> table = args->arg(env, 0)->as_table();
        std::string sindex_name = args->arg(env, 1)->as_str().to_std();

        /* Third argument is the query vector (from r.vector() call) */
        scoped_ptr_t<val_t> vector_val = args->arg(env, 2);
        datum_t vector_datum;
        if (vector_val->get_type().is_convertible(val_t::type_t::DATUM)) {
            vector_datum = vector_val->as_datum();
        } else {
            /* If it's a function, evaluate it to get the datum */
            counted_t<const func_t> func = vector_val->as_func();
            vector_datum = func->call(env->env)->as_datum();
        }

        rcheck(vector_datum.get_type() == datum_t::R_ARRAY
               || vector_datum.get_type() == datum_t::R_VECTOR,
               base_exc_t::LOGIC,
               "The third argument to `vector_near` must be a vector (from `r.vector()`) "
               "or an array of numbers.");

        /* Parse k from optargs (default 10) */
        size_t k = 10;
        if (scoped_ptr_t<val_t> k_val = args->optarg(env, "k")) {
            double k_double = k_val->as_num();
            rcheck(k_double >= 1 && k_double <= 10000,
                   base_exc_t::LOGIC,
                   "`k` must be a number between 1 and 10000.");
            k = static_cast<size_t>(k_double);
        }
        (void)k;          // used in VECTOR-7

        /* Verify the sindex exists and is a vector type. */
        std::map<std::string, std::pair<sindex_config_t, sindex_status_t> >
            configs_and_statuses;
        admin_err_t error;
        if (!env->env->reql_cluster_interface()->sindex_list(
                table->db, name_string_t::guarantee_valid(table->name.c_str()),
                env->env->interruptor, &error, &configs_and_statuses)) {
            REQL_RETHROW(error);
        }

        auto it = configs_and_statuses.find(sindex_name);
        rcheck(it != configs_and_statuses.end(),
               base_exc_t::LOGIC,
               strprintf("Index `%s` was not found on table `%s`.",
                         sindex_name.c_str(),
                         table->display_name().c_str()));

        rcheck(it->second.first.vector == sindex_vector_bool_t::VECTOR,
               base_exc_t::LOGIC,
               strprintf("Index `%s` is not a vector index on table `%s`. "
                         "Use `indexCreate` with `{vector: {dim: ..., metric: ...}}`.",
                         sindex_name.c_str(),
                         table->display_name().c_str()));

        /* Placeholder: return empty array.
         * Actual vector search dispatch will be wired in VECTOR-7. */
        ql::datum_array_builder_t res(ql::configured_limits_t::unlimited);
        return new_val(std::move(res).to_datum());
    }

    virtual const char *name() const { return "vector_near"; }
};

counted_t<term_t> make_vector_near_term(
        compile_env_t *env, const raw_term_t &term) {
    return make_counted<vector_near_term_t>(env, term);
}

} // namespace ql
