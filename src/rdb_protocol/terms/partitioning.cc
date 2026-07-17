// Copyright 2026 RethinkDB, all rights reserved.
#include "rdb_protocol/terms/partitioning.hpp"

#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "clustering/administration/admin_op_exc.hpp"
#include "clustering/administration/auth/permission_error.hpp"
#include "containers/name_string.hpp"
#include "containers/uuid.hpp"
#include "rdb_protocol/env.hpp"
#include "rdb_protocol/error.hpp"
#include "rdb_protocol/op.hpp"
#include "rdb_protocol/partition_errors.hpp"
#include "rdb_protocol/terms/terms.hpp"
#include "rdb_protocol/val.hpp"

namespace ql {

namespace {

name_string_t parse_partition_name(
        rcheckable_t *target, const datum_t &name_d) {
    rcheck_target(target,
                  name_d.get_type() == datum_t::R_STR,
                  base_exc_t::LOGIC,
                  strprintf("Expected type STRING but found %s for partition name:\n%s%s",
                            name_d.get_type_name().c_str(),
                            name_d.print().c_str(),
                            partition_error::error_suffix(
                                partition_error::PARTITION_CONFIG_INVALID)
                                .c_str()));
    name_string_t name;
    bool ok = name.assign_value(name_d.as_str());
    rcheck_target(target, ok, base_exc_t::LOGIC,
                  strprintf("Partition name `%s` invalid (%s).%s",
                            name_d.as_str().to_std().c_str(),
                            name_string_t::valid_char_msg,
                            partition_error::error_suffix(
                                partition_error::PARTITION_CONFIG_INVALID)
                                .c_str()));
    return name;
}

void finalize_partition_entries(partition_config_t *config) {
    for (partition_entry_t &p : config->partitions) {
        if (p.id.is_nil()) {
            p.id = generate_uuid();
        }
        p.storage_id = nil_uuid();
        if (p.state == partition_state_t::CREATING) {
            p.state = partition_state_t::ACTIVE;
        }
    }
    if (config->epoch == 0 && config->is_partitioned()) {
        config->epoch = 1;
    }
}

}  // namespace

partition_config_t parse_partition_config_from_datum(
        const datum_t &d, rcheckable_t *target) {
    rcheck_target(target,
                  d.get_type() == datum_t::R_OBJECT,
                  base_exc_t::LOGIC,
                  strprintf("Expected type OBJECT but found %s for partitions config:\n%s%s",
                            d.get_type_name().c_str(),
                            d.print().c_str(),
                            partition_error::error_suffix(
                                partition_error::PARTITION_CONFIG_INVALID)
                                .c_str()));

    datum_t by_d = d.get_field("by", NOTHROW);
    rcheck_target(target, by_d.has() && by_d.get_type() == datum_t::R_STR,
                  base_exc_t::LOGIC,
                  std::string("Partition config requires a string `by` field "
                              "naming the partition key.")
                      + partition_error::error_suffix(
                          partition_error::PARTITION_CONFIG_INVALID));
    std::string key_field = by_d.as_str().to_std();
    rcheck_target(target, !key_field.empty(), base_exc_t::LOGIC,
                  std::string("Partition key field (`by`) must be a non-empty "
                              "string.")
                      + partition_error::error_suffix(
                          partition_error::PARTITION_CONFIG_INVALID));

    datum_t type_d = d.get_field("type", NOTHROW);
    rcheck_target(target, type_d.has() && type_d.get_type() == datum_t::R_STR,
                  base_exc_t::LOGIC,
                  std::string("Partition config requires a string `type` field "
                              "(`range`, `hash`, or `list`).")
                      + partition_error::error_suffix(
                          partition_error::PARTITION_CONFIG_INVALID));
    std::string type_str = type_d.as_str().to_std();

    partition_config_t config;
    config.key_field = key_field;
    config.epoch = 1;

    if (type_str == "range") {
        config.type = partition_type_t::RANGE;
        datum_t ranges_d = d.get_field("ranges", NOTHROW);
        rcheck_target(target,
                      ranges_d.has() && ranges_d.get_type() == datum_t::R_ARRAY,
                      base_exc_t::LOGIC,
                      std::string("Range partition config requires a `ranges` "
                                  "array.")
                          + partition_error::error_suffix(
                              partition_error::PARTITION_RANGE_INVALID));
        rcheck_target(target, ranges_d.arr_size() > 0, base_exc_t::LOGIC,
                      std::string("Range partition config must define at least "
                                  "one range.")
                          + partition_error::error_suffix(
                              partition_error::PARTITION_RANGE_INVALID));

        for (size_t i = 0; i < ranges_d.arr_size(); ++i) {
            datum_t entry = ranges_d.get(i);
            rcheck_target(target, entry.get_type() == datum_t::R_OBJECT,
                          base_exc_t::LOGIC,
                          std::string("Each range partition entry must be an "
                                      "object.")
                              + partition_error::error_suffix(
                                  partition_error::PARTITION_RANGE_INVALID));
            datum_t name_d = entry.get_field("name", NOTHROW);
            datum_t from_d = entry.get_field("from", NOTHROW);
            datum_t to_d = entry.get_field("to", NOTHROW);
            rcheck_target(target, name_d.has() && from_d.has() && to_d.has(),
                          base_exc_t::LOGIC,
                          std::string("Range partition entries require `name`, "
                                      "`from`, and `to`.")
                              + partition_error::error_suffix(
                                  partition_error::PARTITION_RANGE_INVALID));

            partition_entry_t part;
            part.name = parse_partition_name(target, name_d);
            part.state = partition_state_t::ACTIVE;
            config.partitions.push_back(std::move(part));

            if (i == 0) {
                config.range_boundaries.push_back(from_d);
            } else {
                /* Contiguous ranges: previous `to` must equal this `from`. */
                rcheck_target(target,
                              config.range_boundaries.back() == from_d,
                              base_exc_t::LOGIC,
                              std::string("Range partitions must be contiguous: "
                                          "each range's `from` must equal the "
                                          "previous range's `to`.")
                                  + partition_error::error_suffix(
                                      partition_error::PARTITION_RANGE_INVALID));
            }
            config.range_boundaries.push_back(to_d);
        }
    } else if (type_str == "hash") {
        config.type = partition_type_t::HASH;
        datum_t mod_d = d.get_field("modulus", NOTHROW);
        rcheck_target(target, mod_d.has() && mod_d.get_type() == datum_t::R_NUM,
                      base_exc_t::LOGIC,
                      std::string("Hash partition config requires a numeric "
                                  "`modulus`.")
                          + partition_error::error_suffix(
                              partition_error::PARTITION_HASH_INVALID));
        double mod_num = mod_d.as_num();
        rcheck_target(target,
                      mod_num == floor(mod_num) && mod_num > 0,
                      base_exc_t::LOGIC,
                      std::string("`modulus` must be a positive integer.")
                          + partition_error::error_suffix(
                              partition_error::PARTITION_HASH_INVALID));
        config.hash_modulus = static_cast<uint32_t>(mod_num);

        datum_t parts_d = d.get_field("partitions", NOTHROW);
        rcheck_target(target,
                      parts_d.has() && parts_d.get_type() == datum_t::R_ARRAY,
                      base_exc_t::LOGIC,
                      std::string("Hash partition config requires a "
                                  "`partitions` array.")
                          + partition_error::error_suffix(
                              partition_error::PARTITION_HASH_INVALID));
        for (size_t i = 0; i < parts_d.arr_size(); ++i) {
            datum_t entry = parts_d.get(i);
            rcheck_target(target, entry.get_type() == datum_t::R_OBJECT,
                          base_exc_t::LOGIC,
                          std::string("Each hash partition entry must be an "
                                      "object.")
                              + partition_error::error_suffix(
                                  partition_error::PARTITION_HASH_INVALID));
            datum_t name_d = entry.get_field("name", NOTHROW);
            datum_t buckets_d = entry.get_field("buckets", NOTHROW);
            rcheck_target(target, name_d.has() && buckets_d.has(),
                          base_exc_t::LOGIC,
                          std::string("Hash partition entries require `name` "
                                      "and `buckets`.")
                              + partition_error::error_suffix(
                                  partition_error::PARTITION_HASH_INVALID));
            rcheck_target(target, buckets_d.get_type() == datum_t::R_ARRAY,
                          base_exc_t::LOGIC,
                          std::string("`buckets` must be an array of integers.")
                              + partition_error::error_suffix(
                                  partition_error::PARTITION_HASH_INVALID));

            partition_entry_t part;
            part.name = parse_partition_name(target, name_d);
            part.state = partition_state_t::ACTIVE;
            for (size_t j = 0; j < buckets_d.arr_size(); ++j) {
                datum_t b = buckets_d.get(j);
                rcheck_target(target, b.get_type() == datum_t::R_NUM,
                              base_exc_t::LOGIC,
                              std::string("Hash buckets must be numbers.")
                                  + partition_error::error_suffix(
                                      partition_error::PARTITION_HASH_INVALID));
                double bv = b.as_num();
                rcheck_target(target, bv == std::floor(bv) && bv >= 0,
                              base_exc_t::LOGIC,
                              std::string("Hash buckets must be non-negative "
                                          "integers.")
                                  + partition_error::error_suffix(
                                      partition_error::PARTITION_HASH_INVALID));
                part.hash_buckets.push_back(static_cast<uint32_t>(bv));
            }
            config.partitions.push_back(std::move(part));
        }
    } else if (type_str == "list") {
        config.type = partition_type_t::LIST;
        datum_t parts_d = d.get_field("partitions", NOTHROW);
        rcheck_target(target,
                      parts_d.has() && parts_d.get_type() == datum_t::R_ARRAY,
                      base_exc_t::LOGIC,
                      std::string("List partition config requires a "
                                  "`partitions` array.")
                          + partition_error::error_suffix(
                              partition_error::PARTITION_LIST_INVALID));
        for (size_t i = 0; i < parts_d.arr_size(); ++i) {
            datum_t entry = parts_d.get(i);
            rcheck_target(target, entry.get_type() == datum_t::R_OBJECT,
                          base_exc_t::LOGIC,
                          std::string("Each list partition entry must be an "
                                      "object.")
                              + partition_error::error_suffix(
                                  partition_error::PARTITION_LIST_INVALID));
            datum_t name_d = entry.get_field("name", NOTHROW);
            rcheck_target(target, name_d.has(), base_exc_t::LOGIC,
                          std::string("List partition entries require a "
                                      "`name`.")
                              + partition_error::error_suffix(
                                  partition_error::PARTITION_LIST_INVALID));

            partition_entry_t part;
            part.name = parse_partition_name(target, name_d);
            part.state = partition_state_t::ACTIVE;

            datum_t default_d = entry.get_field("default", NOTHROW);
            if (default_d.has()) {
                rcheck_target(target, default_d.get_type() == datum_t::R_BOOL,
                              base_exc_t::LOGIC,
                              std::string("`default` must be a boolean.")
                                  + partition_error::error_suffix(
                                      partition_error::PARTITION_LIST_INVALID));
                part.list_default = default_d.as_bool();
            }

            datum_t values_d = entry.get_field("values", NOTHROW);
            if (values_d.has()) {
                rcheck_target(target, values_d.get_type() == datum_t::R_ARRAY,
                              base_exc_t::LOGIC,
                              std::string("`values` must be an array.")
                                  + partition_error::error_suffix(
                                      partition_error::PARTITION_LIST_INVALID));
                for (size_t j = 0; j < values_d.arr_size(); ++j) {
                    part.list_values.push_back(values_d.get(j));
                }
            }
            config.partitions.push_back(std::move(part));
        }
    } else {
        rfail_target(target, base_exc_t::LOGIC,
                     "Unknown partition type `%s`; expected `range`, `hash`, "
                     "or `list`.%s",
                     type_str.c_str(),
                     partition_error::error_suffix(
                         partition_error::PARTITION_CONFIG_INVALID).c_str());
    }

    finalize_partition_entries(&config);
    config.validate_or_throw();
    return config;
}

/* ── ReQL terms ──────────────────────────────────────────────────────────── */

/* Meta-op helper: non-deterministic admin terms. */
class meta_op_term_base_t : public op_term_t {
public:
    meta_op_term_base_t(compile_env_t *env, const raw_term_t &term,
                        argspec_t argspec,
                        optargspec_t optargspec = optargspec_t({}))
        : op_term_t(env, term, std::move(argspec), std::move(optargspec)) { }
private:
    virtual deterministic_t is_deterministic() const {
        return deterministic_t::no();
    }
};

class partition_info_term_t : public meta_op_term_base_t {
public:
    partition_info_term_t(compile_env_t *env, const raw_term_t &term)
        : meta_op_term_base_t(env, term, argspec_t(1, 1)) { }

private:
    virtual scoped_ptr_t<val_t> eval_impl(
            scope_env_t *env, args_t *args, eval_flags_t) const {
        counted_t<table_t> table = args->arg(env, 0)->as_table();
        name_string_t table_name =
            name_string_t::guarantee_valid(table->name.c_str());

        ql::datum_t result;
        try {
            admin_err_t error;
            if (!env->env->reql_cluster_interface()->table_partition_info(
                    table->db,
                    table_name,
                    env->env->interruptor,
                    &result,
                    &error)) {
                REQL_RETHROW(error);
            }
        } catch (auth::permission_error_t const &permission_error) {
            rfail(ql::base_exc_t::PERMISSION_ERROR, "%s", permission_error.what());
        }
        return new_val(result);
    }
    virtual const char *name() const { return "partition_info"; }
};

class repartition_term_t : public meta_op_term_base_t {
public:
    repartition_term_t(compile_env_t *env, const raw_term_t &term)
        : meta_op_term_base_t(env, term, argspec_t(2, 2),
                              optargspec_t({"dry_run", "wait"})) { }

private:
    virtual scoped_ptr_t<val_t> eval_impl(
            scope_env_t *env, args_t *args, eval_flags_t) const {
        counted_t<table_t> table = args->arg(env, 0)->as_table();
        name_string_t table_name =
            name_string_t::guarantee_valid(table->name.c_str());

        scoped_ptr_t<val_t> config_val = args->arg(env, 1);
        partition_config_t partition_config =
            parse_partition_config_from_datum(config_val->as_datum(),
                                              config_val.get());

        bool dry_run = false;
        if (scoped_ptr_t<val_t> v = args->optarg(env, "dry_run")) {
            dry_run = v->as_bool();
        }
        bool wait = true;
        if (scoped_ptr_t<val_t> v = args->optarg(env, "wait")) {
            wait = v->as_bool();
        }

        ql::datum_t result;
        try {
            admin_err_t error;
            if (!env->env->reql_cluster_interface()->table_repartition(
                    env->env->get_user_context(),
                    table->db,
                    table_name,
                    partition_config,
                    dry_run,
                    wait,
                    env->env->interruptor,
                    &result,
                    &error)) {
                REQL_RETHROW(error);
            }
        } catch (auth::permission_error_t const &permission_error) {
            rfail(ql::base_exc_t::PERMISSION_ERROR, "%s", permission_error.what());
        }
        return new_val(result);
    }
    virtual const char *name() const { return "repartition"; }
};

counted_t<term_t> make_partition_info_term(
        compile_env_t *env, const raw_term_t &term) {
    return make_counted<partition_info_term_t>(env, term);
}

counted_t<term_t> make_repartition_term(
        compile_env_t *env, const raw_term_t &term) {
    return make_counted<repartition_term_t>(env, term);
}

}  // namespace ql
