// Copyright 2026 RethinkDB contributors, all rights reserved.
#include "rdb_protocol/terms/generated_columns.hpp"

#include <string>

#include "clustering/administration/admin_op_exc.hpp"
#include "rdb_protocol/error.hpp"
#include "rdb_protocol/func.hpp"
#include "rdb_protocol/op.hpp"
#include "rdb_protocol/real_table.hpp"
#include "rdb_protocol/term_walker.hpp"
#include "rdb_protocol/terms/terms.hpp"

namespace ql {

/* Build a wire-func map from a raw object term of the form
   {column_name: func}. Values must be FUNC terms; we compile each one
   directly from its raw term (evaluating the object would fail because
   MAKE_OBJ converts values to datums). */
std::map<std::string, wire_func_t> parse_generated_columns_raw(
        const raw_term_t &obj_term) {
    std::map<std::string, wire_func_t> out;
    rcheck_src(ql::backtrace_id_t::empty(),
               obj_term.type() == Term::MAKE_OBJ,
               base_exc_t::LOGIC,
               "Generated columns must be specified as an object mapping column "
               "names to functions.");
    obj_term.each_optarg([&](const raw_term_t &value, const std::string &name) {
            rcheck_src(ql::backtrace_id_t::empty(),
                       !name.empty(),
                       base_exc_t::LOGIC,
                       "Generated column names must be non-empty.");
            // Compile the value as a function of the row. Driver lambdas
            // arrive as FUNC terms (use their own parameter list); raw
            // expressions are bound as a 1-arg function over the row.
            wire_func_t wf;
            if (value.type() == Term::FUNC) {
                // Mirror func_term_t's argument parsing (func.cc):
                // FUNC takes exactly two args: a literal var-array and a body.
                rcheck_src(ql::backtrace_id_t::empty(),
                           value.num_args() == 2,
                           base_exc_t::LOGIC,
                           "FUNC takes exactly two arguments.");
                std::vector<sym_t> args;
                raw_term_t vars = value.arg(0);
                if (vars.type() == Term::DATUM) {
                    datum_t d = vars.datum();
                    rcheck_src(ql::backtrace_id_t::empty(),
                               d.get_type() == datum_t::type_t::R_ARRAY,
                               base_exc_t::LOGIC,
                               "FUNC variables must be a literal *array* of "
                               "numbers.");
                    for (size_t i = 0; i < d.arr_size(); ++i) {
                        datum_t dnum = d.get(i);
                        rcheck_src(ql::backtrace_id_t::empty(),
                                   dnum.get_type() == datum_t::type_t::R_NUM,
                                   base_exc_t::LOGIC,
                                   "FUNC variables must be a literal array of "
                                   "*numbers*.");
                        args.push_back(sym_t(dnum.as_num()));
                    }
                } else if (vars.type() == Term::MAKE_ARRAY) {
                    for (size_t i = 0; i < vars.num_args(); ++i) {
                        raw_term_t v = vars.arg(i);
                        rcheck_src(ql::backtrace_id_t::empty(),
                                   v.type() == Term::DATUM,
                                   base_exc_t::LOGIC,
                                   "FUNC variables must be a *literal* array of "
                                   "numbers.");
                        datum_t d = v.datum();
                        rcheck_src(ql::backtrace_id_t::empty(),
                                   d.get_type() == datum_t::type_t::R_NUM,
                                   base_exc_t::LOGIC,
                                   "FUNC variables must be a literal array of "
                                   "*numbers*.");
                        args.push_back(sym_t(d.as_num()));
                    }
                } else {
                    rfail_src(ql::backtrace_id_t::empty(), base_exc_t::LOGIC,
                              "FUNC variables must be a *literal array of "
                              "numbers*.");
                }
                wf = wire_func_t(value.arg(1), args);
            } else {
                wf = wire_func_t(value, std::vector<sym_t>{sym_t(0)});
            }
            wf.compile_wire_func()->assert_deterministic(
                constant_now_t::no,
                "Generated column functions must be deterministic.");
            optional<size_t> arity = wf.compile_wire_func()->arity();
            rcheck_src(ql::backtrace_id_t::empty(),
                       static_cast<bool>(arity)
                           && (arity.get() == 1 || arity.get() == 0),
                       base_exc_t::LOGIC,
                       strprintf("Generated column `%s` must expect 1 argument "
                                 "(the row).", name.c_str()));
            auto res = out.insert(std::make_pair(name, wf));
            rcheck_src(ql::backtrace_id_t::empty(),
                       res.second,
                       base_exc_t::LOGIC,
                       strprintf("Duplicate generated column name: %s.",
                                 name.c_str()));
        });
    return out;
}

/* Format the map back into a datum for `get_generated_columns`. */
datum_t format_generated_columns_datum(
        const std::map<std::string, wire_func_t> &config) {
    datum_object_builder_t builder;
    for (const auto &pair : config) {
        std::string query = "r.row(function(row) { return "
            + pair.second.compile_wire_func()->print_js_function()
            + "; })";
        UNUSED bool b = builder.add(
            datum_string_t(pair.first), datum_t(datum_string_t(query)));
        r_sanity_check(!b);
    }
    return std::move(builder).to_datum();
}

class set_generated_columns_term_t : public op_term_t {
public:
    set_generated_columns_term_t(compile_env_t *env, const raw_term_t &term)
        : op_term_t(env, term, argspec_t(2)) { }

