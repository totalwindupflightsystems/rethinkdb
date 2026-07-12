// Copyright 2026 RethinkDB, all rights reserved.
#include "rdb_protocol/terms/terms.hpp"

#include <string>
#include <vector>

#include "rdb_protocol/datum.hpp"
#include "rdb_protocol/error.hpp"
#include "rdb_protocol/op.hpp"
#include "rdb_protocol/val.hpp"

namespace ql {

/* `vector` creates a VECTOR datum from an array of numbers.
 * Usage: r.vector([1.0, 2.0, 3.0])  ->  VECTOR([1.0, 2.0, 3.0]) */
class vector_term_t : public op_term_t {
public:
    vector_term_t(compile_env_t *env, const raw_term_t &term)
        : op_term_t(env, term, argspec_t(1)) { }

    virtual scoped_ptr_t<val_t> eval_impl(
        scope_env_t *env, args_t *args, eval_flags_t) const {
        datum_t arr = args->arg(env, 0)->as_datum();
        rcheck(arr.get_type() == datum_t::R_ARRAY,
               base_exc_t::LOGIC,
               "`r.vector` requires an array of numbers as its argument.");

        std::vector<double> vec;
        vec.reserve(arr.arr_size());
        for (size_t i = 0; i < arr.arr_size(); ++i) {
            datum_t element = arr.get(i);
            rcheck(element.get_type() == datum_t::R_NUM,
                   base_exc_t::LOGIC,
                   "`r.vector` requires an array containing only numbers.");
            vec.push_back(element.as_num());
        }
        return new_val(datum_t::vector(std::move(vec)));
    }

    virtual const char *name() const { return "vector"; }
};

counted_t<term_t> make_vector_term(
        compile_env_t *env, const raw_term_t &term) {
    return make_counted<vector_term_t>(env, term);
}

} // namespace ql
