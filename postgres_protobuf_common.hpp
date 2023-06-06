#ifndef POSTGRES_PROTOBUF_POSTGRES_PROTOBUF_COMMON_HPP_
#define POSTGRES_PROTOBUF_POSTGRES_PROTOBUF_COMMON_HPP_

#include <stdint.h>
#include <string>
#include <utility>

#include <absl/status/status.h>

// ===========================================================================
// ==================== Debug macro (requires postgres.h) ====================
// ===========================================================================
#ifdef DEBUG_PRINT
#define PGPROTO_DEBUG(...) ereport(WARNING, (errmsg(__VA_ARGS__)))
#else
#define PGPROTO_DEBUG(...)
#endif

namespace postgres_protobuf {


}  // namespace postgres_protobuf


// =======================================================
// ==================== Other helpers ====================
// =======================================================

namespace postgres_protobuf {

class BadProto {
 public:
  BadProto(absl::string_view&& msg) : msg(msg) {}
  const std::string msg;
};

}  // namespace postgres_protobuf

#endif  // POSTGRES_PROTOBUF_POSTGRES_PROTOBUF_COMMON_HPP_
