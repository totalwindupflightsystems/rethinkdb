// Copyright 2026 RethinkDB, all rights reserved.
#include "rdb_protocol/terms/time_series.hpp"

#include <cmath>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "btree/time_series_ops.hpp"
#include "rdb_protocol/error.hpp"
#include "rdb_protocol/func.hpp"
#include "rdb_protocol/term_walker.hpp"

namespace ql {

namespace {

/* Extract a literal non-negative integer (seconds) from a raw-term value.
 * Accepts only DATUM literals; anything else is a structural error. */
uint64_t require_uint_seconds(const raw_term_t &value,
                              const rcheckable_t *target,
                              const std::string &what) {
    rcheck_target(target, value.type() == Term::DATUM,
                  base_exc_t::LOGIC,
                  strprintf("Time-series `%s` must be a number of seconds.",
                            what.c_str()));
    datum_t d = value.datum();
    rcheck_target(target, d.get_type() == datum_t::R_NUM,
                  base_exc_t::LOGIC,
                  strprintf("Time-series `%s` must be a number of seconds.",
                            what.c_str()));
    double num = d.as_num();
    rcheck_target(target,
                  num == std::floor(num) && num >= 0 && num < 1e18,
                  base_exc_t::LOGIC,
                  strprintf("Time-series `%s` must be a non-negative integer "
                            "number of seconds.", what.c_str()));
    return static_cast<uint64_t>(num);
}

/* Parse a downsample `aggregate` object: {column_name: ReQL expression}.
 * Values are compiled as functions over the row batch without evaluating
 * the object (evaluating would fail — MAKE_OBJ converts values to datums).
 * Mirrors parse_generated_columns_raw with the time-series error surface. */
std::map<name_string_t, wire_func_t> parse_aggregates(
        const raw_term_t &agg_term, const rcheckable_t *target) {
    std::map<name_string_t, wire_func_t> out;
    rcheck_target(target, agg_term.type() == Term::MAKE_OBJ,
                  base_exc_t::LOGIC,
                  "Downsample `aggregate` must be an object mapping column "
                  "names to ReQL expressions.");
    agg_term.each_optarg([&](const raw_term_t &value, const std::string &name) {
            rcheck_target(target, !name.empty(),
                          base_exc_t::LOGIC,
                          "Downsample aggregate column names must be "
                          "non-empty.");
            wire_func_t wf;
            if (value.type() == Term::FUNC) {
                /* Mirror func_term_t's argument parsing (func.cc): FUNC
                 * takes exactly two args: a literal var-array and a body. */
                rcheck_target(target, value.num_args() == 2,
                              base_exc_t::LOGIC,
                              "FUNC takes exactly two arguments.");
                std::vector<sym_t> args;
                raw_term_t vars = value.arg(0);
                if (vars.type() == Term::DATUM) {
                    datum_t d = vars.datum();
                    rcheck_target(target,
                                  d.get_type() == datum_t::type_t::R_ARRAY,
                                  base_exc_t::LOGIC,
                                  "FUNC variables must be a literal *array* "
                                  "of numbers.");
                    for (size_t i = 0; i < d.arr_size(); ++i) {
                        datum_t dnum = d.get(i);
                        rcheck_target(target,
                                      dnum.get_type() == datum_t::type_t::R_NUM,
                                      base_exc_t::LOGIC,
                                      "FUNC variables must be a literal array "
                                      "of *numbers*.");
                        args.push_back(sym_t(dnum.as_num()));
                    }
                } else if (vars.type() == Term::MAKE_ARRAY) {
                    for (size_t i = 0; i < vars.num_args(); ++i) {
                        raw_term_t v = vars.arg(i);
                        rcheck_target(target, v.type() == Term::DATUM,
                                      base_exc_t::LOGIC,
                                      "FUNC variables must be a *literal* "
                                      "array of numbers.");
                        datum_t d = v.datum();
                        rcheck_target(target,
                                      d.get_type() == datum_t::type_t::R_NUM,
                                      base_exc_t::LOGIC,
                                      "FUNC variables must be a literal array "
                                      "of *numbers*.");
                        args.push_back(sym_t(d.as_num()));
                    }
                } else {
                    rfail_target(target, base_exc_t::LOGIC,
                                 "FUNC variables must be a *literal array of "
                                 "numbers*.");
                }
                wf = wire_func_t(value.arg(1), args);
            } else {
                wf = wire_func_t(value, std::vector<sym_t>{sym_t(0)});
            }
            wf.compile_wire_func()->assert_deterministic(
                constant_now_t::no,
                "Downsample aggregate expression must be deterministic.");
            optional<size_t> arity = wf.compile_wire_func()->arity();
            rcheck_target(target,
                          static_cast<bool>(arity) && arity.get() == 1,
                          base_exc_t::LOGIC,
                          strprintf("Downsample aggregate `%s` must expect 1 "
                                    "argument (the row batch).", name.c_str()));
            auto res = out.insert(std::make_pair(
                name_string_t::guarantee_valid(name.c_str()), wf));
            rcheck_target(target, res.second,
                          base_exc_t::LOGIC,
                          strprintf("Duplicate downsample aggregate column: "
                                    "%s.", name.c_str()));
        });
    rcheck_target(target, !out.empty(),
                  base_exc_t::LOGIC,
                  "Downsample `aggregate` must not be empty.");
    return out;
}

}  // namespace

time_series_config_t parse_time_series_config_from_raw_term(
        const raw_term_t &ts_term, const rcheckable_t *target) {
    rcheck_target(target, ts_term.type() == Term::MAKE_OBJ,
                  base_exc_t::LOGIC,
                  "`timeSeries` must be an object with a `field` key.");

    time_series_config_t config;
    config.enabled = true;

    optional<raw_term_t> field_term = ts_term.optarg("field");
    rcheck_target(target, field_term.has_value(),
                  base_exc_t::LOGIC,
                  "Time-series config requires a `field` key.");
    rcheck_target(target, field_term->type() == Term::DATUM,
                  base_exc_t::LOGIC,
                  "Time-series `field` must be a string.");
    datum_t field_datum = field_term->datum();
    rcheck_target(target, field_datum.get_type() == datum_t::R_STR,
                  base_exc_t::LOGIC,
                  "Time-series `field` must be a string.");
    std::string field_name = field_datum.as_str().to_std();
    rcheck_target(target, !field_name.empty(),
                  base_exc_t::LOGIC,
                  "Time-series field must be a non-empty string.");
    config.time_field = name_string_t::guarantee_valid(field_name.c_str());

    if (optional<raw_term_t> ci_term = ts_term.optarg("chunk_interval")) {
        config.chunk_interval_seconds =
            require_uint_seconds(*ci_term, target, "chunk_interval");
        rcheck_target(target, config.chunk_interval_seconds >= 1,
                      base_exc_t::LOGIC,
                      "Time-series `chunk_interval` must be at least 1 "
                      "second.");
    }

    if (optional<raw_term_t> ret_term = ts_term.optarg("retention")) {
        config.retention_seconds =
            require_uint_seconds(*ret_term, target, "retention");
    }

    if (optional<raw_term_t> ds_term = ts_term.optarg("downsample")) {
        rcheck_target(target, ds_term->type() == Term::MAKE_ARRAY,
                      base_exc_t::LOGIC,
                      "Downsample must be an array of {age, to, aggregate} "
                      "objects.");
        for (size_t i = 0; i < ds_term->num_args(); ++i) {
            raw_term_t step_term = ds_term->arg(i);
            rcheck_target(target, step_term.type() == Term::MAKE_OBJ,
                          base_exc_t::LOGIC,
                          "Each downsample step must be an object with `age`, "
                          "`to`, and `aggregate`.");
            downsample_step_t step;
            optional<raw_term_t> age_term = step_term.optarg("age");
            rcheck_target(target, age_term.has_value(),
                          base_exc_t::LOGIC,
                          "Each downsample step requires an `age`.");
            step.age_seconds = require_uint_seconds(*age_term, target, "age");
            optional<raw_term_t> to_term = step_term.optarg("to");
            rcheck_target(target, to_term.has_value(),
                          base_exc_t::LOGIC,
                          "Each downsample step requires a `to`.");
            step.target_interval_seconds =
                require_uint_seconds(*to_term, target, "to");
            optional<raw_term_t> agg_term = step_term.optarg("aggregate");
            rcheck_target(target, agg_term.has_value(),
                          base_exc_t::LOGIC,
                          "Each downsample step requires an `aggregate` "
                          "object.");
            step.aggregates = parse_aggregates(*agg_term, target);
            config.downsample_steps.push_back(std::move(step));
        }
    }

    config.validate_or_throw();
    return config;
}

datum_t format_time_series_config_datum(const time_series_config_t &config) {
    datum_object_builder_t builder;
    builder.overwrite("field",
        datum_t(datum_string_t(config.time_field.str())));
    builder.overwrite("chunk_interval",
        datum_t(static_cast<double>(config.chunk_interval_seconds)));
    builder.overwrite("retention",
        datum_t(static_cast<double>(config.retention_seconds)));
    datum_array_builder_t steps(configured_limits_t::unlimited);
    for (const downsample_step_t &step : config.downsample_steps) {
        datum_object_builder_t step_builder;
        step_builder.overwrite("age",
            datum_t(static_cast<double>(step.age_seconds)));
        step_builder.overwrite("to",
            datum_t(static_cast<double>(step.target_interval_seconds)));
        datum_object_builder_t agg_builder;
        for (const auto &pair : step.aggregates) {
            std::string query = "r.row(function(row) { return "
                + pair.second.compile_wire_func()->print_js_function()
                + "; })";
            UNUSED bool b = agg_builder.add(
                datum_string_t(pair.first.str()),
                datum_t(datum_string_t(query)));
            r_sanity_check(!b);
        }
        step_builder.overwrite("aggregate", std::move(agg_builder).to_datum());
        steps.add(std::move(step_builder).to_datum());
    }
    builder.overwrite("downsample", std::move(steps).to_datum());
    return std::move(builder).to_datum();
}

datum_t format_time_series_chunk_info_datum(
        const time_chunk_index_t &chunk_index, bool has_catalog) {
    if (!has_catalog) {
        return datum_t::null();
    }

    datum_object_builder_t builder;
    builder.overwrite("chunk_count",
        datum_t(static_cast<double>(chunk_index.chunks.size())));
    builder.overwrite("total_rows",
        datum_t(static_cast<double>(time_series_ops_t::total_rows(chunk_index))));

    datum_array_builder_t chunks(configured_limits_t::unlimited);
    for (const time_chunk_bounds_t &c : chunk_index.chunks) {
        datum_object_builder_t cb;
        cb.overwrite("min_time_us",
            datum_t(static_cast<double>(c.min_time_us)));
        cb.overwrite("max_time_us",
            datum_t(static_cast<double>(c.max_time_us)));
        cb.overwrite("row_count",
            datum_t(static_cast<double>(c.row_count)));
        chunks.add(std::move(cb).to_datum());
    }

    if (chunk_index.chunks.empty()) {
        builder.overwrite("newest", datum_t::null());
    } else {
        const time_chunk_bounds_t &newest = chunk_index.chunks.back();
        datum_object_builder_t nb;
        nb.overwrite("min_time_us",
            datum_t(static_cast<double>(newest.min_time_us)));
        nb.overwrite("max_time_us",
            datum_t(static_cast<double>(newest.max_time_us)));
        nb.overwrite("row_count",
            datum_t(static_cast<double>(newest.row_count)));
        builder.overwrite("newest", std::move(nb).to_datum());
    }

    builder.overwrite("chunks", std::move(chunks).to_datum());
    return std::move(builder).to_datum();
}

bool time_series_reconfigure_allows_retention_change(
        const datum_t &old_datum, const datum_t &new_datum) {
    if (old_datum == new_datum) {
        return true;
    }
    if (!old_datum.has() || !new_datum.has()
            || old_datum.get_type() != datum_t::R_OBJECT
            || new_datum.get_type() != datum_t::R_OBJECT) {
        return false;
    }
    /* Same key set, and every key except `retention` must hold the same
     * value. The key-count check rejects datums that dropped `retention`
     * or carry extra keys. */
    if (old_datum.obj_size() != new_datum.obj_size()) {
        return false;
    }
    const datum_string_t retention_key("retention");
    for (size_t i = 0; i < old_datum.obj_size(); ++i) {
        const datum_string_t &field = old_datum.get_pair(i).first;
        const datum_t new_val = new_datum.get_field(field, NOTHROW);
        if (!new_val.has()) {
            return false;
        }
        if (field == retention_key) {
            continue;
        }
        const datum_t old_val = old_datum.get_field(field, NOTHROW);
        if (old_val != new_val) {
            return false;
        }
    }
    return true;
}

}  // namespace ql
