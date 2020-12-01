#include "descriptor_db.hpp"
#include "postgres_protobuf_common.hpp"
#include "postgres_utils.hpp"
#include "querying.hpp"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/util/json_util.h>

#include <cassert>

extern "C" {
// Must be included before other Postgres headers
#include <postgres.h>

#include <access/htup_details.h>
#include <catalog/pg_type.h>
#include <fmgr.h>
#include <funcapi.h>
#include <utils/array.h>
#include <utils/lsyscache.h>
}  // extern "C"

namespace postgres_protobuf {
namespace version {
const int64_t majorVersion = EXT_VERSION_MAJOR;
const int64_t minorVersion = EXT_VERSION_MINOR;
const int64_t patchlevel = EXT_VERSION_PATCHLEVEL;
constexpr int64_t numericVersion =
    majorVersion * 10000 + minorVersion * 100 + patchlevel;
}  // namespace version
}  // namespace postgres_protobuf

namespace pb = ::google::protobuf;

using namespace postgres_protobuf;
using namespace postgres_protobuf::postgres_utils;

extern "C" {

PG_MODULE_MAGIC;

}  // extern "C"

namespace {
class MultiQueryState {
 public:
  MultiQueryState() : index(0) {}

  pvector<text*> rows;
  size_t index;
};

class ProtobufNotFound {};

void GetProtobufInfoOrThrow(const pstring& desc_spec,
                            std::string* desc_name_append_out,
                            pb::util::TypeResolver** type_resolver_out) {
  const descriptor_db::DescDb& desc_db =
      *descriptor_db::DescDb::GetOrCreateCached();

  std::string::size_type i = desc_spec.find(':');

  std::string desc_set_name;
  int desc_name_start;
  if (i != std::string::npos) {
    desc_set_name = desc_spec.substr(0, i);
    desc_name_start = i + 1;
  } else {
    desc_set_name = "default";
    desc_name_start = 0;
  }

  auto di = desc_db.desc_sets.find(desc_set_name);
  if (di == desc_db.desc_sets.end()) {
    throw ProtobufNotFound();
  }

  std::string desc_name(desc_spec.substr(desc_name_start));

  if (!di->second->pool->FindMessageTypeByName(desc_name)) {
    throw ProtobufNotFound();
  }

  *desc_name_append_out += desc_name;
  *type_resolver_out = di->second->type_resolver.get();
}
}  // namespace

extern "C" {

PG_FUNCTION_INFO_V1(protobuf_extension_version);
PG_FUNCTION_INFO_V1(protobuf_query);
PG_FUNCTION_INFO_V1(protobuf_query_multi);
PG_FUNCTION_INFO_V1(protobuf_query_array);
PG_FUNCTION_INFO_V1(protobuf_to_json_text);
PG_FUNCTION_INFO_V1(protobuf_from_json_text);

Datum protobuf_extension_version(PG_FUNCTION_ARGS) {
  PG_RETURN_INT64(version::numericVersion);
}

Datum protobuf_query(PG_FUNCTION_ARGS) {
  using namespace querying;

  assert(PG_NARGS() == 2);

  try {
    text* query_text = PG_GETARG_TEXT_P(0);
    std::string query_str(VARDATA_ANY(query_text),
                          VARSIZE_ANY_EXHDR(query_text));
    querying::Query query(query_str, 1);
    PGPROTO_DEBUG("Query parsed");

    bytea* proto_bytea = PG_GETARG_BYTEA_P(1);
    const uint8* proto_data =
        reinterpret_cast<const uint8*>(VARDATA_ANY(proto_bytea));
    size_t proto_len = VARSIZE_ANY_EXHDR(proto_bytea);
    const auto rows = query.Run(proto_data, proto_len);
    PGPROTO_DEBUG("Query ran. Results: %lu", rows.size());
    if (!rows.empty()) {
      const std::string& row = rows[0];
      size_t size = VARHDRSZ + row.size();
      bytea* p = static_cast<bytea*>(palloc0_or_throw_bad_alloc(size));
      SET_VARSIZE(p, size);
      memcpy(VARDATA(p), row.data(), row.size());
      PG_RETURN_TEXT_P(p);
    } else {
      PG_RETURN_NULL();
    }
  } catch (const std::bad_alloc& e) {
    ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory")));
  } catch (const BadProto& e) {
    // TODO: is this a good error code?
    ereport(ERROR, (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                    errmsg("invalid protobuf: %s", e.msg.c_str())));
  } catch (const BadQuery& e) {
    // TODO: is this a good error code?
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("invalid query: %s", e.msg.c_str())));
  } catch (const RecursionDepthExceeded& e) {
    // TODO: is this a good error code?
    // TODO: make the limit configurable
    ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                    errmsg("protobuf recursion depth exceeded")));
  } catch (...) {
    ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("unknown C++ exception in postgres_protobuf extension")));
  }
}

