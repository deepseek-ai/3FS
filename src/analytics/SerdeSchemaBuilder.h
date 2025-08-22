#pragma once

#include <folly/logging/xlog.h>
#include <numeric>
#include <parquet/exception.h>
#include <parquet/schema.h>

#include "SerdeStructVisitor.h"
#include "common/serde/Serde.h"
#include "common/utils/Result.h"
#include "common/utils/TypeTraits.h"

namespace hf3fs::analytics {

template <serde::SerdeType SerdeType>
class SerdeSchemaBuilder : public BaseStructVisitor<SerdeSchemaBuilder<SerdeType>> {
 public:
  std::shared_ptr<parquet::schema::GroupNode> getSchema() {
    try {
      fields_.clear();
      fieldNameParts_.clear();
      this->visit<SerdeType>("");
      return std::static_pointer_cast<parquet::schema::GroupNode>(
          parquet::schema::GroupNode::Make("schema", parquet::Repetition::REQUIRED, fields_));
    } catch (const parquet::ParquetException &ex) {
      XLOGF(CRITICAL, "Failed to build schema of type {}, error: {}", nameof::nameof_full_type<SerdeType>(), ex.what());
      return nullptr;
    }
  }

 public:
  // default
  template <typename T>
  void visit(std::string_view k) = delete;

  template <>
  void visit<bool>(std::string_view k) {
    XLOGF(DBG3, "bool visit({}), fullname: '{}'", k, getFieldFullName(k));
    fields_.push_back(parquet::schema::PrimitiveNode::Make(getFieldFullName(k),
                                                           parquet::Repetition::REQUIRED,
                                                           parquet::LogicalType::None(),
                                                           parquet::Type::BOOLEAN));
  }

  template <>
  void visit<int16_t>(std::string_view k) {
    XLOGF(DBG3, "int16_t visit({}), fullname: '{}'", k, getFieldFullName(k));
    fields_.push_back(parquet::schema::PrimitiveNode::Make(getFieldFullName(k),
                                                           parquet::Repetition::REQUIRED,
                                                           parquet::LogicalType::Int(16, true),
                                                           parquet::Type::INT32));
  }

  template <>
  void visit<uint16_t>(std::string_view k) {
    XLOGF(DBG3, "uint16_t visit({}), fullname: '{}'", k, getFieldFullName(k));
    fields_.push_back(parquet::schema::PrimitiveNode::Make(getFieldFullName(k),
                                                           parquet::Repetition::REQUIRED,
                                                           parquet::LogicalType::Int(16, false),
                                                           parquet::Type::INT32));
  }

  template <>
  void visit<int32_t>(std::string_view k) {
    XLOGF(DBG3, "int32_t visit({}), fullname: '{}'", k, getFieldFullName(k));
    fields_.push_back(parquet::schema::PrimitiveNode::Make(getFieldFullName(k),
                                                           parquet::Repetition::REQUIRED,
                                                           parquet::LogicalType::Int(32, true),
                                                           parquet::Type::INT32));
  }

  template <>
  void visit<uint32_t>(std::string_view k) {
    XLOGF(DBG3, "uint32_t visit({}), fullname: '{}'", k, getFieldFullName(k));
    fields_.push_back(parquet::schema::PrimitiveNode::Make(getFieldFullName(k),
                                                           parquet::Repetition::REQUIRED,
                                                           parquet::LogicalType::Int(32, false),
                                                           parquet::Type::INT32));
  }

  template <>
  void visit<int64_t>(std::string_view k) {
    XLOGF(DBG3, "int64_t visit({}), fullname: '{}'", k, getFieldFullName(k));
    fields_.push_back(parquet::schema::PrimitiveNode::Make(getFieldFullName(k),
                                                           parquet::Repetition::REQUIRED,
                                                           parquet::LogicalType::Int(64, true),
                                                           parquet::Type::INT64));
  }

  template <>
  void visit<uint64_t>(std::string_view k) {
    XLOGF(DBG3, "uint64_t visit({}), fullname: '{}'", k, getFieldFullName(k));
    fields_.push_back(parquet::schema::PrimitiveNode::Make(getFieldFullName(k),
                                                           parquet::Repetition::REQUIRED,
                                                           parquet::LogicalType::Int(64, false),
                                                           parquet::Type::INT64));
  }

