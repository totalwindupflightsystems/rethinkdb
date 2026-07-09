// Copyright 2026 RethinkDB, all rights reserved.
#include "rdb_protocol/terms/terms.hpp"

#include <string>
#include <vector>

#include "rdb_protocol/datum_stream.hpp"
#include "rdb_protocol/error.hpp"
#include "rdb_protocol/fts_tokenizer.hpp"
#include "rdb_protocol/op.hpp"
#include "rdb_protocol/val.hpp"

namespace ql {

/* `fts_tokenize` wraps the FTS tokenizer for use from ReQL queries.
 * Usage: r.fts_tokenize("hello world")  ->  ["hello", "world"] */
class fts_tokenize_term_t : public op_term_t {
public:
    fts_tokenize_term_t(compile_env_t *env, const raw_term_t &term)
        : op_term_t(env, term, argspec_t(1)) { }

    virtual scoped_ptr_t<val_t> eval_impl(
        scope_env_t *env, args_t *args, eval_flags_t) const {
        datum_t text = args->arg(env, 0)->as_datum();
        rcheck(text.get_type() == datum_t::R_STR,
               base_exc_t::LOGIC,
               "`fts_tokenize` requires a string argument.");

        fts_tokenizer_t tokenizer;
        std::vector<std::string> tokens = tokenizer.tokenize(text.as_str().to_std());

        ql::datum_array_builder_t res(ql::configured_limits_t::unlimited);
        for (const auto &token : tokens) {
            res.add(ql::datum_t(datum_string_t(token)));
        }
        return new_val(std::move(res).to_datum());
    }

    virtual const char *name() const { return "fts_tokenize"; }
};

/* `fts_match` queries an FTS index against a table.
 * Usage: r.table("posts").fts_match("search query", {index: "my_fts_idx"}) */
class fts_match_term_t : public op_term_t {
public:
    fts_match_term_t(compile_env_t *env, const raw_term_t &term)
        : op_term_t(env, term, argspec_t(2), optargspec_t({ "index" })) { }

    virtual scoped_ptr_t<val_t> eval_impl(
        scope_env_t *env, args_t *args, eval_flags_t) const {
        counted_t<table_t> table = args->arg(env, 0)->as_table();
        datum_t query_datum = args->arg(env, 1)->as_datum();

        rcheck(query_datum.get_type() == datum_t::R_STR,
               base_exc_t::LOGIC,
               "`fts_match` requires a string query.");

        /* Get the sindex name from the optarg, or use a default FTS index name. */
        scoped_ptr_t<val_t> index_val = args->optarg(env, "index");
        std::string sindex_id;
        if (index_val.has()) {
            sindex_id = index_val->as_str().to_std();
        }

        /* Tokenize the query string. */
        fts_tokenizer_t tokenizer;
        std::vector<std::string> tokens = tokenizer.tokenize(
            query_datum.as_str().to_std());

        /* For now, FTS matching uses get_all with multiple keys.
         * Each token is looked up in the FTS sindex.  The multi-index
         * nature of FTS sindexes means each document matching any
         * token is returned. */
        if (tokens.empty()) {
            // Empty query: return empty array.
            ql::datum_array_builder_t res(ql::configured_limits_t::unlimited);
            return new_val(std::move(res).to_datum());
        }

        // Build a keyset from the tokenized query terms.
        std::map<datum_t, uint64_t> keys;
        for (const auto &token : tokens) {
            datum_t key = ql::datum_t(datum_string_t(token));
            keys.insert(std::make_pair(std::move(key), 0)).first->second += 1;
        }

        return new_val(
            make_counted<selection_t>(
                table,
                table->get_all(env->env,
                               datumspec_t(std::move(keys)),
                               sindex_id,
                               backtrace())));
    }

    virtual const char *name() const { return "fts_match"; }
};

counted_t<term_t> make_fts_tokenize_term(
        compile_env_t *env, const raw_term_t &term) {
    return make_counted<fts_tokenize_term_t>(env, term);
}

counted_t<term_t> make_fts_match_term(
        compile_env_t *env, const raw_term_t &term) {
    return make_counted<fts_match_term_t>(env, term);
}

} // namespace ql
