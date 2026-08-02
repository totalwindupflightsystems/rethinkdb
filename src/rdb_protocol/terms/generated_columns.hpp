// Copyright 2026 RethinkDB contributors, all rights reserved.
#ifndef RDB_PROTOCOL_TERMS_GENERATED_COLUMNS_HPP_
#define RDB_PROTOCOL_TERMS_GENERATED_COLUMNS_HPP_

#include <map>
#include <string>

#include "rdb_protocol/context.hpp"
#include "rdb_protocol/datum.hpp"
#include "rdb_protocol/func.hpp"
#include "rdb_protocol/term_storage.hpp"

namespace ql {

// The declarations for the terms in generated_columns.cc are in terms.hpp

// Parse a raw {column_name: func} object term -> wire-func map (validates
// determinism and arity). Throws ql::exc_t on invalid input.
std::map<std::string, wire_func_t> parse_generated_columns_raw(
        const raw_term_t &obj_term);

// Format a wire-func map back into a readable datum.
datum_t format_generated_columns_datum(
        const std::map<std::string, wire_func_t> &config);

}  // namespace ql

#endif // RDB_PROTOCOL_TERMS_GENERATED_COLUMNS_HPP_
