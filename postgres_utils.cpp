
#include "postgres_utils.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>

extern "C" {
#include <pg_config.h>
#include <postgres.h>
#include <utils/builtins.h>
#if PG_VERSION_NUM >= 120000
#include <common/shortest_dec.h>
#include <utils/float.h>
#endif
}

namespace postgres_protobuf {
namespace postgres_utils {

void* palloc0_or_throw_bad_alloc(size_t size) {
  void* p = palloc_extended(size, MCXT_ALLOC_NO_OOM | MCXT_ALLOC_ZERO);
  if (p == nullptr) {
    throw std::bad_alloc();
  }
  return p;
}

std::string float_to_string(float x) {
  // Matches behaviour of `float4out`
#if PG_VERSION_NUM >= 120000
  char buf[32];
  static_assert(32 >= FLOAT_SHORTEST_DECIMAL_LEN);
  if (extra_float_digits > 0) {
    float_to_shortest_decimal_buf(x, buf);
  } else {
    pg_strfromd(buf, 32, FLT_DIG + extra_float_digits, x);
  }
  return std::string(buf);
#else
  if (std::isnan(x)) {
    return "NaN";
  }
  int inf = std::isinf(x);
  if (inf == 1) {
    return "Infinity";
  } else if (inf == -1) {
    return "-Infinity";
  } else {
    int digits = std::max(1, FLT_DIG + extra_float_digits);
    char* buf = psprintf("%.*g", digits, x);
    std::string s(buf);
    pfree(buf);
    return s;
  }
#endif
}

std::string double_to_string(double x) {
  // Matches behaviour of `float8out`
#if PG_VERSION_NUM >= 120000
  char buf[32];
  static_assert(32 >= DOUBLE_SHORTEST_DECIMAL_LEN);
  if (extra_float_digits > 0) {
    double_to_shortest_decimal_buf(x, buf);
  } else {
    pg_strfromd(buf, 32, DBL_DIG + extra_float_digits, x);
  }
  return std::string(buf);
#else
  char* buf = float8out_internal(x);
  std::string s(buf);
  pfree(buf);
  return s;
#endif
}

}  // namespace postgres_utils
}  // namespace postgres_protobuf
