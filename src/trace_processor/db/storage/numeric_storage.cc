
/*
 * Copyright (C) 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/trace_processor/db/storage/numeric_storage.h"

#include <cmath>
#include <cstddef>
#include <string>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/public/compiler.h"
#include "protos/perfetto/trace_processor/serialization.pbzero.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/containers/row_map.h"
#include "src/trace_processor/db/storage/storage.h"
#include "src/trace_processor/db/storage/types.h"
#include "src/trace_processor/db/storage/utils.h"
#include "src/trace_processor/tp_metatrace.h"

namespace perfetto {
namespace trace_processor {
namespace storage {
namespace {

using Range = RowMap::Range;
using NumericValue = std::variant<uint32_t, int32_t, int64_t, double_t>;

// Using the fact that binary operators in std are operators() of classes, we
// can wrap those classes in variants and use them for std::visit in
// SerialComparators. This helps prevent excess templating and switches.
template <typename T>
using FilterOpVariant = std::variant<std::greater<T>,
                                     std::greater_equal<T>,
                                     std::less<T>,
                                     std::less_equal<T>,
                                     std::equal_to<T>,
                                     std::not_equal_to<T>>;

// Based on SqlValue and ColumnType, casts SqlValue to proper type. Assumes the
// |val| and |type| are correct.
inline NumericValue GetNumericTypeVariant(ColumnType type, SqlValue val) {
  switch (type) {
    case ColumnType::kDouble:
      return val.AsDouble();
    case ColumnType::kInt64:
      return val.AsLong();
    case ColumnType::kInt32:
      return static_cast<int32_t>(val.AsLong());
    case ColumnType::kUint32:
      return static_cast<uint32_t>(val.AsLong());
    case ColumnType::kString:
    case ColumnType::kDummy:
    case ColumnType::kId:
      PERFETTO_FATAL("Invalid type");
  }
  PERFETTO_FATAL("For GCC");
}

// Fetch std binary comparator class based on FilterOp. Can be used in
// std::visit for comparison.
template <typename T>
inline FilterOpVariant<T> GetFilterOpVariant(FilterOp op) {
  switch (op) {
    case FilterOp::kEq:
      return FilterOpVariant<T>(std::equal_to<T>());
    case FilterOp::kNe:
      return FilterOpVariant<T>(std::not_equal_to<T>());
    case FilterOp::kGe:
      return FilterOpVariant<T>(std::greater_equal<T>());
    case FilterOp::kGt:
      return FilterOpVariant<T>(std::greater<T>());
    case FilterOp::kLe:
      return FilterOpVariant<T>(std::less_equal<T>());
    case FilterOp::kLt:
      return FilterOpVariant<T>(std::less<T>());
    case FilterOp::kGlob:
    case FilterOp::kRegex:
    case FilterOp::kIsNotNull:
    case FilterOp::kIsNull:
      PERFETTO_FATAL("Not a valid operation on numeric type.");
  }
  PERFETTO_FATAL("For GCC");
}

uint32_t LowerBoundIntrinsic(const void* data,
                             NumericValue val,
                             RowMap::Range search_range) {
  return std::visit(
      [data, search_range](auto val_data) {
        using T = decltype(val_data);
        const T* typed_start = static_cast<const T*>(data);
        auto lower = std::lower_bound(typed_start + search_range.start,
                                      typed_start + search_range.end, val_data);
        return static_cast<uint32_t>(std::distance(typed_start, lower));
      },
      val);
}

uint32_t UpperBoundIntrinsic(const void* data,
                             NumericValue val,
                             RowMap::Range search_range) {
  return std::visit(
      [data, search_range](auto val_data) {
        using T = decltype(val_data);
        const T* typed_start = static_cast<const T*>(data);
        auto upper = std::upper_bound(typed_start + search_range.start,
                                      typed_start + search_range.end, val_data);
        return static_cast<uint32_t>(std::distance(typed_start, upper));
      },
      val);
}

uint32_t LowerBoundExtrinsic(const void* data,
                             NumericValue val,
                             uint32_t* indices,
                             uint32_t indices_count) {
  return std::visit(
      [data, indices, indices_count](auto val_data) {
        using T = decltype(val_data);
        const T* typed_start = static_cast<const T*>(data);
        auto lower =
            std::lower_bound(indices, indices + indices_count, val_data,
                             [typed_start](uint32_t index, T val) {
                               return typed_start[index] < val;
                             });
        return static_cast<uint32_t>(std::distance(indices, lower));
      },
      val);
}

uint32_t UpperBoundExtrinsic(const void* data,
                             NumericValue val,
                             uint32_t* indices,
                             uint32_t indices_count) {
  return std::visit(
      [data, indices, indices_count](auto val_data) {
        using T = decltype(val_data);
        const T* typed_start = static_cast<const T*>(data);
        auto upper =
            std::upper_bound(indices, indices + indices_count, val_data,
                             [typed_start](T val, uint32_t index) {
                               return val < typed_start[index];
                             });
        return static_cast<uint32_t>(std::distance(indices, upper));
      },
      val);
}

template <typename T>
void TypedLinearSearch(T typed_val,
                       const T* start,
                       FilterOp op,
                       BitVector::Builder& builder) {
  switch (op) {
    case FilterOp::kEq:
      return utils::LinearSearchWithComparator(typed_val, start,
                                               std::equal_to<T>(), builder);
    case FilterOp::kNe:
      return utils::LinearSearchWithComparator(typed_val, start,
                                               std::not_equal_to<T>(), builder);
    case FilterOp::kLe:
      return utils::LinearSearchWithComparator(typed_val, start,
                                               std::less_equal<T>(), builder);
    case FilterOp::kLt:
      return utils::LinearSearchWithComparator(typed_val, start, std::less<T>(),
                                               builder);
    case FilterOp::kGt:
      return utils::LinearSearchWithComparator(typed_val, start,
                                               std::greater<T>(), builder);
    case FilterOp::kGe:
      return utils::LinearSearchWithComparator(
          typed_val, start, std::greater_equal<T>(), builder);
    case FilterOp::kGlob:
    case FilterOp::kRegex:
    case FilterOp::kIsNotNull:
    case FilterOp::kIsNull:
      PERFETTO_DFATAL("Illegal argument");
  }
}

}  // namespace

SearchValidationResult NumericStorageBase::ValidateSearchConstraints(
    SqlValue val,
    FilterOp op) const {
  // NULL checks.
  if (PERFETTO_UNLIKELY(val.is_null())) {
    if (op == FilterOp::kIsNotNull) {
      return SearchValidationResult::kAllData;
    }
    if (op == FilterOp::kIsNull) {
      return SearchValidationResult::kNoData;
    }
    PERFETTO_FATAL(
        "Invalid path. NULL should only be compared with 'IS NULL' and 'IS NOT "
        "NULL'");
  }

  // FilterOp checks. Switch so that we get a warning if new FilterOp is not
  // handled.
  switch (op) {
    case FilterOp::kEq:
    case FilterOp::kNe:
    case FilterOp::kLt:
    case FilterOp::kLe:
    case FilterOp::kGt:
    case FilterOp::kGe:
      break;
    case FilterOp::kIsNull:
    case FilterOp::kIsNotNull:
      PERFETTO_FATAL("Invalid constraint");
    case FilterOp::kGlob:
    case FilterOp::kRegex:
      return SearchValidationResult::kNoData;
  }

  // Type checks.
  switch (val.type) {
    case SqlValue::kNull:
    case SqlValue::kLong:
    case SqlValue::kDouble:
      break;
    case SqlValue::kString:
      // Any string is always more than any numeric.
      if (op == FilterOp::kLt || op == FilterOp::kLe) {
        return SearchValidationResult::kAllData;
      }
      return SearchValidationResult::kNoData;
    case SqlValue::kBytes:
      return SearchValidationResult::kNoData;
  }

  // TODO(b/307482437): There is currently no support for comparison with double
  // and it is prevented on QueryExecutor level.
  if (type_ != ColumnType::kDouble) {
    PERFETTO_CHECK(val.type != SqlValue::kDouble);
  }

  // Bounds of the value.
  enum ExtremeVal { kTooBig, kTooSmall, kOk };
  ExtremeVal extreme_validator = kOk;

  switch (type_) {
    case ColumnType::kDouble:
      // Any value would make a sensible comparison with a double.
    case ColumnType::kInt64:
      // TODO(b/307482437): As long as the type is not double there is nothing
      // to verify here, as all values are going to be in the int64_t limits.
      break;
    case ColumnType::kInt32:
      if (val.AsLong() > std::numeric_limits<int32_t>::max()) {
        extreme_validator = kTooBig;
        break;
      }
      if (val.AsLong() < std::numeric_limits<int32_t>::min()) {
        extreme_validator = kTooSmall;
        break;
      }
      break;
    case ColumnType::kUint32:
      if (val.AsLong() > std::numeric_limits<uint32_t>::max()) {
        extreme_validator = kTooBig;
        break;
      }
      if (val.AsLong() < std::numeric_limits<uint32_t>::min()) {
        extreme_validator = kTooSmall;
        break;
      }
      break;
    case ColumnType::kString:
    case ColumnType::kDummy:
    case ColumnType::kId:
      break;
  }

  switch (extreme_validator) {
    case kOk:
      return SearchValidationResult::kOk;
    case kTooBig:
      if (op == FilterOp::kLt || op == FilterOp::kLe || op == FilterOp::kNe) {
        return SearchValidationResult::kAllData;
      }
      return SearchValidationResult::kNoData;
    case kTooSmall:
      if (op == FilterOp::kGt || op == FilterOp::kGe || op == FilterOp::kNe) {
        return SearchValidationResult::kAllData;
      }
      return SearchValidationResult::kNoData;
  }

  PERFETTO_FATAL("For GCC");
}

RangeOrBitVector NumericStorageBase::Search(FilterOp op,
                                            SqlValue sql_val,
                                            RowMap::Range search_range) const {
  PERFETTO_TP_TRACE(metatrace::Category::DB, "NumericStorage::Search",
                    [&search_range, op](metatrace::Record* r) {
                      r->AddArg("Start", std::to_string(search_range.start));
                      r->AddArg("End", std::to_string(search_range.end));
                      r->AddArg("Op",
                                std::to_string(static_cast<uint32_t>(op)));
                    });

  NumericValue val = GetNumericTypeVariant(type_, sql_val);

  if (is_sorted_) {
    if (op != FilterOp::kNe) {
      return RangeOrBitVector(BinarySearchIntrinsic(op, val, search_range));
    }
    // Not equal is a special operation on binary search, as it doesn't define a
    // range, and rather just `not` range returned with `equal` operation.
    RowMap::Range r = BinarySearchIntrinsic(FilterOp::kEq, val, search_range);
    BitVector bv(r.start, true);
    bv.Resize(r.end, false);
    bv.Resize(search_range.end, true);
    return RangeOrBitVector(std::move(bv));
  }

  return RangeOrBitVector(LinearSearchInternal(op, val, search_range));
}

RangeOrBitVector NumericStorageBase::IndexSearch(FilterOp op,
                                                 SqlValue sql_val,
                                                 uint32_t* indices,
                                                 uint32_t indices_size,
                                                 bool sorted) const {
  PERFETTO_TP_TRACE(metatrace::Category::DB, "NumericStorage::IndexSearch",
                    [indices_size, op](metatrace::Record* r) {
                      r->AddArg("Count", std::to_string(indices_size));
                      r->AddArg("Op",
                                std::to_string(static_cast<uint32_t>(op)));
                    });

  NumericValue val = GetNumericTypeVariant(type_, sql_val);
  if (sorted) {
    return RangeOrBitVector(
        BinarySearchExtrinsic(op, val, indices, indices_size));
  }
  return RangeOrBitVector(IndexSearchInternal(op, val, indices, indices_size));
}

BitVector NumericStorageBase::LinearSearchInternal(FilterOp op,
                                                   NumericValue val,
                                                   RowMap::Range range) const {
  BitVector::Builder builder(range.end, range.start);
  if (const auto* u32 = std::get_if<uint32_t>(&val)) {
    auto* start = static_cast<const uint32_t*>(data_) + range.start;
    TypedLinearSearch(*u32, start, op, builder);
  } else if (const auto* i64 = std::get_if<int64_t>(&val)) {
    auto* start = static_cast<const int64_t*>(data_) + range.start;
    TypedLinearSearch(*i64, start, op, builder);
  } else if (const auto* i32 = std::get_if<int32_t>(&val)) {
    auto* start = static_cast<const int32_t*>(data_) + range.start;
    TypedLinearSearch(*i32, start, op, builder);
  } else if (const auto* db = std::get_if<double>(&val)) {
    auto* start = static_cast<const double*>(data_) + range.start;
    TypedLinearSearch(*db, start, op, builder);
  } else {
    PERFETTO_DFATAL("Invalid");
  }
  return std::move(builder).Build();
}

BitVector NumericStorageBase::IndexSearchInternal(
    FilterOp op,
    NumericValue val,
    uint32_t* indices,
    uint32_t indices_count) const {
  BitVector::Builder builder(indices_count);
  std::visit(
      [this, indices, op, &builder](auto val) {
        using T = decltype(val);
        auto* start = static_cast<const T*>(data_);
        std::visit(
            [start, indices, val, &builder](auto comparator) {
              utils::IndexSearchWithComparator(val, start, indices, comparator,
                                               builder);
            },
            GetFilterOpVariant<T>(op));
      },
      val);
  return std::move(builder).Build();
}

RowMap::Range NumericStorageBase::BinarySearchIntrinsic(
    FilterOp op,
    NumericValue val,
    RowMap::Range search_range) const {
  switch (op) {
    case FilterOp::kEq:
      return RowMap::Range(LowerBoundIntrinsic(data_, val, search_range),
                           UpperBoundIntrinsic(data_, val, search_range));
    case FilterOp::kLe: {
      return RowMap::Range(search_range.start,
                           UpperBoundIntrinsic(data_, val, search_range));
    }
    case FilterOp::kLt:
      return RowMap::Range(search_range.start,
                           LowerBoundIntrinsic(data_, val, search_range));
    case FilterOp::kGe:
      return RowMap::Range(LowerBoundIntrinsic(data_, val, search_range),
                           search_range.end);
    case FilterOp::kGt:
      return RowMap::Range(UpperBoundIntrinsic(data_, val, search_range),
                           search_range.end);
    case FilterOp::kNe:
    case FilterOp::kIsNull:
    case FilterOp::kIsNotNull:
    case FilterOp::kGlob:
    case FilterOp::kRegex:
      return RowMap::Range();
  }
  return RowMap::Range();
}

RowMap::Range NumericStorageBase::BinarySearchExtrinsic(
    FilterOp op,
    NumericValue val,
    uint32_t* indices,
    uint32_t indices_count) const {
  switch (op) {
    case FilterOp::kEq:
      return RowMap::Range(
          LowerBoundExtrinsic(data_, val, indices, indices_count),
          UpperBoundExtrinsic(data_, val, indices, indices_count));
    case FilterOp::kLe:
      return RowMap::Range(
          0, UpperBoundExtrinsic(data_, val, indices, indices_count));
    case FilterOp::kLt:
      return RowMap::Range(
          0, LowerBoundExtrinsic(data_, val, indices, indices_count));
    case FilterOp::kGe:
      return RowMap::Range(
          LowerBoundExtrinsic(data_, val, indices, indices_count), size_);
    case FilterOp::kGt:
      return RowMap::Range(
          UpperBoundExtrinsic(data_, val, indices, indices_count), size_);
    case FilterOp::kNe:
    case FilterOp::kIsNull:
    case FilterOp::kIsNotNull:
    case FilterOp::kGlob:
    case FilterOp::kRegex:
      return RowMap::Range();
  }
  return RowMap::Range();
}

void NumericStorageBase::StableSort(uint32_t* rows, uint32_t rows_size) const {
  std::visit(
      [this, &rows, rows_size](auto val_data) {
        using T = decltype(val_data);
        const T* typed_start = static_cast<const T*>(data_);
        std::stable_sort(rows, rows + rows_size,
                         [typed_start](uint32_t a_idx, uint32_t b_idx) {
                           T first_val = typed_start[a_idx];
                           T second_val = typed_start[b_idx];
                           return first_val < second_val;
                         });
      },
      GetNumericTypeVariant(type_, SqlValue::Long(0)));
}

void NumericStorageBase::Sort(uint32_t*, uint32_t) const {
  // TODO(b/307482437): Implement.
  PERFETTO_ELOG("Not implemented");
}

void NumericStorageBase::Serialize(StorageProto* msg) const {
  auto* numeric_storage_msg = msg->set_numeric_storage();
  numeric_storage_msg->set_is_sorted(is_sorted_);
  numeric_storage_msg->set_column_type(static_cast<uint32_t>(type_));

  uint32_t type_size;
  switch (type_) {
    case ColumnType::kInt64:
      type_size = sizeof(int64_t);
      break;
    case ColumnType::kInt32:
      type_size = sizeof(int32_t);
      break;
    case ColumnType::kUint32:
      type_size = sizeof(uint32_t);
      break;
    case ColumnType::kDouble:
      type_size = sizeof(double_t);
      break;
    case ColumnType::kDummy:
    case ColumnType::kId:
    case ColumnType::kString:
      PERFETTO_FATAL("Invalid column type for NumericStorage");
  }
  numeric_storage_msg->set_values(static_cast<const uint8_t*>(data_),
                                  static_cast<size_t>(type_size) * size_);
}

}  // namespace storage
}  // namespace trace_processor
}  // namespace perfetto
