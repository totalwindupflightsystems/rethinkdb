#ifndef RDB_PROTOCOL_DATUM_STREAM_READGENS_HPP_
#define RDB_PROTOCOL_DATUM_STREAM_READGENS_HPP_

#include "rdb_protocol/datum_stream.hpp"

namespace ql {

// This class generates the `read_t`s used in range reads.  It's used by
// `reader_t` below.  Its subclasses are the different types of range reads we
// need to do.
class readgen_t {
public:
    explicit readgen_t(
        serializable_env_t s_env,
        std::string table_name,
        profile_bool_t profile,
        read_mode_t read_mode,
        sorting_t sorting);
    virtual ~readgen_t() { }

    virtual read_t terminal_read(
        const std::vector<transform_variant_t> &transform,
        const terminal_variant_t &_terminal,
        const batchspec_t &batchspec) const = 0;
    // This has to be on `readgen_t` because we sort differently depending on
    // the kinds of reads we're doing.
    virtual void sindex_sort(std::vector<rget_item_t> *vec,
                             const batchspec_t &batchspec) const = 0;

    virtual read_t next_read(
        const optional<active_ranges_t> &active_ranges,
        const optional<reql_version_t> &reql_version,
        optional<changefeed_stamp_t> stamp,
        std::vector<transform_variant_t> transform,
        const batchspec_t &batchspec) const = 0;

    virtual key_range_t original_keyrange(reql_version_t rv) const = 0;
    virtual void restrict_active_ranges(
        sorting_t sorting, active_ranges_t *ranges_inout) const = 0;
    virtual optional<std::string> sindex_name() const = 0;

    virtual changefeed::keyspec_t::range_t get_range_spec(
        std::vector<transform_variant_t>) const = 0;

    const std::string &get_table_name() const { return table_name; }
    read_mode_t get_read_mode() const { return read_mode; }
    // Returns `sorting_` unless the batchspec overrides it.
    sorting_t sorting(const batchspec_t &batchspec) const;
protected:
    const serializable_env_t serializable_env;
    const std::string table_name;
    const profile_bool_t profile;
    const read_mode_t read_mode;
    const sorting_t sorting_;
};

class rget_readgen_t : public readgen_t {
public:
    explicit rget_readgen_t(
        serializable_env_t s_env,
        std::string table_name,
        const datumspec_t &datumspec,
        profile_bool_t profile,
        read_mode_t read_mode,
        sorting_t sorting,
        require_sindexes_t require_sindex_val);

    virtual read_t terminal_read(
        const std::vector<transform_variant_t> &transform,
        const terminal_variant_t &_terminal,
        const batchspec_t &batchspec) const;

    virtual read_t next_read(
        const optional<active_ranges_t> &active_ranges,
        const optional<reql_version_t> &reql_version,
        optional<changefeed_stamp_t> stamp,
        std::vector<transform_variant_t> transform,
        const batchspec_t &batchspec) const;

private:
    virtual rget_read_t next_read_impl(
        const optional<active_ranges_t> &active_ranges,
        const optional<reql_version_t> &reql_version,
        optional<changefeed_stamp_t> stamp,
        std::vector<transform_variant_t> transforms,
        const batchspec_t &batchspec) const = 0;

protected:
    datumspec_t datumspec;
    require_sindexes_t require_sindex_val;
};

class primary_readgen_t : public rget_readgen_t {
public:
    static scoped_ptr_t<readgen_t> make(
        env_t *env,
        std::string table_name,
        read_mode_t read_mode,
        const datumspec_t &datumspec = datumspec_t(datum_range_t::universe()),
        sorting_t sorting = sorting_t::UNORDERED);

private:
    primary_readgen_t(serializable_env_t s_env,
                      std::string table_name,
                      const datumspec_t &datumspec,
                      profile_bool_t profile,
                      read_mode_t read_mode,
                      sorting_t sorting);
    virtual rget_read_t next_read_impl(
        const optional<active_ranges_t> &active_ranges,
        const optional<reql_version_t> &reql_version,
        optional<changefeed_stamp_t> stamp,
        std::vector<transform_variant_t> transform,
        const batchspec_t &batchspec) const;
    virtual void sindex_sort(std::vector<rget_item_t> *vec,
                             const batchspec_t &batchspec) const;
    virtual key_range_t original_keyrange(reql_version_t rv) const;
    virtual optional<std::string> sindex_name() const;
    void restrict_active_ranges(
        sorting_t sorting, active_ranges_t *active_ranges_inout) const final;

    virtual changefeed::keyspec_t::range_t get_range_spec(
            std::vector<transform_variant_t> transforms) const;

    optional<std::map<store_key_t, uint64_t> > store_keys;
};

class sindex_readgen_t : public rget_readgen_t {
public:
    static scoped_ptr_t<readgen_t> make(
        env_t *env,
        std::string table_name,
        read_mode_t read_mode,
        const std::string &sindex,
        const datumspec_t &datumspec = datumspec_t(datum_range_t::universe()),
        sorting_t sorting = sorting_t::UNORDERED,
        require_sindexes_t require_sindex_val = require_sindexes_t::NO);

