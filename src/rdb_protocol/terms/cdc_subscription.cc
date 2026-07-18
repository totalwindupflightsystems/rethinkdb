// Copyright 2026 RethinkDB, all rights reserved.
#include "rdb_protocol/terms/cdc_subscription.hpp"

#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "containers/name_string.hpp"
#include "containers/uuid.hpp"
#include "rdb_protocol/env.hpp"
#include "rdb_protocol/error.hpp"
#include "rdb_protocol/op.hpp"
#include "rdb_protocol/publication.hpp"
#include "rdb_protocol/terms/cdc_publication.hpp"
#include "rdb_protocol/terms/terms.hpp"
#include "rdb_protocol/val.hpp"

namespace ql {

namespace {

name_string_t parse_sub_name(rcheckable_t *target, const datum_t &name_d,
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

datum_t stub_created_response(const std::string &name) {
    ql::datum_object_builder_t res;
    res.overwrite("created", datum_t::boolean(true));
    res.overwrite("subscription", datum_t(datum_string_t(name)));
    res.overwrite("state", datum_t(datum_string_t("creating")));
    res.overwrite("message",
                  datum_t(datum_string_t("CDC term not yet wired to backend")));
    return std::move(res).to_datum();
}

datum_t stub_dropped_response(const std::string &name) {
    ql::datum_object_builder_t res;
    res.overwrite("dropped", datum_t::boolean(true));
    res.overwrite("subscription", datum_t(datum_string_t(name)));
    res.overwrite("message",
                  datum_t(datum_string_t("CDC term not yet wired to backend")));
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

parsed_subscription_create_t parse_subscription_config_from_datum(
        const datum_t &d, rcheckable_t *target) {
    rcheck_target(target, d.get_type() == datum_t::R_OBJECT, base_exc_t::LOGIC,
                  strprintf("Expected type OBJECT but found %s for subscription "
                            "config:\n%s",
                            d.get_type_name().c_str(),
                            d.print().c_str()));

    parsed_subscription_create_t out;

    datum_t name_d = d.get_field("name", NOTHROW);
    rcheck_target(target, name_d.has(), base_exc_t::LOGIC,
                  "Subscription config requires a string `name` field.");
    out.name = parse_sub_name(target, name_d, "Subscription");

    // Source locator (spec) OR publication name (task shorthand).
    datum_t source_d = d.get_field("source", NOTHROW);
    datum_t pub_d = d.get_field("publication", NOTHROW);
    if (source_d.has()) {
        rcheck_target(target, source_d.get_type() == datum_t::R_STR, base_exc_t::LOGIC,
                      "Subscription `source` must be a string locator.");
        out.source = source_d.as_str().to_std();
        rcheck_target(target, !out.source.empty(), base_exc_t::LOGIC,
                      "Subscription `source` must not be empty.");
    }
    if (pub_d.has()) {
        out.publication_name = parse_sub_name(target, pub_d, "Publication");
    }
    rcheck_target(target, source_d.has() || pub_d.has(), base_exc_t::LOGIC,
                  "Subscription config requires `source` (locator) or "
                  "`publication` (name).");

    // Target table: nested object {db, table} or targetTable string.
    datum_t target_d = d.get_field("target", NOTHROW);
    datum_t target_table_d = d.get_field("targetTable", NOTHROW);
    if (!target_table_d.has()) {
        target_table_d = d.get_field("target_table", NOTHROW);
    }

    if (target_d.has()) {
        rcheck_target(target, target_d.get_type() == datum_t::R_OBJECT,
                      base_exc_t::LOGIC,
                      "Subscription `target` must be an object `{db, table}`.");
        datum_t tdb = target_d.get_field("db", NOTHROW);
        datum_t ttbl = target_d.get_field("table", NOTHROW);
        rcheck_target(target, tdb.has() && tdb.get_type() == datum_t::R_STR,
                      base_exc_t::LOGIC,
                      "Subscription `target.db` must be a string.");
        rcheck_target(target, ttbl.has() && ttbl.get_type() == datum_t::R_STR,
                      base_exc_t::LOGIC,
                      "Subscription `target.table` must be a string.");
        out.target_db = tdb.as_str().to_std();
        out.target_table = ttbl.as_str().to_std();
        name_string_t tmp;
        rcheck_target(target, tmp.assign_value(out.target_db), base_exc_t::LOGIC,
                      strprintf("Target database name `%s` invalid (%s).",
                                out.target_db.c_str(),
                                name_string_t::valid_char_msg));
        rcheck_target(target, tmp.assign_value(out.target_table), base_exc_t::LOGIC,
                      strprintf("Target table name `%s` invalid (%s).",
                                out.target_table.c_str(),
                                name_string_t::valid_char_msg));
    } else if (target_table_d.has()) {
        rcheck_target(target, target_table_d.get_type() == datum_t::R_STR,
                      base_exc_t::LOGIC,
                      "Subscription `targetTable` must be a string.");
        out.target_table = target_table_d.as_str().to_std();
        name_string_t tmp;
        rcheck_target(target, tmp.assign_value(out.target_table), base_exc_t::LOGIC,
                      strprintf("Target table name `%s` invalid (%s).",
                                out.target_table.c_str(),
                                name_string_t::valid_char_msg));
    } else {
        // Default target table name = subscription name (spec).
        out.target_table = out.name.str();
    }

    datum_t conflict_d = d.get_field("conflict", NOTHROW);
    if (conflict_d.has()) {
        rcheck_target(target, conflict_d.get_type() == datum_t::R_STR,
                      base_exc_t::LOGIC,
                      "Subscription `conflict` must be a string.");
        std::string c = conflict_d.as_str().to_std();
        if (c == "last_write_wins") {
            out.conflict_policy = conflict_resolution_t::LAST_WRITE_WINS;
        } else if (c == "primary_key_merge") {
            out.conflict_policy = conflict_resolution_t::PRIMARY_KEY_MERGE;
        } else if (c == "custom" || c == "custom_handler") {
            out.conflict_policy = conflict_resolution_t::CUSTOM_HANDLER;
        } else {
            rfail_target(target, base_exc_t::LOGIC,
                         "Unknown conflict resolution `%s`; expected "
                         "`last_write_wins`, `primary_key_merge`, or `custom`.",
                         c.c_str());
        }
    }

    datum_t handler_d = d.get_field("conflictHandler", NOTHROW);
    if (!handler_d.has()) {
        handler_d = d.get_field("conflict_handler", NOTHROW);
    }
    if (out.conflict_policy == conflict_resolution_t::CUSTOM_HANDLER) {
        rcheck_target(target, handler_d.has(), base_exc_t::LOGIC,
                      "`conflictHandler` is required when conflict is `custom`.");
        // Full restricted-ReQL validation is CDC-06; ensure it is present and
        // is a function or string identifier for now.
        bool ok_type =
            handler_d.get_type() == datum_t::R_STR
            || handler_d.get_type() == datum_t::R_OBJECT;
        rcheck_target(target, ok_type, base_exc_t::LOGIC,
                      "`conflictHandler` must be a function or string reference.");
    } else if (handler_d.has()) {
        rfail_target(target, base_exc_t::LOGIC,
                     "`conflictHandler` is only valid when conflict is `custom`.");
    }

    datum_t snap_d = d.get_field("snapshot", NOTHROW);
    if (snap_d.has()) {
        rcheck_target(target, snap_d.get_type() == datum_t::R_STR, base_exc_t::LOGIC,
                      "Subscription `snapshot` must be a string.");
        std::string snap = snap_d.as_str().to_std();
        if (snap == "initial" || snap == "full") {
            out.snapshot_mode = snapshot_mode_t::FULL;
        } else if (snap == "none") {
            out.snapshot_mode = snapshot_mode_t::NONE;
        } else {
            rfail_target(target, base_exc_t::LOGIC,
                         "Unknown subscription snapshot mode `%s`; expected "
                         "`initial` or `none`.",
                         snap.c_str());
        }
    }

    datum_t batch_d = d.get_field("applyBatchSize", NOTHROW);
    if (!batch_d.has()) {
        batch_d = d.get_field("apply_batch_size", NOTHROW);
    }
    if (batch_d.has()) {
        rcheck_target(target, batch_d.get_type() == datum_t::R_NUM, base_exc_t::LOGIC,
                      "`applyBatchSize` must be a number.");
        double b = batch_d.as_num();
        rcheck_target(target, b == std::floor(b) && b >= 1 && b <= 100000,
                      base_exc_t::LOGIC,
                      "`applyBatchSize` must be an integer in [1, 100000].");
        out.apply_batch_size = static_cast<uint32_t>(b);
    }

    datum_t auth_d = d.get_field("auth", NOTHROW);
    if (auth_d.has()) {
        rcheck_target(target, auth_d.get_type() == datum_t::R_OBJECT, base_exc_t::LOGIC,
                      "Subscription `auth` must be an object with a credential "
                      "reference (never a literal secret).");
        // Accept tokenRef / passwordRef / credentialRef — reject obvious secrets.
        datum_t bad = auth_d.get_field("password", NOTHROW);
        if (!bad.has()) {
            bad = auth_d.get_field("token", NOTHROW);
        }
        if (!bad.has()) {
            bad = auth_d.get_field("secret", NOTHROW);
        }
        rcheck_target(target, !bad.has(), base_exc_t::LOGIC,
                      "Subscription `auth` must use a credential reference "
                      "(`tokenRef` / `passwordRef`); literal secrets are rejected.");
        datum_t ref = auth_d.get_field("tokenRef", NOTHROW);
        if (!ref.has()) {
            ref = auth_d.get_field("passwordRef", NOTHROW);
        }
        if (!ref.has()) {
            ref = auth_d.get_field("credentialRef", NOTHROW);
        }
        if (!ref.has()) {
            ref = auth_d.get_field("credentialsRef", NOTHROW);
        }
        rcheck_target(target,
                      ref.has() && ref.get_type() == datum_t::R_STR,
                      base_exc_t::LOGIC,
                      "Subscription `auth` requires a string credential reference "
                      "field (`tokenRef`, `passwordRef`, or `credentialRef`).");
        out.has_auth = true;
    }

    datum_t tls_d = d.get_field("tls", NOTHROW);
    if (tls_d.has()) {
        rcheck_target(target, tls_d.get_type() == datum_t::R_OBJECT, base_exc_t::LOGIC,
                      "Subscription `tls` must be an object.");
        out.has_tls = true;
    }

    return out;
}

/* ── ReQL terms ──────────────────────────────────────────────────────────── */

class subscription_create_term_t : public meta_op_term_base_t {
public:
    subscription_create_term_t(compile_env_t *env, const raw_term_t &term)
        : meta_op_term_base_t(env, term, argspec_t(1, 2)) { }

private:
    virtual scoped_ptr_t<val_t> eval_impl(
            scope_env_t *env, args_t *args, eval_flags_t) const {
        require_cdc_cluster_support(this);
        scoped_ptr_t<val_t> config_val;
        if (args->num_args() == 2) {
            // Scope: table or database.
            scoped_ptr_t<val_t> scope = args->arg(env, 0);
            bool ok = scope->get_type().is_convertible(val_t::type_t::TABLE)
                || scope->get_type().is_convertible(val_t::type_t::DB);
            rcheck(ok, base_exc_t::LOGIC,
                   "createSubscription scope must be a table or database.");
            if (scope->get_type().is_convertible(val_t::type_t::TABLE)) {
                (void)scope->as_table();
            } else {
                (void)scope->as_db();
            }
            config_val = args->arg(env, 1);
        } else {
            config_val = args->arg(env, 0);
        }
        parsed_subscription_create_t cfg =
            parse_subscription_config_from_datum(config_val->as_datum(),
                                                 config_val.get());
        return new_val(stub_created_response(cfg.name.str()));
    }
    virtual const char *name() const { return "subscription_create"; }
};

class subscription_list_term_t : public meta_op_term_base_t {
public:
    subscription_list_term_t(compile_env_t *env, const raw_term_t &term)
        : meta_op_term_base_t(env, term, argspec_t(0, 1)) { }

private:
    virtual scoped_ptr_t<val_t> eval_impl(
            scope_env_t *env, args_t *args, eval_flags_t) const {
        require_cdc_cluster_support(this);
        if (args->num_args() == 1) {
            scoped_ptr_t<val_t> scope = args->arg(env, 0);
            bool ok = scope->get_type().is_convertible(val_t::type_t::TABLE)
                || scope->get_type().is_convertible(val_t::type_t::DB);
            rcheck(ok, base_exc_t::LOGIC,
                   "subscriptionList argument must be a table or database.");
            if (scope->get_type().is_convertible(val_t::type_t::TABLE)) {
                (void)scope->as_table();
            } else {
                (void)scope->as_db();
            }
        }
        return new_val(datum_t(std::vector<datum_t>(),
                               env->env->limits()));
    }
    virtual const char *name() const { return "subscription_list"; }
};

class subscription_status_term_t : public meta_op_term_base_t {
public:
    subscription_status_term_t(compile_env_t *env, const raw_term_t &term)
        : meta_op_term_base_t(env, term, argspec_t(1, 2)) { }

private:
    virtual scoped_ptr_t<val_t> eval_impl(
            scope_env_t *env, args_t *args, eval_flags_t) const {
        require_cdc_cluster_support(this);
        std::string sub_name;
        if (args->num_args() == 2) {
            counted_t<table_t> table = args->arg(env, 0)->as_table();
            (void)table;
            sub_name = args->arg(env, 1)->as_str().to_std();
        } else {
            sub_name = args->arg(env, 0)->as_str().to_std();
        }
        name_string_t checked;
        bool ok = checked.assign_value(sub_name);
        rcheck(ok, base_exc_t::LOGIC,
               strprintf("Subscription name `%s` invalid (%s).",
                         sub_name.c_str(),
                         name_string_t::valid_char_msg));

        ql::datum_object_builder_t res;
        res.overwrite("name", datum_t(datum_string_t(checked.str())));
        res.overwrite("state", datum_t(datum_string_t("unknown")));
        res.overwrite("message",
                      datum_t(datum_string_t(
                          "CDC term not yet wired to backend")));
        return new_val(std::move(res).to_datum());
    }
    virtual const char *name() const { return "subscription_status"; }
};

class subscription_drop_term_t : public meta_op_term_base_t {
public:
    subscription_drop_term_t(compile_env_t *env, const raw_term_t &term)
        : meta_op_term_base_t(env, term, argspec_t(1, 2)) { }

private:
    virtual scoped_ptr_t<val_t> eval_impl(
            scope_env_t *env, args_t *args, eval_flags_t) const {
        require_cdc_cluster_support(this);
        std::string sub_name;
        if (args->num_args() == 2) {
            counted_t<table_t> table = args->arg(env, 0)->as_table();
            (void)table;
            sub_name = args->arg(env, 1)->as_str().to_std();
        } else {
            sub_name = args->arg(env, 0)->as_str().to_std();
        }
        name_string_t checked;
        bool ok = checked.assign_value(sub_name);
        rcheck(ok, base_exc_t::LOGIC,
               strprintf("Subscription name `%s` invalid (%s).",
                         sub_name.c_str(),
                         name_string_t::valid_char_msg));
        return new_val(stub_dropped_response(checked.str()));
    }
    virtual const char *name() const { return "subscription_drop"; }
};

counted_t<term_t> make_subscription_create_term(
        compile_env_t *env, const raw_term_t &term) {
    return make_counted<subscription_create_term_t>(env, term);
}
counted_t<term_t> make_subscription_list_term(
        compile_env_t *env, const raw_term_t &term) {
    return make_counted<subscription_list_term_t>(env, term);
}
counted_t<term_t> make_subscription_status_term(
        compile_env_t *env, const raw_term_t &term) {
    return make_counted<subscription_status_term_t>(env, term);
}
counted_t<term_t> make_subscription_drop_term(
        compile_env_t *env, const raw_term_t &term) {
    return make_counted<subscription_drop_term_t>(env, term);
}

}  // namespace ql
