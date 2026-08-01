// Copyright 2026 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_PSEUDO_VECTOR_HPP_
#define RDB_PROTOCOL_PSEUDO_VECTOR_HPP_

#include <vector>

#include "rapidjson/document.h"
#include "rapidjson/rapidjson.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include "rdb_protocol/datum.hpp"

namespace ql {
namespace pseudo {

extern const char *const vector_string;
extern const char *const vector_data_key;

void encode_vector_ptype(
        const std::vector<double> &vec,
        rapidjson::Writer<rapidjson::StringBuffer> *writer);
rapidjson::Value encode_vector_ptype(
        const std::vector<double> &vec,
        rapidjson::Value::AllocatorType *allocator);
scoped_cJSON_t encode_vector_ptype(const std::vector<double> &vec);

}  // namespace pseudo
}  // namespace ql

#endif  // RDB_PROTOCOL_PSEUDO_VECTOR_HPP_
