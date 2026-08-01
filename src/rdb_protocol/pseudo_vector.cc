// Copyright 2026 RethinkDB, all rights reserved.
// Pseudo-type serialization for VECTOR datums. Mirrors pseudo_binary:
// vectors travel on the wire as {"$reql_type$": "VECTOR", "data": [..]}.
#include "rdb_protocol/pseudo_vector.hpp"

#include "errors.hpp"

#include "rapidjson/rapidjson.h"
#include "rdb_protocol/datum.hpp"

namespace ql {
namespace pseudo {

const char *const vector_string = "VECTOR";
const char *const vector_data_key = "data";

// Given a vector of doubles, encodes it into a `r.vector` pseudotype.
void encode_vector_ptype(
        const std::vector<double> &vec,
        rapidjson::Writer<rapidjson::StringBuffer> *writer) {
    writer->StartObject();
    writer->Key(datum_t::reql_type_string.data(), datum_t::reql_type_string.size());
    writer->String(vector_string);
    writer->Key(vector_data_key);
    writer->StartArray();
    for (double d : vec) {
        writer->Double(d);
    }
    writer->EndArray();
    writer->EndObject();
}

rapidjson::Value encode_vector_ptype(const std::vector<double> &vec,
                                     rapidjson::Value::AllocatorType *allocator) {
    rapidjson::Value res(rapidjson::kObjectType);
    res.AddMember(rapidjson::Value(datum_t::reql_type_string.data(),
                                   datum_t::reql_type_string.size(),
                                   *allocator),
                  rapidjson::Value(vector_string, *allocator), *allocator);
    rapidjson::Value arr(rapidjson::kArrayType);
    for (double d : vec) {
        arr.PushBack(rapidjson::Value(d), *allocator);
    }
    res.AddMember(rapidjson::Value(vector_data_key, *allocator), arr, *allocator);
    return res;
}

scoped_cJSON_t encode_vector_ptype(const std::vector<double> &vec) {
    scoped_cJSON_t res(cJSON_CreateObject());
    res.AddItemToObject(datum_t::reql_type_string.to_std().c_str(),
                        cJSON_CreateString(vector_string));
    cJSON *arr = cJSON_CreateArray();
    for (double d : vec) {
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(d));
    }
    res.AddItemToObject(vector_data_key, arr);
    return res;
}

}  // namespace pseudo
}  // namespace ql