  template <typename T>
  requires std::is_enum_v<T>
  void visit(std::string_view k) {
    XLOGF(DBG3, "enum visit({}), fullname: '{}'", k, getFieldFullName(k));
    fields_.push_back(parquet::schema::PrimitiveNode::Make(getFieldFullName(k),
                                                           parquet::Repetition::REQUIRED,
                                                           parquet::LogicalType::Int(32, true),
                                                           parquet::Type::INT32));
  }

  template <serde::ConvertibleToString T>
  void visit(std::string_view k) {
    XLOGF(DBG3, "string visit({}), fullname: '{}'", k, getFieldFullName(k));
    fields_.push_back(parquet::schema::PrimitiveNode::Make(getFieldFullName(k),
                                                           parquet::Repetition::REQUIRED,
                                                           parquet::LogicalType::String(),
                                                           parquet::Type::BYTE_ARRAY));
  }

  template <StrongTyped T>
  void visit(std::string_view k) {
    XLOGF(DBG3, "strongtyped visit({}), fullname: '{}'", k, getFieldFullName(k));
    BaseStructVisitor<SerdeSchemaBuilder>::template visit<T>(k);
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

  template <serde::SerdeTypeWithoutSpecializedSerdeMethod T>
  void visit(std::string_view k) {
    XLOGF(DBG3, "serdetype visit({}), fullname: '{}'", k, getFieldFullName(k));
    if (!k.empty()) fieldNameParts_.push_back(filterOutInvalidChars(k));
    BaseStructVisitor<SerdeSchemaBuilder>::template visit<T>(k);
    if (!k.empty()) fieldNameParts_.pop_back();
  }

  template <typename T>
  requires is_specialization_of_v<T, folly::Expected>
  void visit(std::string_view k) {
    XLOGF(DBG3, "result visit({}), fullname: '{}'", k, getFieldFullName(k));
    std::string errorColumnName = std::string{k} + kResultErrorTypeColumnSuffix;
    visit<typename T::error_type>(errorColumnName);
    visit<typename T::value_type>(k);
  }

  template <typename T>
  requires is_variant_v<T>
  void visit(std::string_view k) {
    XLOGF(DBG3, "variant visit({}), fullname: '{}'", k, getFieldFullName(k));
    std::string valIdxColumnName = std::string{k} + kVariantValueIndexColumnSuffix;
    visit<uint32_t>(valIdxColumnName);
    BaseStructVisitor<SerdeSchemaBuilder>::template visit<T>(k);
  }

  template <typename T>
  requires is_vector_v<T> || is_set_v<T>
  void visit(std::string_view k) {
    XLOGF(DBG3, "container visit({}), fullname: '{}'", k, getFieldFullName(k));
    fields_.push_back(parquet::schema::PrimitiveNode::Make(getFieldFullName(k),
                                                           parquet::Repetition::REQUIRED,
                                                           parquet::LogicalType::String(),
                                                           parquet::Type::BYTE_ARRAY));
  }

  template <typename T>
  requires is_optional_v<T>
  void visit(std::string_view k) {
    XLOGF(DBG3, "container visit({}), fullname: '{}'", k, getFieldFullName(k));
    fields_.push_back(parquet::schema::PrimitiveNode::Make(getFieldFullName(k),
                                                           parquet::Repetition::REQUIRED,
                                                           parquet::LogicalType::String(),
                                                           parquet::Type::BYTE_ARRAY));
  }

 private:
  std::string getFieldFullName(std::string_view k) {
    fieldNameParts_.push_back(filterOutInvalidChars(k));
    auto fieldFullName = std::accumulate(fieldNameParts_.begin(),
                                         fieldNameParts_.end(),
                                         std::string{},
                                         [](const std::string &a, const std::string &b) {
                                           return a + (a.empty() ? std::string{} : std::string("_")) + b;
                                         });
    fieldNameParts_.pop_back();
    return fieldFullName;
  }

  std::string filterOutInvalidChars(std::string_view k) {
    return std::accumulate(k.begin(), k.end(), std::string{}, [](const std::string &a, const char b) {
      if (('a' <= b && b <= 'z') || ('A' <= b && b <= 'Z') || ('0' <= b && b <= '9')) return a + b;
      return a;
    });
  };

 private:
  parquet::schema::NodeVector fields_;
  std::vector<std::string> fieldNameParts_;
};

}  // namespace hf3fs::analytics
