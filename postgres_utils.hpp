#ifndef POSTGRES_PROTOBUF_POSTGRES_UTILS_HPP_
#define POSTGRES_PROTOBUF_POSTGRES_UTILS_HPP_

#include <stdint.h>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

extern "C" {
// We don't want to include `postgres.h` here since it pollutes the namespace,
// causing problems with any protobuf includes that come after it.
extern void pfree(void* pointer);
}

namespace postgres_protobuf {
namespace postgres_utils {

// ===================================================================
// ==================== Memory allocation helpers ====================
// ===================================================================

void* palloc0_or_throw_bad_alloc(size_t size);

// Allocate and construct a C++ object in the current Postgres memory context.
// Throws std::bad_alloc on failure
template <typename T, typename... Args>
T* pnew(Args&&... args) {
  void* block = palloc0_or_throw_bad_alloc(sizeof(T));
  T* tp = nullptr;
  try {
    tp = new (block) T(std::forward<Args>(args)...);
  } catch (...) {
    pfree(block);
    throw;
  }
  return tp;
}

// Like pfree but calls the destructor first.
template <typename T>
void pdelete(T* p) {
  p->~T();
  pfree(p);
}

// TODO: pass memory context explicitly?
template <typename T>
class PostgresAllocator : public std::allocator<T> {
 public:
  typedef T value_type;

  PostgresAllocator() = default;
  template <class U>
  constexpr PostgresAllocator(const PostgresAllocator<U>&) noexcept {}

  T* allocate(size_t n) {
    if (n > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
      throw std::bad_alloc();
    }
    void* p = palloc0_or_throw_bad_alloc(n * sizeof(T));
#ifdef DEBUG_ALLOC
    PGPROTO_DEBUG("ALLOC %lx IN %s", (intptr_t)p, CurrentMemoryContext->name);
#endif
    if (p == nullptr) {
      throw std::bad_alloc();
    }
    return static_cast<T*>(p);
  }

  void deallocate(T* p, std::size_t) noexcept {
#ifdef DEBUG_ALLOC
    PGPROTO_DEBUG("FREE %lx", (intptr_t)p);
#endif
    pfree(p);
  }

  template <typename U>
  struct rebind {
    typedef PostgresAllocator<U> other;
  };
};

template <class T, class U>
bool operator==(const PostgresAllocator<T>&, const PostgresAllocator<U>&) {
  return true;
}
template <class T, class U>
bool operator!=(const PostgresAllocator<T>&, const PostgresAllocator<U>&) {
  return false;
}

using pstring =
    std::basic_string<char, std::char_traits<char>, PostgresAllocator<char>>;

template <typename T>
using pvector = std::vector<T, PostgresAllocator<T>>;

template <typename T>
struct pfree_deleter {
  void operator()(T* p) { pfree(p); }
};

template <typename T>
using punique_ptr = std::unique_ptr<T, pfree_deleter<T>>;

}  // namespace postgres_utils
}  // namespace postgres_protobuf

// ==============================================================
// ==================== std::hash extensions ====================
// ==============================================================

namespace std {
template <>
struct hash<postgres_protobuf::postgres_utils::pstring> {
  std::size_t operator()(
      postgres_protobuf::postgres_utils::pstring const& s) const noexcept {
    return std::hash<std::string_view>{}(std::string_view(s));
  }
};
}  // namespace std

#endif  // POSTGRES_PROTOBUF_POSTGRES_UTILS_HPP_
