// Copyright 2026 RethinkDB, all rights reserved.
#include "rdb_protocol/terms/cdc_sink.hpp"

#include <cinttypes>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "containers/name_string.hpp"
#include "containers/uuid.hpp"
#include "rdb_protocol/env.hpp"
#include "rdb_protocol/error.hpp"
#include "rdb_protocol/op.hpp"
#include "rdb_protocol/terms/cdc_publication.hpp"
#include "rdb_protocol/terms/terms.hpp"
#include "rdb_protocol/val.hpp"

namespace ql {

namespace {

name_string_t parse_sink_name(rcheckable_t *target, const datum_t &name_d,
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

void parse_batching(rcheckable_t *target, const datum_t &d,
                    cdc_batching_config_t *out) {
    auto take_uint = [&](const char *camel, const char *snake,
                         uint64_t *dest, uint64_t min_v, uint64_t max_v) {
        datum_t v = d.get_field(camel, NOTHROW);
        if (!v.has()) {
            v = d.get_field(snake, NOTHROW);
        }
        if (!v.has()) {
            return;
        }
        rcheck_target(target, v.get_type() == datum_t::R_NUM, base_exc_t::LOGIC,
                      strprintf("`%s` must be a number.", camel));
        double n = v.as_num();
        rcheck_target(target, n == std::floor(n) && n >= static_cast<double>(min_v)
                                  && n <= static_cast<double>(max_v),
                      base_exc_t::LOGIC,
                      strprintf("`%s` must be an integer in [%" PRIu64 ", %" PRIu64 "].",
                                camel, min_v, max_v));
        *dest = static_cast<uint64_t>(n);
    };

    take_uint("batchSize", "max_records", &out->max_records, 1, 1000000);
    // Also accept maxRecords alias
    {
        datum_t v = d.get_field("maxRecords", NOTHROW);
        if (v.has()) {
            rcheck_target(target, v.get_type() == datum_t::R_NUM, base_exc_t::LOGIC,
                          "`maxRecords` must be a number.");
            double n = v.as_num();
            rcheck_target(target, n == std::floor(n) && n >= 1 && n <= 1000000,
                          base_exc_t::LOGIC,
                          "`maxRecords` must be an integer in [1, 1000000].");
            out->max_records = static_cast<uint64_t>(n);
        }
    }
    take_uint("maxInFlightBatches", "max_in_flight_batches",
              &out->max_in_flight_batches, 1, 1000);
    take_uint("flushIntervalMs", "flush_interval_ms",
              &out->flush_interval_ms, 1, 3600000);
    take_uint("maxBufferBytes", "max_buffer_bytes",
              &out->max_buffer_bytes, 1024, 1024ULL * 1024 * 1024);
}

datum_t stub_created_response(const std::string &name) {
    ql::datum_object_builder_t res;
    res.overwrite("created", datum_t::boolean(true));
    res.overwrite("sink", datum_t(datum_string_t(name)));
    res.overwrite("state", datum_t(datum_string_t("creating")));
    res.overwrite("message",
                  datum_t(datum_string_t("CDC term not yet wired to backend")));
    return std::move(res).to_datum();
}

datum_t stub_dropped_response(const std::string &name) {
    ql::datum_object_builder_t res;
    res.overwrite("dropped", datum_t::boolean(true));
    res.overwrite("sink", datum_t(datum_string_t(name)));
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

parsed_cdc_sink_create_t parse_cdc_sink_config_from_datum(
        const datum_t &d, rcheckable_t *target) {
    rcheck_target(target, d.get_type() == datum_t::R_OBJECT, base_exc_t::LOGIC,
                  strprintf("Expected type OBJECT but found %s for CDC sink "
                            "config:\n%s",
                            d.get_type_name().c_str(),
                            d.print().c_str()));

    parsed_cdc_sink_create_t out;

    datum_t name_d = d.get_field("name", NOTHROW);
    rcheck_target(target, name_d.has(), base_exc_t::LOGIC,
                  "CDC sink config requires a string `name` field.");
    out.name = parse_sink_name(target, name_d, "CDC sink");

    datum_t pub_d = d.get_field("publication", NOTHROW);
    rcheck_target(target, pub_d.has(), base_exc_t::LOGIC,
                  "CDC sink config requires a string `publication` field.");
    out.publication_name = parse_sink_name(target, pub_d, "Publication");

    datum_t type_d = d.get_field("type", NOTHROW);
    rcheck_target(target, type_d.has() && type_d.get_type() == datum_t::R_STR,
                  base_exc_t::LOGIC,
                  "CDC sink config requires a string `type` "
                  "(`kafka`, `webhook`, `file`, or `s3`).");
    std::string type_str = type_d.as_str().to_std();
    if (type_str == "kafka") {
        out.sink_type = cdc_sink_type_t::KAFKA;
    } else if (type_str == "webhook") {
        out.sink_type = cdc_sink_type_t::WEBHOOK;
    } else if (type_str == "file") {
        out.sink_type = cdc_sink_type_t::FILE;
    } else if (type_str == "s3") {
        out.sink_type = cdc_sink_type_t::S3;
    } else {
        rfail_target(target, base_exc_t::LOGIC,
                     "Unknown CDC sink type `%s`; expected `kafka`, `webhook`, "
                     "`file`, or `s3`.",
                     type_str.c_str());
    }

    // Type-specific connection fields.
    switch (out.sink_type) {
    case cdc_sink_type_t::KAFKA: {
        datum_t brokers = d.get_field("brokers", NOTHROW);
        datum_t conn = d.get_field("connection", NOTHROW);
        if (brokers.has()) {
            rcheck_target(target, brokers.get_type() == datum_t::R_ARRAY,
                          base_exc_t::LOGIC,
                          "Kafka sink `brokers` must be an array of strings.");
            rcheck_target(target, brokers.arr_size() > 0, base_exc_t::LOGIC,
                          "Kafka sink `brokers` must not be empty.");
            std::string joined;
            for (size_t i = 0; i < brokers.arr_size(); ++i) {
                datum_t b = brokers.get(i);
                rcheck_target(target, b.get_type() == datum_t::R_STR,
                              base_exc_t::LOGIC,
                              "Kafka sink broker entries must be strings.");
                if (i > 0) {
                    joined.push_back(',');
                }
                joined += b.as_str().to_std();
            }
            out.connection_string = std::move(joined);
        } else if (conn.has()) {
            rcheck_target(target, conn.get_type() == datum_t::R_STR, base_exc_t::LOGIC,
                          "Kafka sink `connection` must be a string.");
            out.connection_string = conn.as_str().to_std();
        } else {
            rfail_target(target, base_exc_t::LOGIC,
                         "Kafka sink requires `brokers` (array) or `connection` "
                         "(string).");
        }
        datum_t topic_d = d.get_field("topic", NOTHROW);
        rcheck_target(target, topic_d.has() && topic_d.get_type() == datum_t::R_STR,
                      base_exc_t::LOGIC,
                      "Kafka sink requires a string `topic`.");
        out.topic = topic_d.as_str().to_std();
        rcheck_target(target, !out.topic.empty(), base_exc_t::LOGIC,
                      "Kafka sink `topic` must not be empty.");
        break;
    }
    case cdc_sink_type_t::WEBHOOK: {
        datum_t url = d.get_field("url", NOTHROW);
        if (!url.has()) {
            url = d.get_field("connection", NOTHROW);
        }
        rcheck_target(target, url.has() && url.get_type() == datum_t::R_STR,
                      base_exc_t::LOGIC,
                      "Webhook sink requires a string `url`.");
        out.connection_string = url.as_str().to_std();
        rcheck_target(target, out.connection_string.find("https://") == 0
                                  || out.connection_string.find("http://") == 0,
                      base_exc_t::LOGIC,
                      "Webhook sink `url` must start with http:// or https://.");
        out.topic = "webhook";
        break;
    }
    case cdc_sink_type_t::FILE: {
        datum_t path = d.get_field("path", NOTHROW);
        if (!path.has()) {
            path = d.get_field("connection", NOTHROW);
        }
        rcheck_target(target, path.has() && path.get_type() == datum_t::R_STR,
                      base_exc_t::LOGIC,
                      "File sink requires a string `path`.");
        out.connection_string = path.as_str().to_std();
        rcheck_target(target, !out.connection_string.empty(), base_exc_t::LOGIC,
                      "File sink `path` must not be empty.");
        out.topic = "file";
        break;
    }
    case cdc_sink_type_t::S3: {
        datum_t bucket = d.get_field("bucket", NOTHROW);
        if (!bucket.has()) {
            bucket = d.get_field("connection", NOTHROW);
        }
        rcheck_target(target, bucket.has() && bucket.get_type() == datum_t::R_STR,
                      base_exc_t::LOGIC,
                      "S3 sink requires a string `bucket`.");
        out.connection_string = bucket.as_str().to_std();
        datum_t prefix = d.get_field("prefix", NOTHROW);
        if (!prefix.has()) {
            prefix = d.get_field("topic", NOTHROW);
        }
        if (prefix.has()) {
            rcheck_target(target, prefix.get_type() == datum_t::R_STR, base_exc_t::LOGIC,
                          "S3 sink `prefix` must be a string.");
            out.topic = prefix.as_str().to_std();
        } else {
            out.topic = "s3";
        }
        break;
    }
    default:
        unreachable();
    }

    // Credentials: credentialsRef / credentialRef — never literal secrets.
    datum_t cred = d.get_field("credentialsRef", NOTHROW);
    if (!cred.has()) {
        cred = d.get_field("credentialRef", NOTHROW);
    }
    if (!cred.has()) {
        cred = d.get_field("credential_ref", NOTHROW);
    }
    if (cred.has()) {
        rcheck_target(target, cred.get_type() == datum_t::R_STR, base_exc_t::LOGIC,
                      "`credentialsRef` must be a string secret reference.");
        out.credential_ref = cred.as_str().to_std();
        rcheck_target(target, !out.credential_ref.empty(), base_exc_t::LOGIC,
                      "`credentialsRef` must not be empty.");
    }
    // Reject literal secrets if someone stuffed them in.
    rcheck_target(target, !d.get_field("password", NOTHROW).has()
                              && !d.get_field("secret", NOTHROW).has()
                              && !d.get_field("token", NOTHROW).has(),
                  base_exc_t::LOGIC,
                  "CDC sink config must not contain literal secrets; use "
                  "`credentialsRef`.");

    parse_batching(target, d, &out.batching);

    // Optional deadLetter object: type + topic/url validated shallowly.
    datum_t dl = d.get_field("deadLetter", NOTHROW);
    if (!dl.has()) {
        dl = d.get_field("dead_letter", NOTHROW);
    }
    if (dl.has()) {
        rcheck_target(target, dl.get_type() == datum_t::R_OBJECT, base_exc_t::LOGIC,
                      "`deadLetter` must be an object.");
        datum_t dl_type = dl.get_field("type", NOTHROW);
        rcheck_target(target, dl_type.has() && dl_type.get_type() == datum_t::R_STR,
                      base_exc_t::LOGIC,
                      "`deadLetter.type` must be a string.");
        std::string dlt = dl_type.as_str().to_std();
        rcheck_target(target,
                      dlt == "kafka" || dlt == "webhook" || dlt == "file" || dlt == "s3",
                      base_exc_t::LOGIC,
                      "Unknown `deadLetter.type`; expected kafka/webhook/file/s3.");
    }

    return out;
}

/* ── ReQL terms ──────────────────────────────────────────────────────────── */

class cdc_sink_create_term_t : public meta_op_term_base_t {
public:
    cdc_sink_create_term_t(compile_env_t *env, const raw_term_t &term)
        : meta_op_term_base_t(env, term, argspec_t(2, 2)) { }

private:
    virtual scoped_ptr_t<val_t> eval_impl(
            scope_env_t *env, args_t *args, eval_flags_t) const {
        require_cdc_cluster_support(this);
        counted_t<table_t> table = args->arg(env, 0)->as_table();
        (void)table;
        scoped_ptr_t<val_t> config_val = args->arg(env, 1);
        parsed_cdc_sink_create_t cfg =
            parse_cdc_sink_config_from_datum(config_val->as_datum(),
                                             config_val.get());
        return new_val(stub_created_response(cfg.name.str()));
    }
    virtual const char *name() const { return "cdc_sink_create"; }
};

class cdc_sink_list_term_t : public meta_op_term_base_t {
public:
    cdc_sink_list_term_t(compile_env_t *env, const raw_term_t &term)
        : meta_op_term_base_t(env, term, argspec_t(1, 1)) { }

private:
    virtual scoped_ptr_t<val_t> eval_impl(
            scope_env_t *env, args_t *args, eval_flags_t) const {
        require_cdc_cluster_support(this);
        counted_t<table_t> table = args->arg(env, 0)->as_table();
        (void)table;
        return new_val(datum_t(std::vector<datum_t>(),
                               env->env->limits()));
    }
    virtual const char *name() const { return "cdc_sink_list"; }
};

class cdc_sink_status_term_t : public meta_op_term_base_t {
public:
    cdc_sink_status_term_t(compile_env_t *env, const raw_term_t &term)
        : meta_op_term_base_t(env, term, argspec_t(2, 2)) { }

private:
    virtual scoped_ptr_t<val_t> eval_impl(
            scope_env_t *env, args_t *args, eval_flags_t) const {
        require_cdc_cluster_support(this);
        counted_t<table_t> table = args->arg(env, 0)->as_table();
        (void)table;
        std::string sink_name = args->arg(env, 1)->as_str().to_std();
        name_string_t checked;
        bool ok = checked.assign_value(sink_name);
        rcheck(ok, base_exc_t::LOGIC,
               strprintf("CDC sink name `%s` invalid (%s).",
                         sink_name.c_str(),
                         name_string_t::valid_char_msg));

        ql::datum_object_builder_t res;
        res.overwrite("name", datum_t(datum_string_t(checked.str())));
        res.overwrite("state", datum_t(datum_string_t("unknown")));
        res.overwrite("message",
                      datum_t(datum_string_t(
                          "CDC term not yet wired to backend")));
        return new_val(std::move(res).to_datum());
    }
    virtual const char *name() const { return "cdc_sink_status"; }
};

class cdc_sink_drop_term_t : public meta_op_term_base_t {
public:
    cdc_sink_drop_term_t(compile_env_t *env, const raw_term_t &term)
        : meta_op_term_base_t(env, term, argspec_t(2, 2)) { }

private:
    virtual scoped_ptr_t<val_t> eval_impl(
            scope_env_t *env, args_t *args, eval_flags_t) const {
        require_cdc_cluster_support(this);
        counted_t<table_t> table = args->arg(env, 0)->as_table();
        (void)table;
        std::string sink_name = args->arg(env, 1)->as_str().to_std();
        name_string_t checked;
        bool ok = checked.assign_value(sink_name);
        rcheck(ok, base_exc_t::LOGIC,
               strprintf("CDC sink name `%s` invalid (%s).",
                         sink_name.c_str(),
                         name_string_t::valid_char_msg));
        return new_val(stub_dropped_response(checked.str()));
    }
    virtual const char *name() const { return "cdc_sink_drop"; }
};

counted_t<term_t> make_cdc_sink_create_term(
        compile_env_t *env, const raw_term_t &term) {
    return make_counted<cdc_sink_create_term_t>(env, term);
}
counted_t<term_t> make_cdc_sink_list_term(
        compile_env_t *env, const raw_term_t &term) {
    return make_counted<cdc_sink_list_term_t>(env, term);
}
counted_t<term_t> make_cdc_sink_status_term(
        compile_env_t *env, const raw_term_t &term) {
    return make_counted<cdc_sink_status_term_t>(env, term);
}
counted_t<term_t> make_cdc_sink_drop_term(
        compile_env_t *env, const raw_term_t &term) {
    return make_counted<cdc_sink_drop_term_t>(env, term);
}

}  // namespace ql
