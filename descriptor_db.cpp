#include "descriptor_db.hpp"

#include "postgres_protobuf_common.hpp"
#include "postgres_utils.hpp"

#include <cstring>

#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/util/type_resolver_util.h>

extern "C" {
// Must be included before other Postgres headers
#include <postgres.h>

#include <executor/spi.h>
#include <funcapi.h>
}

using namespace postgres_protobuf::postgres_utils;

namespace postgres_protobuf {
namespace descriptor_db {

const std::shared_ptr<DescDb>& DescDb::GetOrCreateCached() {
  if (cached_ != nullptr) {
    return cached_;
  }

  MemoryContext outer_mctx = CurrentMemoryContext;

  if (SPI_connect() != SPI_OK_CONNECT) {
    ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR), errmsg("SPI_connect failed")));
  }

  const char* sql =
      "SELECT name, file_descriptor_set "
      "FROM protobuf_file_descriptor_sets";
  int status = SPI_execute(sql, true, 0);
  if (status != SPI_OK_SELECT) {
    ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("SPI_execute failed: %s", SPI_result_code_string(status))));
  }

  // Read all rows before allocating anything on the C++ heap.
  // We do this because there may be a Postgres error while reading rows.
  // We allocate the rows in the memory context of the caller, which outlasts
  // `SPI_finish`. (We do this after `SPI_execute` because `SPI_execute` would
  // switch the context back. `SPI_getvalue` and `SPI_getbinval` don't do that)
  MemoryContextSwitchTo(outer_mctx);
  pvector<std::tuple<pstring, pstring>> rows;
  rows.reserve(SPI_processed);
  TupleDesc tupdesc = SPI_tuptable->tupdesc;
  for (uint64 i = 0; i < SPI_processed; ++i) {
    HeapTuple row = SPI_tuptable->vals[i];

    char* name_cstr = SPI_getvalue(row, tupdesc, 1);
    pstring name(name_cstr);
    pfree(name_cstr);
    name_cstr = nullptr;

    bool isnull;
    bytea* fds_binary = (bytea*)PG_DETOAST_DATUM_PACKED(
        SPI_getbinval(row, tupdesc, 2, &isnull));
    if (isnull) {
      ereport(WARNING,
              (errcode(ERRCODE_INTERNAL_ERROR),
               errmsg("Didn't expect postgres_protobuf_file_descriptor_sets to "
                      "contain nulls")));
      continue;
    }

    pstring fds =
        pstring(VARDATA_ANY(fds_binary), VARSIZE_ANY_EXHDR(fds_binary));

    rows.emplace_back(std::move(name), std::move(fds));
  }

  if (SPI_finish() != SPI_OK_FINISH) {
    ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR), errmsg("SPI_finish failed")));
  }

  // We start creating RAII objects that allocate on the C++ heap now.
  // No more Postgres operations, which may throw Postgres exceptions,
  // are allowed in this block.
  {
    std::unordered_map<std::string, std::unique_ptr<DescSet>> desc_sets;
    for (const auto& row : rows) {
      pb::FileDescriptorSet fds;
      const pstring& name = std::get<0>(row);
      const pstring& fds_data = std::get<1>(row);

      if (!fds.ParseFromArray(fds_data.data(), fds_data.size())) {
        throw BadProto("failed to parse FileDescriptorSet");
      }

      std::unique_ptr<DescSet>& desc_set = desc_sets[std::string(name)];
      if (desc_set == nullptr) {
        desc_set = std::make_unique<DescSet>();
      }
      while (fds.file_size() > 0) {
        auto* fd = fds.mutable_file()->ReleaseLast();
        desc_set->desc_db->AddAndOwn(fd);
      }
    }

    cached_ = std::shared_ptr<DescDb>(new DescDb(std::move(desc_sets)));
  }

  // TODO: can we safely make the cache outlive the transaction?
  // We'd need an efficient way to check whether the source table
  // has changed.
  // These posts suggest checking `xmin` and `ctid` on all rows:
  // https://www.postgresql.org/message-id/24054.1077121761@sss.pgh.pa.us
  // https://www.postgresql.org/message-id/4599.1171052901@sss.pgh.pa.us
  MemoryContextCallback* callback =
      static_cast<MemoryContextCallback*>(MemoryContextAllocExtended(
          CurTransactionContext, sizeof(MemoryContextCallback),
          MCXT_ALLOC_ZERO | MCXT_ALLOC_NO_OOM));
  if (callback == nullptr) {
    throw std::bad_alloc();
  }
  memset(callback, 0, sizeof(*callback));
  callback->func = &DescDb::ClearCacheCallback;
  callback->arg = nullptr;
  MemoryContextRegisterResetCallback(CurTransactionContext, callback);

  PGPROTO_DEBUG("DescDb cache rebuilt");
  return cached_;
}

void DescDb::ClearCache() {
  cached_.reset();
}

DescDb::DescDb(
    std::unordered_map<std::string, std::unique_ptr<DescSet>> desc_sets)
    : desc_sets(std::move(desc_sets)) {}

std::shared_ptr<DescDb> DescDb::cached_;

void DescDb::ClearCacheCallback(void*) { ClearCache(); }

DescSet::DescSet()
    : desc_db(std::make_unique<pb::SimpleDescriptorDatabase>()),
      pool(std::make_unique<pb::DescriptorPool>(desc_db.get())),
      type_resolver(pb::util::NewTypeResolverForDescriptorPool(
          "type.googleapis.com", pool.get())) {}

}  // namespace descriptor_db
}  // namespace postgres_protobuf
