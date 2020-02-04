#ifndef POSTGRES_PROTOBUF_QUERYING_HPP_
#define POSTGRES_PROTOBUF_QUERYING_HPP_

#include "descriptor_db.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace postgres_protobuf {
namespace descriptor_db {
class DescDb;
}

namespace querying {

// TODO: query cache

class BadQuery {
 public:
  BadQuery(std::string&& msg) : msg(msg) {}
  const std::string msg;
};

class RecursionDepthExceeded {};

class QueryImpl;

class Query {
 public:
  Query(const std::string& query, std::optional<uint64_t> limit);
  Query(const Query&) = delete;
  void operator=(const Query&) = delete;

  ~Query();

  std::vector<std::string> Run(const std::uint8_t* proto_data,
                               size_t proto_len);

 private:
  QueryImpl* impl_;
};

}  // namespace querying
}  // namespace postgres_protobuf

#endif  // POSTGRES_PROTOBUF_QUERYING_HPP_