Datum protobuf_query_array(PG_FUNCTION_ARGS) {
  using namespace querying;

  assert(PG_NARGS() == 2);

  try {
    text* query_text = PG_GETARG_TEXT_P(0);
    std::string query_str(VARDATA_ANY(query_text),
                          VARSIZE_ANY_EXHDR(query_text));
    querying::Query query(query_str, std::nullopt);
    PGPROTO_DEBUG("Query parsed");

    bytea* proto_bytea = PG_GETARG_BYTEA_P(1);
    const uint8* proto_data =
        reinterpret_cast<const uint8*>(VARDATA_ANY(proto_bytea));
    size_t proto_len = VARSIZE_ANY_EXHDR(proto_bytea);
    const auto rows = query.Run(proto_data, proto_len);
    PGPROTO_DEBUG("Query ran. Results: %lu", rows.size());
    Datum* elements = static_cast<Datum*>(palloc0_or_throw_bad_alloc(sizeof(Datum) * rows.size()));
    for (size_t i = 0; i < rows.size(); ++i) {
      const std::string& row = rows[i];
      size_t size = VARHDRSZ + row.size();
      bytea* p = static_cast<bytea*>(palloc0_or_throw_bad_alloc(size));
      SET_VARSIZE(p, size);
      memcpy(VARDATA(p), row.data(), row.size());
      elements[i] = reinterpret_cast<Datum>(p);
    }
    int16 typlen;
    bool typbyval;
    char typalign;
    get_typlenbyvalalign(TEXTOID, &typlen, &typbyval, &typalign);
    ArrayType* result = construct_array(elements, rows.size(), TEXTOID, typlen, typbyval, typalign);
    PG_RETURN_ARRAYTYPE_P(result);
  } catch (const std::bad_alloc& e) {
    ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory")));
  } catch (const BadProto& e) {
    // TODO: is this a good error code?
    ereport(ERROR, (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                    errmsg("invalid protobuf: %s", e.msg.c_str())));
  } catch (const BadQuery& e) {
    // TODO: is this a good error code?
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("invalid query: %s", e.msg.c_str())));
  } catch (const RecursionDepthExceeded& e) {
    // TODO: is this a good error code?
    // TODO: make the limit configurable
    ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                    errmsg("protobuf recursion depth exceeded")));
  } catch (...) {
    ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("unknown C++ exception in postgres_protobuf extension")));
  }
}

Datum protobuf_query_multi(PG_FUNCTION_ARGS) {
  using namespace querying;

  assert(PG_NARGS() == 2);

  try {
    FuncCallContext* funcctx;
    MultiQueryState* state;

    if (SRF_IS_FIRSTCALL()) {
      funcctx = SRF_FIRSTCALL_INIT();
      MemoryContext old_context =
          MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
      text* query_text = PG_GETARG_TEXT_P(0);
      std::string query_str(VARDATA_ANY(query_text),
                            VARSIZE_ANY_EXHDR(query_text));
      querying::Query query(query_str, std::nullopt);
      PGPROTO_DEBUG("Query parsed");

      state = pnew<MultiQueryState>();
      funcctx->user_fctx = state;
      bytea* proto_bytea = PG_GETARG_BYTEA_P(1);
      const uint8* proto_data =
          reinterpret_cast<const uint8*>(VARDATA_ANY(proto_bytea));
      size_t proto_len = VARSIZE_ANY_EXHDR(proto_bytea);
      for (const std::string& row : query.Run(proto_data, proto_len)) {
        size_t size = VARHDRSZ + row.size();
        bytea* p = static_cast<bytea*>(palloc0_or_throw_bad_alloc(size));
        SET_VARSIZE(p, size);
        memcpy(VARDATA(p), row.data(), row.size());
        state->rows.push_back(p);
      }
      PGPROTO_DEBUG("Query ran. Results: %lu", state->rows.size());

      MemoryContextSwitchTo(old_context);
    }

    funcctx = SRF_PERCALL_SETUP();
    state = static_cast<MultiQueryState*>(funcctx->user_fctx);

    if (state->index < state->rows.size()) {
      int i = state->index++;
      SRF_RETURN_NEXT(funcctx, (Datum)state->rows[i]);
    } else {
      SRF_RETURN_DONE(funcctx);
    }
  } catch (const std::bad_alloc& e) {
    ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory")));
  } catch (const BadProto& e) {
    // TODO: is this a good error code?
    ereport(ERROR, (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                    errmsg("invalid protobuf: %s", e.msg.c_str())));
  } catch (const BadQuery& e) {
    // TODO: is this a good error code?
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("invalid query: %s", e.msg.c_str())));
  } catch (const RecursionDepthExceeded& e) {
    // TODO: is this a good error code?
    // TODO: make the limit configurable
    ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                    errmsg("protobuf recursion depth exceeded")));
  } catch (...) {
    ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("unknown C++ exception in postgres_protobuf extension")));
  }
}

