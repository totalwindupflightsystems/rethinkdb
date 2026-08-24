// Copyright 2026 RethinkDB, all rights reserved.
#include "rdb_protocol/cdc_pump.hpp"

#include <functional>
#include <utility>

#include "arch/runtime/coroutines.hpp"
#include "arch/timing.hpp"
#include "clustering/administration/namespace_interface_repository.hpp"
#include "clustering/administration/tables/table_metadata.hpp"
#include "clustering/table_manager/table_meta_client.hpp"
#include "logger.hpp"
#include "protocol_api.hpp"
#include "rdb_protocol/datum.hpp"
#include "rdb_protocol/datum_stream.hpp"
#include "rdb_protocol/env.hpp"
#include "rdb_protocol/error.hpp"
#include "rdb_protocol/protocol.hpp"
#include "rdb_protocol/pseudo_time.hpp"
#include "rdb_protocol/val.hpp"
#include "time.hpp"

namespace ql {

cdc_pump_t::cdc_pump_t(rdb_context_t *rdb_context,
                       table_meta_client_t *table_meta_client,
                       namespace_repo_t *namespace_repo,
                       ql::changefeed::client_t *changefeed_client)
    : rdb_context_(rdb_context),
      table_meta_client_(table_meta_client),
      namespace_repo_(namespace_repo),
      changefeed_client_(changefeed_client) {
    guarantee(rdb_context_ != nullptr);
    guarantee(table_meta_client_ != nullptr);
    guarantee(namespace_repo_ != nullptr);
    guarantee(changefeed_client_ != nullptr);
    coro_t::spawn_sometime(std::bind(&cdc_pump_t::run, this, drainer_.lock()));
}

cdc_pump_t::~cdc_pump_t() {
    drainer_.drain();
}

void cdc_pump_t::run(auto_drainer_t::lock_t keepalive) {
    try {
        for (;;) {
            signal_t *drain_signal = keepalive.get_drain_signal();
            if (drain_signal->is_pulsed()) {
                return;
            }
            try {
                drive_subscriptions(drain_signal, keepalive);
            } catch (const interrupted_exc_t &) {
                return;
            } catch (const std::exception &e) {
                logWRN("cdc_pump: metadata scan failed: %s", e.what());
            }
            try {
                nap(POLL_INTERVAL_MS, drain_signal);
            } catch (const interrupted_exc_t &) {
                return;
            }
        }
    } catch (const interrupted_exc_t &) {
        /* Draining. */
    }
}

void cdc_pump_t::drive_subscriptions(signal_t *interruptor,
                                     auto_drainer_t::lock_t keepalive) {
    std::map<namespace_id_t, table_basic_config_t> tables;
    table_meta_client_->list_names(&tables);

    for (const auto &table_pair : tables) {
        if (interruptor->is_pulsed()) {
            return;
        }
        const namespace_id_t &table_id = table_pair.first;

        table_config_and_shards_t config;
        try {
            table_meta_client_->get_config(table_id, interruptor, &config);
        } catch (const no_such_table_exc_t &) {
            continue;
        } catch (const failed_table_op_exc_t &) {
            continue;
        }

        for (const auto &sub_pair : config.subscriptions) {
            const subscription_config_t &sub = sub_pair.second;
            if (sub.state != subscription_state_t::CREATING) {
                continue;
            }
            if (in_flight_subscriptions_.count(sub.subscription_id) > 0) {
                continue;
            }

            /* Resolve the publication this subscription references. */
            publication_config_t pub;
            if (!find_publication_by_name(
                    config.publications, sub.publication_name, &pub)) {
                logWRN("cdc_pump: subscription `%s` references unknown "
                       "publication `%s`; leaving in CREATING",
                       sub.name.c_str(), sub.publication_name.c_str());
                continue;
            }

            in_flight_subscriptions_.insert(sub.subscription_id);
            coro_t::spawn_sometime(std::bind(
                &cdc_pump_t::stream_subscription_coro, this,
                sub, pub, table_pair.second.name.str(), keepalive));
        }
    }
}

void cdc_pump_t::stream_subscription_coro(subscription_config_t sub,
                                          publication_config_t pub,
                                          std::string table_name,
                                          auto_drainer_t::lock_t keepalive) {
    signal_t *drain_signal = keepalive.get_drain_signal();
    try {
        subscription_handle_t handle(sub);
        subscription_applier_t applier(&handle);

        /* Resolve the target table's primary-key field name so the
         * target-writer can extract the key from change images (RT-GAP-015). */
        table_config_and_shards_t target_config;
        std::string target_pk = "id";
        try {
            table_meta_client_->get_config(
                sub.target_table_id, drain_signal, &target_config);
            target_pk = target_config.config.basic.primary_key;
        } catch (const no_such_table_exc_t &) {
            logWRN("cdc_pump: subscription `%s` target table missing; "
                   "leaving in CREATING", sub.name.c_str());
            return;
        } catch (const failed_table_op_exc_t &) {
            logWRN("cdc_pump: subscription `%s` target table unavailable; "
                   "leaving in CREATING", sub.name.c_str());
            return;
        }

        /* Target-writer seam: perform the real target-table write through
         * the namespace interface. */
        applier.set_target_writer(
            [this, &sub, target_pk](const change_record_t &record, signal_t *interruptor) {
                namespace_interface_access_t access =
                    namespace_repo_->get_namespace_interface(
                        sub.target_table_id, interruptor);
                namespace_interface_t *nif = access.get();
                if (nif == nullptr) {
                    return false;
                }

                write_response_t response;
                if (record.op == change_operation_t::DELETE) {
                    datum_t before = deserialize_datum_from_vector(
                        record.before_image);
                    if (!before.has()) {
                        return false;
                    }
                    datum_t pk_val = before.get_field(target_pk.c_str(), NOTHROW);
                    if (!pk_val.has()) {
                        return false;
                    }
                    std::string pkey = pk_val.print_primary();
                    write_t write(
                        point_delete_t(store_key_t(pkey)),
                        DURABILITY_REQUIREMENT_DEFAULT,
                        profile_bool_t::DONT_PROFILE,
                        configured_limits_t());
                    nif->write(
                        auth::user_context_t(auth::permissions_t(
                            tribool::True, tribool::True,
                            tribool::True, tribool::True)),
                        write, &response, order_token_t::ignore, interruptor);
                    return true;
                }

                datum_t after = deserialize_datum_from_vector(
                    record.after_image);
                if (!after.has()) {
                    return false;
                }
                datum_t pk_val = after.get_field(target_pk.c_str(), NOTHROW);
                if (!pk_val.has()) {
                    return false;
                }
                std::string pkey = pk_val.print_primary();
                write_t write(
                    point_write_t(store_key_t(pkey), after, true),
                    DURABILITY_REQUIREMENT_DEFAULT,
                    profile_bool_t::DONT_PROFILE,
                    configured_limits_t());
                nif->write(
                    auth::user_context_t(auth::permissions_t(
                        tribool::True, tribool::True,
                        tribool::True, tribool::True)),
                    write, &response, order_token_t::ignore, interruptor);
                return true;
            });

        /* Drive the lifecycle transitions. */
        std::string reason;
        if (!transition_creating_to_connecting(
                &handle, pub.publication_id, pub.database_id, pub.table_id,
                &reason)) {
            logWRN("cdc_pump: subscription `%s` CREATING→CONNECTING failed: %s",
                   sub.name.c_str(), reason.c_str());
            return;
        }
        /* Snapshot mode NONE: skip the snapshot partition and go straight
         * to CATCHING_UP with an empty live-start position set. */
        handle.snapshot_mode = snapshot_mode_t::NONE;
        if (!transition_connecting_to_catching_up(
                &handle, std::vector<snapshot_barrier_t>(), generate_uuid(),
                &reason)) {
            logWRN("cdc_pump: subscription `%s` CONNECTING→CATCHING_UP failed: %s",
                   sub.name.c_str(), reason.c_str());
            return;
        }
        if (!transition_catching_up_to_streaming(&handle, &reason)) {
            logWRN("cdc_pump: subscription `%s` CATCHING_UP→STREAMING failed: %s",
                   sub.name.c_str(), reason.c_str());
            return;
        }

        logNTC("cdc_pump: subscription `%s` streaming from table `%s`",
               sub.name.c_str(), table_name.c_str());

        stream_subscription(&handle, &applier, pub.table_id, table_name,
                            drain_signal);
    } catch (const interrupted_exc_t &) {
        /* Draining. */
    } catch (const std::exception &e) {
        logWRN("cdc_pump: subscription `%s` stream failed: %s",
               sub.name.c_str(), e.what());
    }
    in_flight_subscriptions_.erase(sub.subscription_id);
}

size_t cdc_pump_t::stream_subscription(subscription_handle_t *handle,
                                       subscription_applier_t *applier,
                                       const uuid_u &source_table_id,
                                       const std::string &table_name,
                                       signal_t *interruptor) {
    /* Build the admin user context for internal reads/writes. */
    auth::user_context_t admin_user(
        auth::permissions_t(tribool::True, tribool::True,
                            tribool::True, tribool::True));

    env_t env(
        rdb_context_,
        return_empty_normal_batches_t::NO,
        interruptor,
        serializable_env_t{
            global_optargs_t(),
            admin_user,
            pseudo::make_time(current_microtime() / 1.0e6, "+00:00")},
        nullptr);

    /* Open a changefeed stream over the whole source table. */
    changefeed::streamspec_t streamspec(
        counted_t<datum_stream_t>(),
        table_name,
        false,  /* include_offsets */
        false,  /* include_states */
        false,  /* include_types */
        configured_limits_t(),
        datum_t::boolean(false),  /* squash */
        changefeed::keyspec_t::range_t{
            std::vector<transform_variant_t>(),
            r_nullopt,
            sorting_t::UNORDERED,
            datumspec_t(datum_range_t::universe()),
            r_nullopt});

    counted_t<datum_stream_t> stream =
        changefeed_client_->new_stream(
            &env, streamspec, source_table_id, backtrace_id_t::empty());

    /* RT-GAP-037: the changefeed is open — rows will now flow to the
    target. Persist the STREAMING transition through Raft so
    subscription_status reports 'streaming' instead of the hardcoded
    CREATING that left every live subscription stuck in 'creating'
    forever. Best-effort: on failure the data path keeps streaming and
    the persisted state stays as-is (the pump never re-drives this
    subscription while it is in flight). */
    try {
        table_config_and_shards_change_t state_change(
            table_config_and_shards_change_t::subscription_set_state_t{
                source_table_id, handle->config.subscription_id,
                subscription_state_t::STREAMING});
        table_meta_client_->set_config(
            source_table_id, state_change, interruptor);
    } catch (const no_such_table_exc_t &) {
        logWRN("cdc_pump: subscription `%s` source table gone; "
               "cannot persist STREAMING state",
               handle->config.name.c_str());
    } catch (const failed_table_op_exc_t &) {
        logWRN("cdc_pump: subscription `%s` metadata update failed; "
               "persisted state stays as-is",
               handle->config.name.c_str());
    }

    uuid_u shard_id = generate_uuid();
    log_sequence_number_t lsn{0};
    size_t applied = 0;

    for (;;) {
        if (interruptor->is_pulsed()) {
            break;
        }
        /* Changefeed streams only accept NORMAL batches (stream_t rejects
         * TERMINAL). next() blocks until a change arrives or the
         * interruptor fires. */
        datum_t change = stream->next(
            &env, batchspec_t::user(batch_type_t::NORMAL, &env));
        if (!change.has()) {
            break;
        }
        if (handle->is_cancel_requested()) {
            break;
        }

        lsn.value += 1;
        change_record_t record = change_datum_to_record(
            change, handle->config.source_cluster_id, source_table_id,
            shard_id, lsn);

        std::vector<change_record_t> batch;
        batch.push_back(std::move(record));
        applier->apply_batch(batch, interruptor);
        ++applied;
    }
    return applied;
}

change_record_t cdc_pump_t::change_datum_to_record(
    const datum_t &change,
    const uuid_u &source_cluster_id,
    const uuid_u &source_table_id,
    const uuid_u &shard_id,
    log_sequence_number_t lsn) {
    change_record_t record;
    record.event_id.source_cluster_id = source_cluster_id;
    record.event_id.table_id = source_table_id;
    record.event_id.shard_id = shard_id;
    record.event_id.lsn = lsn;
    record.commit_timestamp = current_microtime();

    datum_t old_val = change.get_field("old_val", NOTHROW);
    datum_t new_val = change.get_field("new_val", NOTHROW);

    /* A missing field and an explicit R_NULL both mean "no value" for
     * changefeed change datums. */
    bool has_old = old_val.has() && old_val.get_type() != datum_t::R_NULL;
    bool has_new = new_val.has() && new_val.get_type() != datum_t::R_NULL;

    if (!has_old && has_new) {
        record.op = change_operation_t::INSERT;
        record.after_image = serialize_datum_to_vector(new_val);
    } else if (has_old && !has_new) {
        record.op = change_operation_t::DELETE;
        record.before_image = serialize_datum_to_vector(old_val);
    } else if (has_old && has_new) {
        record.op = change_operation_t::UPDATE;
        record.before_image = serialize_datum_to_vector(old_val);
        record.after_image = serialize_datum_to_vector(new_val);
    } else {
        /* Neither side present — skip (shouldn't happen for real changes). */
        record.op = change_operation_t::REPLACE;
    }
    return record;
}

}  // namespace ql