    virtual void sindex_sort(std::vector<rget_item_t> *vec,
                             const batchspec_t &batchspec) const;
    virtual key_range_t original_keyrange(reql_version_t rv) const;
    virtual optional<std::string> sindex_name() const;
    void restrict_active_ranges(sorting_t, active_ranges_t *) const final { }
private:
    sindex_readgen_t(
        serializable_env_t s_env,
        std::string table_name,
        const std::string &sindex,
        const datumspec_t &datumspec,
        profile_bool_t profile,
        read_mode_t read_mode,
        sorting_t sorting,
        require_sindexes_t require_sindex_val);
    virtual rget_read_t next_read_impl(
        const optional<active_ranges_t> &active_ranges,
        const optional<reql_version_t> &reql_version,
        optional<changefeed_stamp_t> stamp,
        std::vector<transform_variant_t> transform,
        const batchspec_t &batchspec) const;

    virtual changefeed::keyspec_t::range_t get_range_spec(
        std::vector<transform_variant_t> transforms) const;

    const std::string sindex;
    bool sent_first_read;
};

/* PHASE3-TS-3: readgen for `between` on a time-series table's time field
 * (spec §4.2/§6.3). The key space is the full primary-key range (rows are
 * ordered by pkey inside each chunk, not by time); the time window travels
 * in `rget_read_t::ts_range` and the store prunes the scanned chunk roots
 * to those overlapping the window. */
class ts_readgen_t : public rget_readgen_t {
public:
    static scoped_ptr_t<readgen_t> make(
        env_t *env,
        std::string table_name,
        read_mode_t read_mode,
        const datumspec_t &datumspec,
        sorting_t sorting,
        const std::string &time_field);

    virtual void sindex_sort(std::vector<rget_item_t> *vec,
                             const batchspec_t &batchspec) const;
    virtual key_range_t original_keyrange(reql_version_t rv) const;
    virtual optional<std::string> sindex_name() const;
    void restrict_active_ranges(sorting_t, active_ranges_t *) const final { }

    virtual changefeed::keyspec_t::range_t get_range_spec(
            std::vector<transform_variant_t> transforms) const;

private:
    ts_readgen_t(
        serializable_env_t s_env,
        std::string table_name,
        const datumspec_t &datumspec,
        profile_bool_t profile,
        read_mode_t read_mode,
        sorting_t sorting,
        const std::string &time_field);
    virtual rget_read_t next_read_impl(
        const optional<active_ranges_t> &active_ranges,
        const optional<reql_version_t> &reql_version,
        optional<changefeed_stamp_t> stamp,
        std::vector<transform_variant_t> transform,
        const batchspec_t &batchspec) const;

    ts_between_range_t ts_range;
};

/* PHASE3-TS-3: true when `datumspec`'s bounds form a time-range over a
 * time-series table — each bound is a ReQL time, r.minval or r.maxval,
 * with at least one bound an actual time. Used by the read dispatch to
 * resolve plain `between` (no index optarg) on time-series tables. */
bool ts_bounds_are_time_like(const datumspec_t &datumspec);

// For geospatial intersection queries
class intersecting_readgen_t : public readgen_t {
public:
    static scoped_ptr_t<readgen_t> make(
        env_t *env,
        std::string table_name,
        read_mode_t read_mode,
        const std::string &sindex,
        const datum_t &query_geometry);

    virtual read_t terminal_read(
        const std::vector<transform_variant_t> &transform,
        const terminal_variant_t &_terminal,
        const batchspec_t &batchspec) const;

    virtual read_t next_read(
        const optional<active_ranges_t> &active_ranges,
        const optional<reql_version_t> &reql_version,
        optional<changefeed_stamp_t> stamp,
        std::vector<transform_variant_t> transform,
        const batchspec_t &batchspec) const;

    virtual void sindex_sort(std::vector<rget_item_t> *vec,
                             const batchspec_t &batchspec) const;
    virtual key_range_t original_keyrange(reql_version_t rv) const;
    virtual optional<std::string> sindex_name() const;
    void restrict_active_ranges(sorting_t, active_ranges_t *) const final { }

    virtual changefeed::keyspec_t::range_t get_range_spec(
        std::vector<transform_variant_t>) const;

private:
    intersecting_readgen_t(
        serializable_env_t s_env,
        std::string table_name,
        const std::string &sindex,
        const datum_t &query_geometry,
        profile_bool_t profile,
        read_mode_t read_mode);

    // Analogue to rget_readgen_t::next_read_impl(), but generates an intersecting
    // geo read.
    intersecting_geo_read_t next_read_impl(
        const optional<active_ranges_t> &active_ranges,
        const optional<reql_version_t> &reql_version,
        optional<changefeed_stamp_t> stamp,
        std::vector<transform_variant_t> transforms,
        const batchspec_t &batchspec) const;

    const std::string sindex;
    const datum_t query_geometry;
};


}  // namespace ql

#endif  // RDB_PROTOCOL_DATUM_STREAM_READGENS_HPP_
