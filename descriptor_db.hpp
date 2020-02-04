#ifndef POSTGRES_PROTOBUF_DESCRIPTOR_DB_HPP_
#define POSTGRES_PROTOBUF_DESCRIPTOR_DB_HPP_

#include <memory>
#include <unordered_map>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor_database.h>
#include <google/protobuf/util/type_resolver.h>

namespace postgres_protobuf {
namespace descriptor_db {

namespace pb = ::google::protobuf;

struct DescDb;
struct DescSet;

// NOTE: This currently lives outside of Postgres's memory management,
// because the protobuf library doesn't support alternative allocators.
struct DescDb {
  // TODO: load DescSets lazily
  const std::unordered_map<std::string, std::unique_ptr<DescSet>> desc_sets;

  static const std::shared_ptr<DescDb>& GetOrCreateCached();
  static void ClearCache();

 private:
  DescDb(std::unordered_map<std::string, std::unique_ptr<DescSet>> desc_sets);

  static std::shared_ptr<DescDb> cached_;

  static uint64_t GetTableXmin();
  static void ClearCacheCallback(void*);
};

struct DescSet {
  std::unique_ptr<pb::SimpleDescriptorDatabase> desc_db;
  std::unique_ptr<pb::DescriptorPool> pool;
  std::unique_ptr<pb::util::TypeResolver> type_resolver;

  DescSet();
};

}  // namespace descriptor_db
}  // namespace postgres_protobuf

#endif  // POSTGRES_PROTOBUF_DESCRIPTOR_DB_HPP_
