#pragma once
#include <arrow/io/file.h>
#include <parquet/stream_writer.h>
#include <type_traits>
#include <utility>
#include <variant>

#include "SerdeObjectVisitor.h"
#include "SerdeSchemaBuilder.h"
#include "common/serde/Serde.h"
#include "common/utils/Nameof.hpp"
#include "common/utils/StrongType.h"
#include "common/utils/UtcTime.h"

namespace hf3fs::analytics {

template <serde::SerdeType SerdeType>
class SerdeObjectWriter : public BaseObjectVisitor<SerdeObjectWriter<SerdeType>> {
 public:
  SerdeObjectWriter(parquet::StreamWriter &&writer)
      : writer_(std::move(writer)),
        createTime_(UtcClock::now()) {}

  static std::shared_ptr<SerdeObjectWriter> open(const Path path,
                                                 const bool append = false,
                                                 const size_t maxRowGroupLength = 1'000'000,
                                                 const std::vector<parquet::SortingColumn> &sortedColumns = {}) {
    // open file
    auto openStream = arrow::io::FileOutputStream::Open(path.string(), append);

    if (!openStream.ok()) {
      XLOGF(ERR, "Failed to open file output stream: {}, error: {}", path.string(), openStream.status().message());
      return nullptr;
    }

    std::shared_ptr<arrow::io::FileOutputStream> outfile;
    PARQUET_ASSIGN_OR_THROW(outfile, openStream);

    // generate schema
    SerdeSchemaBuilder<SerdeType> schemaBuilder;
    auto schemaNode = schemaBuilder.getSchema();

    if (schemaNode == nullptr) {
      return nullptr;
    }

    parquet::WriterProperties::Builder writerBuilder;
    writerBuilder.set_sorting_columns(sortedColumns);
    writerBuilder.max_row_group_length(maxRowGroupLength);
    writerBuilder.data_page_version(parquet::ParquetDataPageVersion::V2);

    // set global encoding and compression method
    // writerBuilder.encoding(parquet::Encoding::DELTA_BINARY_PACKED);
    writerBuilder.compression(parquet::Compression::ZSTD);

    // set encoding for string columns
    // for (int fieldIndex = 0; fieldIndex < schemaNode->field_count(); fieldIndex++) {
    //   auto fieldNode = schemaNode->field(fieldIndex);
    //   auto fieldType = fieldNode->logical_type();
    //   if (fieldType->is_string()) writerBuilder.encoding(fieldNode->name(),
    //   parquet::Encoding::DELTA_LENGTH_BYTE_ARRAY);
    // }

    try {
      auto fileWriter = parquet::ParquetFileWriter::Open(outfile, schemaNode, writerBuilder.build());
      if (fileWriter == nullptr) {
        XLOGF(ERR, "Failed to open file writer: {}", path.string());
        return nullptr;
      }

      parquet::StreamWriter streamWriter(std::move(fileWriter));
      return std::make_shared<SerdeObjectWriter>(std::move(streamWriter));

    } catch (const std::exception &ex) {
      XLOGF(ERR, "Failed to create stream writer: {}, error: {}", path.string(), ex.what());
      return nullptr;
    }
  }

  SerdeObjectWriter &operator<<(const SerdeType &v) {
    if (!bool(*this)) return *this;

    try {
      visit("", v);
      writer_ << parquet::EndRow;
    } catch (const parquet::ParquetException &ex) {
      XLOGF(CRITICAL, "Failed to write to parquet file, error: {}", ex.what());
      isOk_ = false;
    }

    return *this;
  }

  void endRowGroup() { writer_.EndRowGroup(); }

  UtcTime createTime() { return createTime_; }

  operator bool() const { return ok(); }

  bool ok() const { return isOk_; }

 public:
  // default
  template <typename T>
  void visit(std::string_view k, const T &v) = delete;

  template <typename T>
  requires std::is_arithmetic_v<T>
  void visit(std::string_view k, const T &v) {
    XLOGF(DBG3, "arithmetic visit({})", k);
    writer_ << v;
  }

  template <typename T>
  requires std::is_enum_v<T>
  void visit(std::string_view k, const T &v) {
    XLOGF(DBG3, "enum visit({})", k);
    writer_ << (int32_t)v;
  }