Datum protobuf_to_json_text(PG_FUNCTION_ARGS) {
  text* protobuf_type_text = PG_GETARG_TEXT_P(0);
  pstring protobuf_type_str(VARDATA_ANY(protobuf_type_text),
                            VARSIZE_ANY_EXHDR(protobuf_type_text));

  try {
    bytea* proto_bytea = PG_GETARG_BYTEA_P(1);
    const uint8* proto_data =
        reinterpret_cast<const uint8*>(VARDATA_ANY(proto_bytea));
    size_t proto_len = VARSIZE_ANY_EXHDR(proto_bytea);
    std::string proto_str(reinterpret_cast<const char*>(proto_data), proto_len);

    std::string type_url = "type.googleapis.com/";
    pb::util::TypeResolver* type_resolver = nullptr;
    GetProtobufInfoOrThrow(protobuf_type_str, &type_url, &type_resolver);

    std::string json_str;
    pb::util::Status status = pb::util::BinaryToJsonString(
        type_resolver, type_url, proto_str, &json_str);
    if (!status.ok()) {
      throw BadProto(status.error_message());
    }

    size_t result_size = VARHDRSZ + json_str.size();
    bytea* result =
        static_cast<bytea*>(palloc0_or_throw_bad_alloc(result_size));
    SET_VARSIZE(result, result_size);
    memcpy(VARDATA(result), json_str.data(), json_str.size());
    PG_RETURN_TEXT_P(result);
  } catch (const std::bad_alloc& e) {
    ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory")));
  } catch (const ProtobufNotFound& e) {
    ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                    errmsg("invalid query: protobuf type %s not found",
                           protobuf_type_str.c_str())));
  } catch (...) {
    ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("unknown C++ exception in postgres_protobuf extension")));
  }
}

Datum protobuf_from_json_text(PG_FUNCTION_ARGS) {
  text* protobuf_type_text = PG_GETARG_TEXT_P(0);
  pstring protobuf_type_str(VARDATA_ANY(protobuf_type_text),
                            VARSIZE_ANY_EXHDR(protobuf_type_text));

  try {
    text* json_text = PG_GETARG_TEXT_P(1);
    std::string json_str(VARDATA_ANY(json_text), VARSIZE_ANY_EXHDR(json_text));

    std::string type_url = "type.googleapis.com/";
    pb::util::TypeResolver* type_resolver = nullptr;
    GetProtobufInfoOrThrow(protobuf_type_str, &type_url, &type_resolver);

    std::string proto_str;
    pb::util::Status status = pb::util::JsonToBinaryString(
        type_resolver, type_url, json_str, &proto_str);
    if (!status.ok()) {
      throw BadProto(status.error_message());
    }

    size_t result_size = VARHDRSZ + proto_str.size();
    bytea* result =
        static_cast<bytea*>(palloc0_or_throw_bad_alloc(result_size));
    SET_VARSIZE(result, result_size);
    memcpy(VARDATA(result), proto_str.data(), proto_str.size());
    PG_RETURN_BYTEA_P(result);
  } catch (const std::bad_alloc& e) {
    ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory")));
  } catch (const ProtobufNotFound& e) {
    ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                    errmsg("invalid query: protobuf type %s not found",
                           protobuf_type_str.c_str())));
  } catch (...) {
    ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("unknown C++ exception in postgres_protobuf extension")));
  }
}

// Module finarlizer
void _PG_fini() {
  descriptor_db::DescDb::ClearCache();
  pb::ShutdownProtobufLibrary();
}

}  // extern "C"
