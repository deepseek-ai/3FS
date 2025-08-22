#include <folly/logging/xlog.h>
#include <parquet/schema.h>
#include <string_view>

#include "analytics/SerdeObjectVisitor.h"
#include "fbs/meta/Schema.h"
#include "fbs/storage/Common.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::analytics {

class DebugObjectVisitor : public BaseObjectVisitor<DebugObjectVisitor> {
 public:
  template <typename T>
  void visit(std::string_view k, T &) = delete;

  template <typename T>
  requires std::is_arithmetic_v<T>
  void visit(std::string_view k, T &) { XLOGF(DBG3, "arithmetic visit({})", k); }

  template <typename T>
  requires std::is_enum_v<T>
  void visit(std::string_view k, T &) { XLOGF(DBG3, "enum visit({})", k); }

  template <typename T>
  requires std::is_convertible_v<T, std::string_view>
  void visit(std::string_view k, T &) { XLOGF(DBG3, "string visit({})", k); }

  template <StrongTyped T>
  void visit(std::string_view k, T &v) {
    XLOGF(DBG3, "strongtyped visit({})", k);
    visit<typename T::UnderlyingType>(k, v.toUnderType());
  }

  template <serde::WithReadableSerdeMethod T>
  void visit(std::string_view k, T &v) {
    XLOGF(DBG3, "WithReadableSerdeMethod visit({})", k);
    auto serialized = serde::SerdeMethod<T>::serdeToReadable(v);
    visit(k, serialized);
  }

  template <serde::WithSerdeMethod T>
  void visit(std::string_view k, T &v) {
    XLOGF(DBG3, "WithSerdeMethod visit({})", k);
    auto serialized = serde::SerdeMethod<T>::serdeTo(v);
    visit(k, serialized);
  }

  template <serde::WithReadableSerdeMemberMethod T>
  void visit(std::string_view k, T &v) {
    XLOGF(DBG3, "WithReadableSerdeMemberMethod visit({})", k);
    auto serialized = v.serdeToReadable();
    visit(k, serialized);
  }

  template <serde::WithSerdeMemberMethod T>
  void visit(std::string_view k, T &v) {
    XLOGF(DBG3, "WithSerdeMemberMethod visit({})", k);
    auto serialized = v.serdeTo();
    visit(k, serialized);
  }

  template <serde::SerdeTypeWithoutSpecializedSerdeMethod T>
  void visit(std::string_view k, T &val) {
    XLOGF(DBG3, "serdetype visit({})", k);
    BaseObjectVisitor<DebugObjectVisitor>::visit(k, val);
  }

  template <typename T>
  requires is_variant_v<T>
  void visit(std::string_view k, T &val) {
    XLOGF(DBG3, "variant visit({})", k);
    BaseObjectVisitor<DebugObjectVisitor>::visit(k, val);
  }

  template <typename T>
  requires is_vector_v<T> || is_set_v<T>
  void visit(std::string_view k, T &val) {
    XLOGF(DBG3, "container visit({})", k);
    BaseObjectVisitor<DebugObjectVisitor>::visit(k, val);
  }

  template <typename T>
  requires is_optional_v<T>
  void visit(std::string_view k, T &val) {
    XLOGF(DBG3, "container visit({})", k);
    BaseObjectVisitor<DebugObjectVisitor>::visit(k, val);
  }
};

TEST(TestSerdeObjectVisitor, ChunkId) {
  storage::ChunkId chunkId;
  DebugObjectVisitor visitor;
  visitor.visit("chunkId", chunkId);
}

TEST(TestSerdeObjectVisitor, ChunkMeta) {
  storage::ChunkMeta chunkmeta;
  DebugObjectVisitor visitor;
  visitor.visit("chunkmeta", chunkmeta);
}

TEST(TestSerdeObjectVisitor, InodeId) {
  meta::InodeId inodeId;
  DebugObjectVisitor visitor;
  visitor.visit("inodeId", inodeId);
}

TEST(TestSerdeObjectVisitor, Inode) {
  DebugObjectVisitor visitor;
  meta::Inode inode;
  visitor.visit("inode", inode);
}

}  // namespace hf3fs::analytics
