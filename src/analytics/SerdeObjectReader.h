#pragma once
#include <arrow/io/file.h>
#include <optional>
#include <parquet/stream_reader.h>
#include <type_traits>
#include <utility>
#include <variant>

#include "SerdeObjectVisitor.h"
#include "SerdeSchemaBuilder.h"
#include "common/serde/Serde.h"
#include "common/utils/Nameof.hpp"
#include "common/utils/StrongType.h"
#include "common/utils/TypeTraits.h"

namespace hf3fs::analytics {

template <serde::SerdeType SerdeType>
class SerdeObjectReader : public BaseObjectVisitor<SerdeObjectReader<SerdeType>> {
 public:
  SerdeObjectReader(parquet::StreamReader &&reader)
      : reader_(std::move(reader)) {}

  static std::shared_ptr<SerdeObjectReader> open(const Path path) {
    // open file
    auto openStream = arrow::io::ReadableFile::Open(path.string());

    if (!openStream.ok()) {
      XLOGF(ERR, "Failed to open file input stream: {}, error: {}", path.string(), openStream.status().message());
      return nullptr;
    }

    std::shared_ptr<arrow::io::ReadableFile> infile;
    PARQUET_ASSIGN_OR_THROW(infile, openStream);

    try {
      parquet::StreamReader streamReader{parquet::ParquetFileReader::Open(infile)};
      return std::make_shared<SerdeObjectReader>(std::move(streamReader));
    } catch (const std::exception &ex) {
      XLOGF(ERR, "Failed to create stream reader: {}, error: {}", path.string(), ex.what());
      return nullptr;
    }
  }

  SerdeObjectReader &operator>>(SerdeType &v) {
    eof_ = eof();
    if (!bool(*this)) return *this;

    try {
      visit("", v);
      reader_ >> parquet::EndRow;
    } catch (const parquet::ParquetException &ex) {
      XLOGF(CRITICAL, "Failed to read from parquet file, error: {}", ex.what());
      isOk_ = false;
    }

    return *this;
  }

  operator bool() const { return ok() && !eof_; }

  bool ok() const { return isOk_; }

  bool eof() const { return reader_.eof(); }

  size_t numRows() const { return reader_.num_rows(); }

 public:
  // default
  template <typename T>
  void visit(std::string_view k, T &v) = delete;

  template <typename T>
  requires std::is_arithmetic_v<T>
  void visit(std::string_view k, T &v) {
    reader_ >> v;
    XLOGF(DBG3, "arithmetic visit({}): {}", k, v);
  }

  template <typename T>
  requires std::is_enum_v<T>
  void visit(std::string_view k, T &v) {
    int32_t n;
    reader_ >> n;
    XLOGF(DBG3, "enum visit({}): {}", k, n);
    auto result = magic_enum::enum_cast<T>(n);
    if (result) {
      v = *result;
    } else {
      XLOGF(CRITICAL, "Failed to parse enum {} from value: {}", nameof::nameof_short_type<T>(), n);
    }
  }

  template <serde::ConvertibleToString T>
  void visit(std::string_view k, T &v) {
    reader_ >> v;
    XLOGF(DBG3, "string visit({}): {}", k, v);
  }

  template <StrongTyped T>
  void visit(std::string_view k, T &v) {
    XLOGF(DBG3, "strongtyped visit({})", k);
    BaseObjectVisitor<SerdeObjectReader>::visit(k, v);
  }

  template <serde::WithReadableSerdeMethod T>
  void visit(std::string_view k, T &val) {
    XLOGF(DBG3, "WithReadableSerdeMethod visit({})", k);
    typename serde::SerdeToReadableReturnType<T> serialized;
    visit(k, serialized);
    auto result = serde::SerdeMethod<T>::serdeFromReadable(serialized);
    if (result) {
      val = *result;
    } else {
      XLOGF(CRITICAL, "Failed to parse {} from value: {}", nameof::nameof_short_type<T>(), serialized);
    }
  }