    deterministic_t is_deterministic() const final {
        return deterministic_t::no();
    }

    virtual scoped_ptr_t<val_t> eval_impl(
        scope_env_t *env, args_t *args, eval_flags_t) const {
        counted_t<table_t> table = args->arg(env, 0)->as_table();

        // Parse the generated-columns object from its raw term, so FUNC
        // values are compiled (not evaluated to datums).
        std::map<std::string, wire_func_t> config =
            parse_generated_columns_raw(get_src().arg(1));

        try {
            admin_err_t err;
            if (!env->env->reql_cluster_interface()->set_generated_columns(
                    env->env->get_user_context(),
                    table->db,
                    name_string_t::guarantee_valid(table->name.c_str()),
                    config,
                    env->env->interruptor,
                    &err)) {
                REQL_RETHROW(err);
            }
        } catch (auth::permission_error_t const &permission_error) {
            rfail(ql::base_exc_t::PERMISSION_ERROR, "%s", permission_error.what());
        }

        ql::datum_object_builder_t res;
        res.overwrite(config.empty() ? "dropped" : "created", datum_t(1.0));
        return new_val(std::move(res).to_datum());
    }

    virtual const char *name() const { return "set_generated_columns"; }
};

class get_generated_columns_term_t : public op_term_t {
public:
    get_generated_columns_term_t(compile_env_t *env, const raw_term_t &term)
        : op_term_t(env, term, argspec_t(1)) { }

    deterministic_t is_deterministic() const final {
        return deterministic_t::no();
    }

    virtual scoped_ptr_t<val_t> eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const {
        counted_t<table_t> table = args->arg(env, 0)->as_table();

        std::map<std::string, wire_func_t> config;
        try {
            admin_err_t error;
            if (!env->env->reql_cluster_interface()->get_generated_columns(
                    env->env->get_user_context(),
                    table->db,
                    name_string_t::guarantee_valid(table->name.c_str()),
                    env->env->interruptor,
                    &config,
                    &error)) {
                REQL_RETHROW(error);
            }
        } catch (auth::permission_error_t const &permission_error) {
            rfail(ql::base_exc_t::PERMISSION_ERROR, "%s", permission_error.what());
        }
        return new_val(format_generated_columns_datum(config));
    }

    virtual const char *name() const { return "get_generated_columns"; }
};

counted_t<term_t> make_set_generated_columns_term(
        compile_env_t *env, const raw_term_t &term) {
    return make_counted<set_generated_columns_term_t>(env, term);
}
counted_t<term_t> make_get_generated_columns_term(
        compile_env_t *env, const raw_term_t &term) {
    return make_counted<get_generated_columns_term_t>(env, term);
}

} // namespace ql