  template <serde::ConvertibleToString T>
  void visit(std::string_view k, const T &v) {
    XLOGF(DBG3, "string visit({})", k);
    writer_ << v;
  }

  template <StrongTyped T>
  void visit(std::string_view k, const T &v) {
    XLOGF(DBG3, "strongtyped visit({})", k);
    visit<typename T::UnderlyingType>(k, v.toUnderType());
  }

  template <serde::WithReadableSerdeMethod T>
  void visit(std::string_view k, const T &val) {
    auto serialized = serde::SerdeMethod<T>::serdeToReadable(val);
    XLOGF(DBG3,
          "WithReadableSerdeMethod visit({}), serialized: {} {}",
          k,
          nameof::nameof_type<decltype(serialized)>(),
          serialized);
    visit<serde::SerdeToReadableReturnType<T>>(k, serialized);
  }

  template <serde::WithSerdeMethod T>
  void visit(std::string_view k, const T &val) {
    auto serialized = serde::SerdeMethod<T>::serdeTo(val);
    XLOGF(DBG3,
          "WithSerdeMethod visit({}), serialized: {} {}",
          k,
          nameof::nameof_type<decltype(serialized)>(),
          serialized);
    visit(k, serialized);
  }

  template <serde::WithReadableSerdeMemberMethod T>
  void visit(std::string_view k, const T &v) {
    auto serialized = v.serdeToReadable();
    XLOGF(DBG3,
          "WithReadableSerdeMemberMethod visit({}), serialized: {} {}",
          k,
          nameof::nameof_type<decltype(serialized)>(),
          serialized);
    visit(k, serialized);
  }

  template <serde::WithSerdeMemberMethod T>
  void visit(std::string_view k, const T &v) {
    auto serialized = v.serdeTo();
    XLOGF(DBG3,
          "WithSerdeMemberMethod visit({}), serialized: {} {}",
          k,
          nameof::nameof_type<decltype(serialized)>(),
          serialized);
    visit(k, serialized);
  }

  template <serde::SerdeTypeWithoutSpecializedSerdeMethod T>
  void visit(std::string_view k, const T &val) {
    XLOGF(DBG3, "serdetype visit({})", k);
    BaseObjectVisitor<SerdeObjectWriter>::visit(k, const_cast<T &>(val));
  }

  template <typename T>
  requires is_specialization_of_v<T, folly::Expected>
  void visit(std::string_view k, const T &val) {
    XLOGF(DBG3, "result visit({})", k);
    std::string errorColumnName = std::string{k} + kResultErrorTypeColumnSuffix;

    if (val.hasValue()) {
      Status ok(StatusCode::kOK);
      visit<typename T::error_type>(errorColumnName, ok);
      visit<typename T::value_type>(k, val.value());
    } else {
      typename T::value_type value{};
      visit<typename T::error_type>(errorColumnName, val.error());
      visit<typename T::value_type>(k, value);
    }
  }

  template <typename T>
  requires is_variant_v<T>
  void visit(std::string_view k, const T &val) {
    XLOGF(DBG3, "variant visit({})", k);
    std::string valIdxColumnName = std::string{k} + kVariantValueIndexColumnSuffix;
    visit<uint32_t>(valIdxColumnName, val.index());
    BaseObjectVisitor<SerdeObjectWriter>::visit(k, const_cast<T &>(val));
  }

  template <typename T>
  requires is_vector_v<T> || is_set_v<T>
  void visit(std::string_view k, const T &val) {
    XLOGF(DBG3, "container visit({})", k);
    auto str = serde::toJsonString(val);
    writer_ << str;
  }

  template <typename T>
  requires is_optional_v<T>
  void visit(std::string_view k, const T &val) {
    XLOGF(DBG3, "optional visit({})", k);
    if (!val.has_value()) {
      writer_ << "";
    } else {
      auto str = serde::toJsonString(*val);
      writer_ << str;
    }
  }

 private:
  parquet::StreamWriter writer_;
  UtcTime createTime_;
  bool isOk_{true};
};

template <typename T>
SerdeObjectWriter<T> &operator<<(SerdeObjectWriter<T> &writer, parquet::EndRowGroupType) {
  writer.endRowGroup();
  return writer;
}

}  // namespace hf3fs::analytics