  template <serde::WithSerdeMethod T>
  void visit(std::string_view k, T &val) {
    XLOGF(DBG3, "WithSerdeMethod visit({})", k);
    typename serde::SerdeToReturnType<T> serialized;
    visit(k, serialized);
    auto result = serde::SerdeMethod<T>::serdeFrom(serialized);
    if (result) {
      val = *result;
    } else {
      XLOGF(CRITICAL, "Failed to parse {} from value: {}", nameof::nameof_short_type<T>(), serialized);
    }
  }

  template <serde::WithReadableSerdeMemberMethod T>
  void visit(std::string_view k, T &val) {
    XLOGF(DBG3, "WithReadableSerdeMemberMethod visit({})", k);
    serde::SerdeToReadableMemberMethodReturnType<T> serialized;
    visit(k, serialized);
    auto result = T::serdeFromReadable(serialized);
    if (result) {
      val = *result;
    } else {
      XLOGF(CRITICAL, "Failed to parse {} from value: {}", nameof::nameof_short_type<T>(), serialized);
    }
  }

  template <serde::WithSerdeMemberMethod T>
  void visit(std::string_view k, T &val) {
    XLOGF(DBG3, "WithSerdeMemberMethod visit({})", k);
    serde::SerdeToReadableMemberMethodReturnType<T> serialized;
    visit(k, serialized);
    auto result = T::serdeFromReadable(serialized);
    if (result) {
      val = *result;
    } else {
      XLOGF(CRITICAL, "Failed to parse {} from value: {}", nameof::nameof_short_type<T>(), serialized);
    }
  }

  template <serde::SerdeTypeWithoutSpecializedSerdeMethod T>
  void visit(std::string_view k, T &val) {
    XLOGF(DBG3, "serdetype visit({})", k);
    BaseObjectVisitor<SerdeObjectReader>::visit(k, val);
  }

  template <typename T>
  requires is_specialization_of_v<T, folly::Expected>
  void visit(std::string_view k, T &val) {
    XLOGF(DBG3, "result visit({})", k);
    std::string errorColumnName = std::string{k} + kResultErrorTypeColumnSuffix;

    Status status(StatusCode::kOK);
    typename T::value_type value;
    visit<typename T::error_type>(errorColumnName, status);
    visit<typename T::value_type>(k, value);

    if (status.isOK()) {
      val = std::move(value);
    } else {
      val = makeError(status);
    }
  }

  template <typename T>
  requires is_variant_v<T>
  void visit(std::string_view k, T &val) {
    XLOGF(DBG3, "variant visit({})", k);
    // get the index of value
    std::string valIdxColumnName = std::string{k} + kVariantValueIndexColumnSuffix;
    uint32_t valIdx = 0;
    visit<uint32_t>(valIdxColumnName, valIdx);

    // read and set the value
    uint32_t altIdx = 0;
    visitVariant(val, [&](std::string_view typeName, auto &&v) {
      std::string altTypeName = std::string{k} + std::string{typeName};
      std::remove_reference_t<decltype(v)> alt;
      visit(altTypeName, alt);
      if (altIdx == valIdx) val = std::move(alt);
      altIdx++;
    });
  }

  template <typename T>
  requires is_vector_v<T> || is_set_v<T>
  void visit(std::string_view k, T &val) {
    std::string str;
    reader_ >> str;
    XLOGF(DBG3, "container visit({}): {}", k, str);
    auto result = serde::fromJsonString(val, str);
    if (!result) {
      XLOGF(CRITICAL, "Failed to parse {} from json string: {}", nameof::nameof_short_type<T>(), str);
    }
  }

  template <typename T>
  requires is_optional_v<T>
  void visit(std::string_view k, T &val) {
    std::string str;
    reader_ >> str;
    XLOGF(DBG3, "container visit({}): {}", k, str);
    if (str.empty()) {
      val = std::nullopt;
    } else {
      using ValueType = typename T::value_type;
      val = ValueType();
      auto result = serde::fromJsonString(*val, str);
      if (!result) {
        XLOGF(CRITICAL, "Failed to parse {} from json string: {}", nameof::nameof_short_type<ValueType>(), str);
      }
    }
  }

 private:
  parquet::StreamReader reader_;
  bool isOk_{true};
  bool eof_{false};
};

}  // namespace hf3fs::analytics
