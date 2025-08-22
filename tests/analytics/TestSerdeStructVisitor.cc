#include <folly/logging/xlog.h>
#include <parquet/schema.h>

#include "analytics/SerdeStructVisitor.h"
#include "fbs/meta/Schema.h"
#include "fbs/storage/Common.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::analytics {

class DebugStructVisitor : public BaseStructVisitor<DebugStructVisitor> {
 public:
  // default
  template <typename T>
  void visit(std::string_view k) = delete;

  template <>
  void visit<uint16_t>(std::string_view k) {
    XLOGF(DBG3, "uint16_t visit({})", k);
  }

  template <>
  void visit<uint32_t>(std::string_view k) {
    XLOGF(DBG3, "uint32_t visit({})", k);
  }

  template <>
  void visit<uint64_t>(std::string_view k) {
    XLOGF(DBG3, "uint64_t visit({})", k);
  }

  template <>
  void visit<int16_t>(std::string_view k) {
    XLOGF(DBG3, "int16_t visit({})", k);
  }

  template <>
  void visit<int32_t>(std::string_view k) {
    XLOGF(DBG3, "int32_t visit({})", k);
  }

  template <>
  void visit<int64_t>(std::string_view k) {
    XLOGF(DBG3, "int64_t visit({})", k);
  }

  template <typename T>
  requires std::is_enum_v<T>
  void visit(std::string_view k) { XLOGF(DBG3, "enum visit({})", k); }

  template <serde::ConvertibleToString T>
  void visit(std::string_view k) {
    XLOGF(DBG3, "string visit({})", k);
  }

  template <StrongTyped T>
  void visit(std::string_view k) {
    XLOGF(DBG3, "strongtyped visit({})", k);
    BaseStructVisitor<DebugStructVisitor>::visit<T>(k);
  }

  template <serde::SerdeTypeWithoutSpecializedSerdeMethod T>
  void visit(std::string_view k) {
    XLOGF(DBG3, "serdetype visit({})", k);
    BaseStructVisitor<DebugStructVisitor>::visit<T>(k);
  }

  template <serde::WithReadableSerdeMethod T>
  void visit(std::string_view k) {
    XLOGF(DBG3, "WithReadableSerdeMethod visit({})", k);
    visit<serde::SerdeToReadableReturnType<T>>(k);
  }

  template <serde::WithSerdeMethod T>
  void visit(std::string_view k) {
    XLOGF(DBG3, "WithSerdeMethod visit({})", k);
    visit<serde::SerdeToReturnType<T>>(k);
  }

  template <serde::WithReadableSerdeMemberMethod T>
  void visit(std::string_view k) {
    XLOGF(DBG3, "WithReadableSerdeMemberMethod visit({})", k);
    visit<serde::SerdeToReadableMemberMethodReturnType<T>>(k);
  }

  template <serde::WithSerdeMemberMethod T>
  void visit(std::string_view k) {
    XLOGF(DBG3, "WithSerdeMemberMethod visit({})", k);
    visit<serde::SerdeToMemberMethodReturnType<T>>(k);
  }

  template <typename T>
  requires is_variant_v<T>
  void visit(std::string_view k) {
    XLOGF(DBG3, "variant visit({})", k);
    BaseStructVisitor<DebugStructVisitor>::visit<T>(k);
  }

  template <typename T>
  requires is_vector_v<T> || is_set_v<T>
  void visit(std::string_view k) {
    XLOGF(DBG3, "container visit({})", k);
    BaseStructVisitor<DebugStructVisitor>::visit<T>(k);
  }

  template <typename T>
  requires is_optional_v<T>
  void visit(std::string_view k) {
    XLOGF(DBG3, "container visit({})", k);
    BaseStructVisitor<DebugStructVisitor>::visit<T>(k);
  }
};

TEST(TestSerdeStructVisitor, ChunkId) {
  DebugStructVisitor visitor;
  visitor.visit<storage::ChunkId>("chunkId");
}

TEST(TestSerdeStructVisitor, ChunkMeta) {
  DebugStructVisitor visitor;
  visitor.visit<storage::ChunkMeta>("chunkmeta");
}

TEST(TestSerdeStructVisitor, InodeId) {
  DebugStructVisitor visitor;
  visitor.visit<meta::InodeId>("inodeId");
}

TEST(TestSerdeStructVisitor, Inode) {
  DebugStructVisitor visitor;
  visitor.visit<meta::Inode>("inode");
}

TEST(TestSerdeStructVisitor, DirEntry) {
  DebugStructVisitor visitor;
  visitor.visit<meta::DirEntry>("dirEntry");
}

}  // namespace hf3fs::analytics
