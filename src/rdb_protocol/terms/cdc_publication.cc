// Copyright 2026 RethinkDB, all rights reserved.
#include "rdb_protocol/terms/cdc_publication.hpp"

#include <cmath>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "clustering/administration/admin_op_exc.hpp"
#include "containers/name_string.hpp"
#include "containers/uuid.hpp"
#include "rdb_protocol/cdc_types.hpp"
#include "rdb_protocol/env.hpp"
#include "rdb_protocol/error.hpp"
#include "rdb_protocol/op.hpp"
#include "rdb_protocol/publication.hpp"
#include "rdb_protocol/terms/terms.hpp"
#include "rdb_protocol/val.hpp"
#include "time.hpp"
#include "version.hpp"

namespace ql {

namespace {

name_string_t parse_cdc_name(rcheckable_t *target, const datum_t &name_d,
                             const char *what) {
    rcheck_target(target,
                  name_d.get_type() == datum_t::R_STR,
                  base_exc_t::LOGIC,
                  strprintf("Expected type STRING but found %s for %s:\n%s",
                            name_d.get_type_name().c_str(),
                            what,
                            name_d.print().c_str()));
    name_string_t name;
    bool ok = name.assign_value(name_d.as_str());
    rcheck_target(target, ok, base_exc_t::LOGIC,
                  strprintf("%s name `%s` invalid (%s).",
                            what,
                            name_d.as_str().to_std().c_str(),
                            name_string_t::valid_char_msg));
    return name;
}

/* Operation bitmap matching change_operation_t: INSERT=1, UPDATE=2, DELETE=4, REPLACE=8 */
uint32_t parse_operations_bitmap(rcheckable_t *target, const datum_t &ops_d) {
    rcheck_target(target, ops_d.get_type() == datum_t::R_ARRAY, base_exc_t::LOGIC,
                  strprintf("Publication filter `operations` must be an array, got %s.",
                            ops_d.get_type_name().c_str()));
    rcheck_target(target, ops_d.arr_size() > 0, base_exc_t::LOGIC,
                  "Publication filter `operations` must not be empty.");
    uint32_t bitmap = 0;
    for (size_t i = 0; i < ops_d.arr_size(); ++i) {
        datum_t op = ops_d.get(i);
        rcheck_target(target, op.get_type() == datum_t::R_STR, base_exc_t::LOGIC,
                      "Publication filter `operations` entries must be strings.");
        std::string s = op.as_str().to_std();
        if (s == "insert") {
            bitmap |= (1u << static_cast<int>(change_operation_t::INSERT));
        } else if (s == "update") {
            bitmap |= (1u << static_cast<int>(change_operation_t::UPDATE));
        } else if (s == "delete") {
            bitmap |= (1u << static_cast<int>(change_operation_t::DELETE));
        } else if (s == "replace") {
            bitmap |= (1u << static_cast<int>(change_operation_t::REPLACE));
        } else {
            rfail_target(target, base_exc_t::LOGIC,
                         "Unknown publication operation `%s`; expected "
                         "`insert`, `update`, `delete`, or `replace`.",
                         s.c_str());
        }
    }
    return bitmap;
}

void validate_predicate_value(rcheckable_t *target, const datum_t &v,
                              const std::string &field) {
    if (v.get_type() == datum_t::R_STR
        || v.get_type() == datum_t::R_NUM
        || v.get_type() == datum_t::R_BOOL
        || v.get_type() == datum_t::R_NULL) {
        return;
    }
    if (v.get_type() == datum_t::R_OBJECT) {
        // Finite `in` set only: {in: [scalar, ...]}
        rcheck_target(target, v.obj_size() == 1, base_exc_t::LOGIC,
                      strprintf("Publication filter predicate for `%s` must be a "
                                "scalar equality or `{in: [...]}`.",
                                field.c_str()));
        std::pair<datum_string_t, datum_t> pair = v.get_pair(0);
        rcheck_target(target, pair.first.to_std() == "in", base_exc_t::LOGIC,
                      strprintf("Publication filter predicate for `%s` only "
                                "supports the `in` operator, got `%s`.",
                                field.c_str(),
                                pair.first.to_std().c_str()));
        rcheck_target(target, pair.second.get_type() == datum_t::R_ARRAY,
                      base_exc_t::LOGIC,
                      strprintf("Publication filter predicate `%s.in` must be "
                                "an array.",
                                field.c_str()));
        rcheck_target(target, pair.second.arr_size() > 0, base_exc_t::LOGIC,
                      strprintf("Publication filter predicate `%s.in` must not "
                                "be empty.",
                                field.c_str()));
        for (size_t i = 0; i < pair.second.arr_size(); ++i) {
            datum_t elem = pair.second.get(i);
            rcheck_target(target,
                          elem.get_type() == datum_t::R_STR
                              || elem.get_type() == datum_t::R_NUM
                              || elem.get_type() == datum_t::R_BOOL
                              || elem.get_type() == datum_t::R_NULL,
                          base_exc_t::LOGIC,
                          strprintf("Publication filter predicate `%s.in` "
                                    "entries must be scalars.",
                                    field.c_str()));
        }
        return;
    }
    rfail_target(target, base_exc_t::LOGIC,
                 "Publication filter predicate for `%s` must be a scalar or "
                 "`{in: [...]}` (got %s). Nested paths, regexes, and functions "
                 "are not allowed.",
                 field.c_str(),
                 v.get_type_name().c_str());
}

void validate_filter(rcheckable_t *target, const datum_t &filter_d,
                     publication_filter_t *out) {
    rcheck_target(target, filter_d.get_type() == datum_t::R_OBJECT, base_exc_t::LOGIC,
                  strprintf("Publication `filter` must be an object, got %s.",
                            filter_d.get_type_name().c_str()));

    datum_t fields_d = filter_d.get_field("fields", NOTHROW);
    if (fields_d.has()) {
        rcheck_target(target, fields_d.get_type() == datum_t::R_ARRAY,
                      base_exc_t::LOGIC,
                      "Publication filter `fields` must be an array of strings.");
        for (size_t i = 0; i < fields_d.arr_size(); ++i) {
            datum_t f = fields_d.get(i);
            rcheck_target(target, f.get_type() == datum_t::R_STR, base_exc_t::LOGIC,
                          "Publication filter `fields` entries must be strings.");
            std::string field = f.as_str().to_std();
            rcheck_target(target, !field.empty(), base_exc_t::LOGIC,
                          "Publication filter `fields` entries must be non-empty.");
            // Nested paths rejected: no dots.
            rcheck_target(target, field.find('.') == std::string::npos,
                          base_exc_t::LOGIC,
                          strprintf("Publication filter field `%s` must be a "
                                    "top-level field (nested paths are rejected).",
                                    field.c_str()));
            out->projected_fields.insert(std::move(field));
        }
    }

    datum_t ops_d = filter_d.get_field("operations", NOTHROW);
    if (ops_d.has()) {
        out->allowed_operations = parse_operations_bitmap(target, ops_d);
    } else {
        out->allowed_operations = 0x0F;  // all
    }

    datum_t pred_d = filter_d.get_field("predicate", NOTHROW);
    if (pred_d.has()) {
        rcheck_target(target, pred_d.get_type() == datum_t::R_OBJECT,
                      base_exc_t::LOGIC,
                      "Publication filter `predicate` must be an object.");
        for (size_t i = 0; i < pred_d.obj_size(); ++i) {
            std::pair<datum_string_t, datum_t> pair = pred_d.get_pair(i);
            std::string key = pair.first.to_std();
            rcheck_target(target, key.find('.') == std::string::npos,
                          base_exc_t::LOGIC,
                          strprintf("Publication filter predicate key `%s` must "
                                    "be a top-level field.",
                                    key.c_str()));
            validate_predicate_value(target, pair.second, key);
        }
        // Predicate map is validated but not stored on publication_filter_t yet
        // (CDC-01 stores projected_fields + allowed_operations only). Full
        // predicate storage lands with CDC-05 filter compilation.
    }
}

const char *publication_state_to_cstr(publication_state_t state) {
    switch (state) {
        case publication_state_t::CREATING:  return "creating";
        case publication_state_t::READY:     return "ready";
        case publication_state_t::DROPPING:  return "dropping";
        case publication_state_t::DROPPED:   return "dropped";
        case publication_state_t::ERROR:     return "error";
        default:                             return "unknown";
    }
}

const char *publication_format_to_cstr(publication_format_t fmt) {
    switch (fmt) {
        case publication_format_t::JSON_V1:          return "json";
        case publication_format_t::INTERNAL_RDB_V1:  return "internal_rdb_v1";
        default:                                     return "unknown";
    }
}

const char *snapshot_mode_to_cstr(snapshot_mode_t mode) {
    switch (mode) {
        case snapshot_mode_t::FULL:  return "initial";
        case snapshot_mode_t::NONE:  return "none";
        default:                     return "unknown";
    }
}

datum_t publication_config_to_datum(const publication_config_t &config) {
    ql::datum_object_builder_t res;
    res.overwrite("id", datum_t(datum_string_t(uuid_to_str(config.publication_id))));
    res.overwrite("name", datum_t(datum_string_t(config.name.str())));
    res.overwrite("state",
        datum_t(datum_string_t(publication_state_to_cstr(config.state))));
    res.overwrite("format",
        datum_t(datum_string_t(publication_format_to_cstr(config.format))));
    res.overwrite("include_before",
        datum_t::boolean(config.include_before_image));
    res.overwrite("include_after",
        datum_t::boolean(config.include_after_image));
    res.overwrite("snapshot",
        datum_t(datum_string_t(snapshot_mode_to_cstr(config.default_snapshot_mode))));
    res.overwrite("max_slot_lag_bytes",
        datum_t(static_cast<double>(config.max_slot_lag_bytes)));
    res.overwrite("database_id",
        datum_t(datum_string_t(uuid_to_str(config.database_id))));
    res.overwrite("table_id",
        datum_t(datum_string_t(uuid_to_str(config.table_id))));
    return std::move(res).to_datum();
}

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

}  // namespace

void require_cdc_cluster_support(const rcheckable_t *target) {
    /* CDC metadata serialization is additive and will be bound to a future
     * cluster_version_t once Raft metadata lands (CDC-05+). Connected peers
     * already negotiate a common CLUSTER version (see rpc/connectivity), so a
     * homogeneous cluster of current builds is the baseline.
     *
     * When a dedicated CDC cluster version is introduced, this gate must
     * reject if any peer's negotiated version is older than that threshold.
     * Until then, allow the ReQL surface so validation and stub responses
     * can be exercised; backend wiring still returns a stub message. */
    static_assert(static_cast<int>(cluster_version_t::CLUSTER)
                      == static_cast<int>(cluster_version_t::LATEST_OVERALL),
                  "CDC gate assumes a single live cluster serialization version");
    (void)target;
}

publication_config_t parse_publication_config_from_datum(
        const datum_t &d, rcheckable_t *target) {
    rcheck_target(target, d.get_type() == datum_t::R_OBJECT, base_exc_t::LOGIC,
                  strprintf("Expected type OBJECT but found %s for publication "
                            "config:\n%s",
                            d.get_type_name().c_str(),
                            d.print().c_str()));

    publication_config_t config;
    config.publication_id = generate_uuid();
    config.state = publication_state_t::CREATING;
    config.created_at = current_microtime();
    config.include_before_image = true;
    config.include_after_image = true;
    config.format = publication_format_t::JSON_V1;
    config.default_snapshot_mode = snapshot_mode_t::FULL;
    config.max_slot_lag_bytes = 1024ULL * 1024 * 1024;
    config.filter.allowed_operations = 0x0F;

    datum_t name_d = d.get_field("name", NOTHROW);
    rcheck_target(target, name_d.has(), base_exc_t::LOGIC,
                  "Publication config requires a string `name` field.");
    config.name = parse_cdc_name(target, name_d, "Publication");

    datum_t filter_d = d.get_field("filter", NOTHROW);
    if (filter_d.has()) {
        validate_filter(target, filter_d, &config.filter);
    }

    datum_t format_d = d.get_field("format", NOTHROW);
    if (format_d.has()) {
        rcheck_target(target, format_d.get_type() == datum_t::R_STR, base_exc_t::LOGIC,
                      "Publication `format` must be a string.");
        std::string fmt = format_d.as_str().to_std();
        if (fmt == "json" || fmt == "json_v1") {
            config.format = publication_format_t::JSON_V1;
        } else if (fmt == "internal_rdb_v1" || fmt == "internal") {
            config.format = publication_format_t::INTERNAL_RDB_V1;
        } else {
            rfail_target(target, base_exc_t::LOGIC,
                         "Unknown publication format `%s`; expected `json` or "
                         "`internal_rdb_v1`.",
                         fmt.c_str());
        }
    }

    datum_t before_d = d.get_field("includeBefore", NOTHROW);
    if (!before_d.has()) {
        before_d = d.get_field("include_before", NOTHROW);
    }
    if (before_d.has()) {
        rcheck_target(target, before_d.get_type() == datum_t::R_BOOL, base_exc_t::LOGIC,
                      "`includeBefore` must be a boolean.");
        config.include_before_image = before_d.as_bool();
    }

    datum_t after_d = d.get_field("includeAfter", NOTHROW);
    if (!after_d.has()) {
        after_d = d.get_field("include_after", NOTHROW);
    }
    if (after_d.has()) {
        rcheck_target(target, after_d.get_type() == datum_t::R_BOOL, base_exc_t::LOGIC,
                      "`includeAfter` must be a boolean.");
        config.include_after_image = after_d.as_bool();
    }

    rcheck_target(target,
                  config.include_before_image || config.include_after_image,
                  base_exc_t::LOGIC,
                  "At least one of `includeBefore` and `includeAfter` must be true.");

    datum_t snap_d = d.get_field("snapshot", NOTHROW);
    if (snap_d.has()) {
        rcheck_target(target, snap_d.get_type() == datum_t::R_STR, base_exc_t::LOGIC,
                      "Publication `snapshot` must be a string.");
        std::string snap = snap_d.as_str().to_std();
        if (snap == "initial" || snap == "full") {
            config.default_snapshot_mode = snapshot_mode_t::FULL;
        } else if (snap == "none") {
            config.default_snapshot_mode = snapshot_mode_t::NONE;
        } else {
            rfail_target(target, base_exc_t::LOGIC,
                         "Unknown publication snapshot mode `%s`; expected "
                         "`initial` or `none`.",
                         snap.c_str());
        }
    }

    datum_t lag_d = d.get_field("maxSlotLagBytes", NOTHROW);
    if (!lag_d.has()) {
        lag_d = d.get_field("max_slot_lag_bytes", NOTHROW);
    }
    if (lag_d.has()) {
        rcheck_target(target, lag_d.get_type() == datum_t::R_NUM, base_exc_t::LOGIC,
                      "`maxSlotLagBytes` must be a number.");
        double lag = lag_d.as_num();
        rcheck_target(target, lag == std::floor(lag) && lag > 0, base_exc_t::LOGIC,
                      "`maxSlotLagBytes` must be a positive integer.");
        config.max_slot_lag_bytes = static_cast<uint64_t>(lag);
    }

    return config;
}

/* ── ReQL terms ──────────────────────────────────────────────────────────── */

class publication_create_term_t : public meta_op_term_base_t {
public:
    publication_create_term_t(compile_env_t *env, const raw_term_t &term)
        : meta_op_term_base_t(env, term, argspec_t(2, 2),
                              optargspec_t({})) { }

private:
    virtual scoped_ptr_t<val_t> eval_impl(
            scope_env_t *env, args_t *args, eval_flags_t) const {
        require_cdc_cluster_support(this);
        counted_t<table_t> table = args->arg(env, 0)->as_table();
        scoped_ptr_t<val_t> config_val = args->arg(env, 1);
        publication_config_t config =
            parse_publication_config_from_datum(config_val->as_datum(),
                                                config_val.get());
        config.database_id = table->db->id;
        config.table_id = table->get_id();

        try {
            admin_err_t error;
            if (!env->env->reql_cluster_interface()->publication_create(
                    env->env->get_user_context(),
                    table->db,
                    name_string_t::guarantee_valid(table->name.c_str()),
                    config,
                    env->env->interruptor,
                    &error)) {
                REQL_RETHROW(error);
            }
        } catch (auth::permission_error_t const &permission_error) {
            rfail(ql::base_exc_t::PERMISSION_ERROR, "%s",
                  permission_error.what());
        }

        ql::datum_object_builder_t res;
        res.overwrite("created", datum_t(1.0));
        res.overwrite("publication", datum_t(datum_string_t(config.name.str())));
        res.overwrite("state", datum_t(datum_string_t("creating")));
        return new_val(std::move(res).to_datum());
    }
    virtual const char *name() const { return "publication_create"; }
};

class publication_list_term_t : public meta_op_term_base_t {
public:
    publication_list_term_t(compile_env_t *env, const raw_term_t &term)
        : meta_op_term_base_t(env, term, argspec_t(0, 1)) { }

private:
    virtual scoped_ptr_t<val_t> eval_impl(
            scope_env_t *env, args_t *args, eval_flags_t) const {
        require_cdc_cluster_support(this);

        counted_t<table_t> table;
        if (args->num_args() == 1) {
            scoped_ptr_t<val_t> scope = args->arg(env, 0);
            rcheck(scope->get_type().is_convertible(val_t::type_t::TABLE),
                   base_exc_t::LOGIC,
                   "publicationList argument must be a table.");
            table = scope->as_table();
        }

        admin_err_t error;
        std::map<uuid_u, ql::publication_config_t> pubs;
        if (!env->env->reql_cluster_interface()->publication_list(
                table->db, name_string_t::guarantee_valid(table->name.c_str()),
                env->env->interruptor, &error, &pubs)) {
            REQL_RETHROW(error);
        }
        std::vector<datum_t> arr;
        arr.reserve(pubs.size());
        for (const auto &pair : pubs) {
            ql::datum_object_builder_t entry;
            entry.overwrite("id",
                datum_t(datum_string_t(uuid_to_str(pair.first))));
            entry.overwrite("name",
                datum_t(datum_string_t(pair.second.name.str())));
            entry.overwrite("state",
                datum_t(datum_string_t(
                    publication_state_to_string(pair.second.state))));
            arr.push_back(std::move(entry).to_datum());
        }
        return new_val(datum_t(std::move(arr), env->env->limits()));
    }
    virtual const char *name() const { return "publication_list"; }
};

class publication_status_term_t : public meta_op_term_base_t {
public:
    publication_status_term_t(compile_env_t *env, const raw_term_t &term)
        : meta_op_term_base_t(env, term, argspec_t(1, 2)) { }

private:
    virtual scoped_ptr_t<val_t> eval_impl(
            scope_env_t *env, args_t *args, eval_flags_t) const {
        require_cdc_cluster_support(this);

        counted_t<const db_t> db;
        name_string_t table_name;
        std::string pub_name_str;

        if (args->num_args() == 2) {
            counted_t<table_t> table = args->arg(env, 0)->as_table();
            db = table->db;
            table_name = name_string_t::guarantee_valid(table->name.c_str());
            pub_name_str = args->arg(env, 1)->as_str().to_std();
        } else {
            pub_name_str = args->arg(env, 0)->as_str().to_std();
            /* Without explicit table scope, use database from r.db() call. */
            // No database scope provided — caller must use table-scoped form.
            rcheck(false, base_exc_t::LOGIC,
                   "publicationStatus requires either a table scope "
                   "(r.table('t').publicationStatus('name')) or both a table "
                   "and publication name.");
        }

        name_string_t pub_name;
        bool ok = pub_name.assign_value(pub_name_str);
        rcheck(ok, base_exc_t::LOGIC,
               strprintf("Publication name `%s` invalid (%s).",
                         pub_name_str.c_str(),
                         name_string_t::valid_char_msg));

        admin_err_t error;
        ql::publication_config_t config;
        if (!env->env->reql_cluster_interface()->publication_status(
                db, table_name, pub_name,
                env->env->interruptor, &error, &config)) {
            REQL_RETHROW(error);
        }

        return new_val(publication_config_to_datum(config));
    }
    virtual const char *name() const { return "publication_status"; }
};

class publication_drop_term_t : public meta_op_term_base_t {
public:
    publication_drop_term_t(compile_env_t *env, const raw_term_t &term)
        : meta_op_term_base_t(env, term, argspec_t(1, 2)) { }

private:
    virtual scoped_ptr_t<val_t> eval_impl(
            scope_env_t *env, args_t *args, eval_flags_t) const {
        require_cdc_cluster_support(this);

        counted_t<const db_t> db;
        name_string_t table_name;
        std::string pub_name_str;

        if (args->num_args() == 2) {
            counted_t<table_t> table = args->arg(env, 0)->as_table();
            db = table->db;
            table_name = name_string_t::guarantee_valid(table->name.c_str());
            pub_name_str = args->arg(env, 1)->as_str().to_std();
        } else {
            pub_name_str = args->arg(env, 0)->as_str().to_std();
            rcheck(false, base_exc_t::LOGIC,
                   "publicationDrop requires a table scope "
                   "(r.table('t').publicationDrop('name')).");
        }

        name_string_t pub_name;
        bool ok = pub_name.assign_value(pub_name_str);
        rcheck(ok, base_exc_t::LOGIC,
               strprintf("Publication name `%s` invalid (%s).",
                         pub_name_str.c_str(),
                         name_string_t::valid_char_msg));

        /* First resolve the publication to get its UUID. */
        admin_err_t error;
        ql::publication_config_t config;
        if (!env->env->reql_cluster_interface()->publication_status(
                db, table_name, pub_name,
                env->env->interruptor, &error, &config)) {
            REQL_RETHROW(error);
        }

        try {
            if (!env->env->reql_cluster_interface()->publication_drop(
                    env->env->get_user_context(),
                    db, table_name,
                    config.publication_id, pub_name,
                    env->env->interruptor, &error)) {
                REQL_RETHROW(error);
            }
        } catch (auth::permission_error_t const &permission_error) {
            rfail(ql::base_exc_t::PERMISSION_ERROR, "%s",
                  permission_error.what());
        }

        ql::datum_object_builder_t res;
        res.overwrite("dropped", datum_t(1.0));
        res.overwrite("publication",
            datum_t(datum_string_t(pub_name.str())));
        return new_val(std::move(res).to_datum());
    }
    virtual const char *name() const { return "publication_drop"; }
};

counted_t<term_t> make_publication_create_term(
        compile_env_t *env, const raw_term_t &term) {
    return make_counted<publication_create_term_t>(env, term);
}
counted_t<term_t> make_publication_list_term(
        compile_env_t *env, const raw_term_t &term) {
    return make_counted<publication_list_term_t>(env, term);
}
counted_t<term_t> make_publication_status_term(
        compile_env_t *env, const raw_term_t &term) {
    return make_counted<publication_status_term_t>(env, term);
}
counted_t<term_t> make_publication_drop_term(
        compile_env_t *env, const raw_term_t &term) {
    return make_counted<publication_drop_term_t>(env, term);
}

}  // namespace ql
