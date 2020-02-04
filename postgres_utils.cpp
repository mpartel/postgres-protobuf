
#include "postgres_utils.hpp"

extern "C" {
#include <postgres.h>
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

}  // namespace postgres_utils
}  // namespace postgres_protobuf
