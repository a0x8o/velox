/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cpr/cpr.h> // @manual
#include <folly/json.h>
#include <iostream>
#include <utility>

#include "velox/common/base/Fs.h"
#include "velox/common/encode/Base64.h"
#include "velox/connectors/hive/HiveDataSink.h"
#include "velox/connectors/hive/TableHandle.h"
#include "velox/core/Expressions.h"
#include "velox/core/ITypedExpr.h"
#include "velox/dwio/common/WriterFactory.h"
#include "velox/dwio/dwrf/writer/Writer.h"
#include "velox/exec/fuzzer/FuzzerUtil.h"
#include "velox/exec/fuzzer/PrestoQueryRunner.h"
#include "velox/exec/fuzzer/PrestoSql.h"
#include "velox/exec/tests/utils/QueryAssertions.h"
#include "velox/functions/prestosql/types/IPAddressType.h"
#include "velox/functions/prestosql/types/IPPrefixType.h"
#include "velox/functions/prestosql/types/JsonType.h"
#include "velox/functions/prestosql/types/UuidType.h"
#include "velox/serializers/PrestoSerializer.h"
#include "velox/type/parser/TypeParser.h"

using namespace facebook::velox;

namespace facebook::velox::exec::test {
namespace {

void writeToFile(
    const std::string& path,
    const std::vector<RowVectorPtr>& data,
    memory::MemoryPool* pool) {
  VELOX_CHECK_GT(data.size(), 0);

  std::shared_ptr<dwio::common::WriterOptions> options =
      std::make_shared<dwrf::WriterOptions>();
  options->schema = data[0]->type();
  options->memoryPool = pool;

  auto writeFile = std::make_unique<LocalWriteFile>(path, true, false);
  auto sink =
      std::make_unique<dwio::common::WriteFileSink>(std::move(writeFile), path);
  auto writer = dwio::common::getWriterFactory(dwio::common::FileFormat::DWRF)
                    ->createWriter(std::move(sink), options);
  for (const auto& vector : data) {
    writer->write(vector);
  }
  writer->close();
}

std::unique_ptr<ByteInputStream> toByteStream(const std::string& input) {
  std::vector<ByteRange> ranges;
  ranges.push_back(
      {reinterpret_cast<uint8_t*>(const_cast<char*>(input.data())),
       static_cast<int32_t>(input.length()),
       0});
  return std::make_unique<BufferInputStream>(std::move(ranges));
}

RowVectorPtr deserialize(
    const RowTypePtr& rowType,
    const std::string& input,
    memory::MemoryPool* pool) {
  auto byteStream = toByteStream(input);
  auto serde = std::make_unique<serializer::presto::PrestoVectorSerde>();
  RowVectorPtr result;
  serde->deserialize(byteStream.get(), pool, rowType, &result, nullptr);
  return result;
}

class ServerResponse {
 public:
  explicit ServerResponse(const std::string& responseJson)
      : response_(folly::parseJson(responseJson)) {}

  void throwIfFailed() const {
    if (response_.count("error") == 0) {
      return;
    }

    const auto& error = response_["error"];

    VELOX_FAIL(
        "Presto query failed: {} {}",
        error["errorCode"].asInt(),
        error["message"].asString());
  }

  std::string queryId() const {
    return response_["id"].asString();
  }

  bool queryCompleted() const {
    return response_.count("nextUri") == 0;
  }

  std::string nextUri() const {
    return response_["nextUri"].asString();
  }

  std::vector<RowVectorPtr> queryResults(memory::MemoryPool* pool) const {
    if (!response_.count("binaryData")) {
      return {};
    }

    std::vector<std::string> names;
    std::vector<TypePtr> types;
    for (const auto& column : response_["columns"]) {
      names.push_back(column["name"].asString());
      types.push_back(parseType(column["type"].asString()));
    }

    auto rowType = ROW(std::move(names), std::move(types));

    // There should be a single column with possibly multiple rows. Each row
    // contains base64-encoded PrestoPage of results.

    std::vector<RowVectorPtr> vectors;
    for (auto& encodedData : response_["binaryData"]) {
      const std::string data =
          encoding::Base64::decode(encodedData.stringPiece());
      vectors.push_back(deserialize(rowType, data, pool));
    }
    return vectors;
  }

 private:
  folly::dynamic response_;
};

} // namespace

PrestoQueryRunner::PrestoQueryRunner(
    memory::MemoryPool* pool,
    std::string coordinatorUri,
    std::string user,
    std::chrono::milliseconds timeout)
    : ReferenceQueryRunner(pool),
      coordinatorUri_{std::move(coordinatorUri)},
      user_{std::move(user)},
      timeout_(timeout) {
  eventBaseThread_.start("PrestoQueryRunner");
  pool_ = aggregatePool()->addLeafChild("leaf");
  queryRunnerContext_ = std::make_shared<QueryRunnerContext>();
}

std::optional<std::string> PrestoQueryRunner::toSql(
    const core::PlanNodePtr& plan) {
  if (const auto projectNode =
          std::dynamic_pointer_cast<const core::ProjectNode>(plan)) {
    return toSql(projectNode);
  }

  if (const auto windowNode =
          std::dynamic_pointer_cast<const core::WindowNode>(plan)) {
    return toSql(windowNode);
  }

  if (const auto aggregationNode =
          std::dynamic_pointer_cast<const core::AggregationNode>(plan)) {
    return toSql(aggregationNode);
  }

  if (const auto rowNumberNode =
          std::dynamic_pointer_cast<const core::RowNumberNode>(plan)) {
    return toSql(rowNumberNode);
  }

  if (const auto topNRowNumberNode =
          std::dynamic_pointer_cast<const core::TopNRowNumberNode>(plan)) {
    return toSql(topNRowNumberNode);
  }

  if (auto tableWriteNode =
          std::dynamic_pointer_cast<const core::TableWriteNode>(plan)) {
    return toSql(tableWriteNode);
  }

  if (const auto joinNode =
          std::dynamic_pointer_cast<const core::HashJoinNode>(plan)) {
    return toSql(joinNode);
  }

  if (const auto joinNode =
          std::dynamic_pointer_cast<const core::NestedLoopJoinNode>(plan)) {
    return toSql(joinNode);
  }

  if (const auto valuesNode =
          std::dynamic_pointer_cast<const core::ValuesNode>(plan)) {
    return toSql(valuesNode);
  }

  if (const auto tableScanNode =
          std::dynamic_pointer_cast<const core::TableScanNode>(plan)) {
    return toSql(tableScanNode);
  }

  VELOX_NYI();
}

namespace {

std::string toWindowCallSql(
    const core::CallTypedExprPtr& call,
    bool ignoreNulls = false) {
  std::stringstream sql;
  sql << call->name() << "(";
  toCallInputsSql(call->inputs(), sql);
  sql << ")";
  if (ignoreNulls) {
    sql << " IGNORE NULLS";
  }
  return sql.str();
}

} // namespace

const std::vector<TypePtr>& PrestoQueryRunner::supportedScalarTypes() const {
  static const std::vector<TypePtr> kScalarTypes{
      BOOLEAN(),
      TINYINT(),
      SMALLINT(),
      INTEGER(),
      BIGINT(),
      REAL(),
      DOUBLE(),
      VARCHAR(),
      VARBINARY(),
      TIMESTAMP(),
  };
  return kScalarTypes;
}

const std::unordered_map<std::string, DataSpec>&
PrestoQueryRunner::aggregationFunctionDataSpecs() const {
  // For some functions, velox supports NaN, Infinity better than presto query
  // runner, which makes the comparison impossible.
  // Add data constraint in vector fuzzer to enforce to not generate such data
  // for those functions before they are fixed in presto query runner
  static const std::unordered_map<std::string, DataSpec>
      kAggregationFunctionDataSpecs{
          {"regr_avgx", DataSpec{false, false}},
          {"regr_avgy", DataSpec{false, false}},
          {"regr_r2", DataSpec{false, false}},
          {"regr_sxx", DataSpec{false, false}},
          {"regr_syy", DataSpec{false, false}},
          {"regr_sxy", DataSpec{false, false}},
          {"regr_slope", DataSpec{false, false}},
          {"regr_replacement", DataSpec{false, false}},
          {"covar_pop", DataSpec{true, false}},
          {"covar_samp", DataSpec{true, false}},
      };

  return kAggregationFunctionDataSpecs;
}

std::optional<std::string> PrestoQueryRunner::toSql(
    const std::shared_ptr<const core::AggregationNode>& aggregationNode) {
  // Assume plan is Aggregation over Values.
  VELOX_CHECK(aggregationNode->step() == core::AggregationNode::Step::kSingle);

  if (!isSupportedDwrfType(aggregationNode->sources()[0]->outputType())) {
    return std::nullopt;
  }

  std::vector<std::string> groupingKeys;
  for (const auto& key : aggregationNode->groupingKeys()) {
    groupingKeys.push_back(key->name());
  }

  std::stringstream sql;
  sql << "SELECT " << folly::join(", ", groupingKeys);

  const auto& aggregates = aggregationNode->aggregates();
  if (!aggregates.empty()) {
    if (!groupingKeys.empty()) {
      sql << ", ";
    }

    for (auto i = 0; i < aggregates.size(); ++i) {
      appendComma(i, sql);
      const auto& aggregate = aggregates[i];
      sql << toAggregateCallSql(
          aggregate.call,
          aggregate.sortingKeys,
          aggregate.sortingOrders,
          aggregate.distinct);

      if (aggregate.mask != nullptr) {
        sql << " filter (where " << aggregate.mask->name() << ")";
      }
      sql << " as " << aggregationNode->aggregateNames()[i];
    }
  }
  // AggregationNode should have a single source.
  std::optional<std::string> source = toSql(aggregationNode->sources()[0]);
  if (!source) {
    return std::nullopt;
  }
  sql << " FROM " << *source;

  if (!groupingKeys.empty()) {
    sql << " GROUP BY " << folly::join(", ", groupingKeys);
  }

  return sql.str();
}

std::optional<std::string> PrestoQueryRunner::toSql(
    const std::shared_ptr<const core::ProjectNode>& projectNode) {
  auto sourceSql = toSql(projectNode->sources()[0]);
  if (!sourceSql.has_value()) {
    return std::nullopt;
  }

  std::stringstream sql;
  sql << "SELECT ";

  for (auto i = 0; i < projectNode->names().size(); ++i) {
    appendComma(i, sql);
    auto projection = projectNode->projections()[i];
    if (auto field =
            std::dynamic_pointer_cast<const core::FieldAccessTypedExpr>(
                projection)) {
      sql << field->name();
    } else if (
        auto call =
            std::dynamic_pointer_cast<const core::CallTypedExpr>(projection)) {
      sql << toCallSql(call);
    } else if (
        auto cast =
            std::dynamic_pointer_cast<const core::CastTypedExpr>(projection)) {
      sql << toCastSql(*cast);
    } else if (
        auto concat = std::dynamic_pointer_cast<const core::ConcatTypedExpr>(
            projection)) {
      sql << toConcatSql(*concat);
    } else if (
        auto constant =
            std::dynamic_pointer_cast<const core::ConstantTypedExpr>(
                projection)) {
      sql << toConstantSql(*constant);
    } else {
      VELOX_NYI();
    }

    sql << " as " << projectNode->names()[i];
  }

  sql << " FROM (" << sourceSql.value() << ")";
  return sql.str();
}

std::optional<std::string> PrestoQueryRunner::toSql(
    const std::shared_ptr<const core::WindowNode>& windowNode) {
  if (!isSupportedDwrfType(windowNode->sources()[0]->outputType())) {
    return std::nullopt;
  }

  std::stringstream sql;
  sql << "SELECT ";

  const auto& inputType = windowNode->sources()[0]->outputType();
  for (auto i = 0; i < inputType->size(); ++i) {
    appendComma(i, sql);
    sql << inputType->nameOf(i);
  }

  sql << ", ";

  const auto& functions = windowNode->windowFunctions();
  for (auto i = 0; i < functions.size(); ++i) {
    appendComma(i, sql);
    sql << toWindowCallSql(functions[i].functionCall, functions[i].ignoreNulls);

    sql << " OVER (";

    const auto& partitionKeys = windowNode->partitionKeys();
    if (!partitionKeys.empty()) {
      sql << "PARTITION BY ";
      for (auto j = 0; j < partitionKeys.size(); ++j) {
        appendComma(j, sql);
        sql << partitionKeys[j]->name();
      }
    }

    const auto& sortingKeys = windowNode->sortingKeys();
    const auto& sortingOrders = windowNode->sortingOrders();

    if (!sortingKeys.empty()) {
      sql << " ORDER BY ";
      for (auto j = 0; j < sortingKeys.size(); ++j) {
        appendComma(j, sql);
        sql << sortingKeys[j]->name() << " " << sortingOrders[j].toString();
      }
    }

    sql << " " << queryRunnerContext_->windowFrames_.at(windowNode->id()).at(i);
    sql << ")";
  }

  // WindowNode should have a single source.
  std::optional<std::string> source = toSql(windowNode->sources()[0]);
  if (!source) {
    return std::nullopt;
  }
  sql << " FROM " << *source;

  return sql.str();
}

bool PrestoQueryRunner::isConstantExprSupported(
    const core::TypedExprPtr& expr) {
  if (std::dynamic_pointer_cast<const core::ConstantTypedExpr>(expr)) {
    // TODO: support constant literals of these types. Complex-typed constant
    // literals require support of converting them to SQL. Json, Ipaddress,
    // Ipprefix, and Uuid can be enabled after we're able to generate valid
    // input values, because when these types are used as the type of a constant
    // literal in SQL, Presto implicitly invokes json_parse(),
    // cast(x as Ipaddress), cast(x as Ipprefix) and cast(x as uuid) on it,
    // which makes the behavior of Presto different from Velox. Timestamp
    // constant literals require further investigation to ensure Presto uses the
    // same timezone as Velox. Interval type cannot be used as the type of
    // constant literals in Presto SQL.
    auto& type = expr->type();
    return type->isPrimitiveType() && !type->isTimestamp() &&
        !isJsonType(type) && !type->isIntervalDayTime() &&
        !isIPAddressType(type) && !isIPPrefixType(type) && !isUuidType(type);
  }
  return true;
}

bool PrestoQueryRunner::isSupported(const exec::FunctionSignature& signature) {
  // TODO: support queries with these types. Among the types below, hugeint is
  // not a native type in Presto, so fuzzer should not use it as the type of
  // cast-to or constant literals. Hyperloglog and TDigest can only be casted
  // from varbinary and cannot be used as the type of constant literals.
  // Interval year to month can only be casted from NULL and cannot be used as
  // the type of constant literals. Json, Ipaddress, Ipprefix, and UUID require
  // special handling, because Presto requires literals of these types to be
  // valid, and doesn't allow creating HIVE columns of these types.
  return !(
      usesTypeName(signature, "bingtile") ||
      usesTypeName(signature, "interval year to month") ||
      usesTypeName(signature, "hugeint") ||
      usesTypeName(signature, "hyperloglog") ||
      usesTypeName(signature, "tdigest") ||
      usesInputTypeName(signature, "json") ||
      usesInputTypeName(signature, "ipaddress") ||
      usesInputTypeName(signature, "ipprefix") ||
      usesInputTypeName(signature, "uuid"));
}

std::optional<std::string> PrestoQueryRunner::toSql(
    const std::shared_ptr<const core::RowNumberNode>& rowNumberNode) {
  if (!isSupportedDwrfType(rowNumberNode->sources()[0]->outputType())) {
    return std::nullopt;
  }

  std::stringstream sql;
  sql << "SELECT ";

  const auto& inputType = rowNumberNode->sources()[0]->outputType();
  for (auto i = 0; i < inputType->size(); ++i) {
    appendComma(i, sql);
    sql << inputType->nameOf(i);
  }

  sql << ", row_number() OVER (";

  const auto& partitionKeys = rowNumberNode->partitionKeys();
  if (!partitionKeys.empty()) {
    sql << "partition by ";
    for (auto i = 0; i < partitionKeys.size(); ++i) {
      appendComma(i, sql);
      sql << partitionKeys[i]->name();
    }
  }

  // RowNumberNode should have a single source.
  std::optional<std::string> source = toSql(rowNumberNode->sources()[0]);
  if (!source) {
    return std::nullopt;
  }
  sql << ") as row_number FROM " << *source;

  return sql.str();
}

std::optional<std::string> PrestoQueryRunner::toSql(
    const std::shared_ptr<const core::TopNRowNumberNode>& topNRowNumberNode) {
  if (!isSupportedDwrfType(topNRowNumberNode->sources()[0]->outputType())) {
    return std::nullopt;
  }

  std::stringstream sql;
  sql << "SELECT * FROM (SELECT ";

  const auto& inputType = topNRowNumberNode->sources()[0]->outputType();
  for (auto i = 0; i < inputType->size(); ++i) {
    appendComma(i, sql);
    sql << inputType->nameOf(i);
  }

  sql << ", row_number() OVER (";

  const auto& partitionKeys = topNRowNumberNode->partitionKeys();
  if (!partitionKeys.empty()) {
    sql << "partition by ";
    for (auto i = 0; i < partitionKeys.size(); ++i) {
      appendComma(i, sql);
      sql << partitionKeys[i]->name();
    }
  }

  const auto& sortingKeys = topNRowNumberNode->sortingKeys();
  const auto& sortingOrders = topNRowNumberNode->sortingOrders();

  if (!sortingKeys.empty()) {
    sql << " ORDER BY ";
    for (auto j = 0; j < sortingKeys.size(); ++j) {
      appendComma(j, sql);
      sql << sortingKeys[j]->name() << " " << sortingOrders[j].toString();
    }
  }

  std::string rowNumberColumnName = topNRowNumberNode->generateRowNumber()
      ? topNRowNumberNode->outputType()->nameOf(
            topNRowNumberNode->outputType()->children().size() - 1)
      : "row_number";

  // TopNRowNumberNode should have a single source.
  std::optional<std::string> source = toSql(topNRowNumberNode->sources()[0]);
  if (!source) {
    return std::nullopt;
  }
  sql << ") as " << rowNumberColumnName << " FROM " << *source << ") ";
  sql << " where " << rowNumberColumnName
      << " <= " << topNRowNumberNode->limit();

  return sql.str();
}

std::optional<std::string> PrestoQueryRunner::toSql(
    const std::shared_ptr<const core::TableWriteNode>& tableWriteNode) {
  auto insertTableHandle =
      std::dynamic_pointer_cast<connector::hive::HiveInsertTableHandle>(
          tableWriteNode->insertTableHandle()->connectorInsertTableHandle());

  // Returns a CTAS sql with specified table properties from TableWriteNode,
  // example sql:
  // CREATE TABLE tmp_write WITH (
  // PARTITIONED_BY = ARRAY['p0'],
  // BUCKETED_COUNT = 2, BUCKETED_BY = ARRAY['b0', 'b1'],
  // SORTED_BY = ARRAY['s0 ASC', 's1 DESC'],
  // FORMAT = 'ORC'
  // )
  // AS SELECT * FROM t_<id>
  std::stringstream sql;
  sql << "CREATE TABLE tmp_write";
  std::vector<std::string> partitionKeys;
  for (auto i = 0; i < tableWriteNode->columnNames().size(); ++i) {
    if (insertTableHandle->inputColumns()[i]->isPartitionKey()) {
      partitionKeys.push_back(insertTableHandle->inputColumns()[i]->name());
    }
  }
  sql << " WITH (";

  if (insertTableHandle->isPartitioned()) {
    sql << " PARTITIONED_BY = ARRAY[";
    for (int i = 0; i < partitionKeys.size(); ++i) {
      appendComma(i, sql);
      sql << "'" << partitionKeys[i] << "'";
    }
    sql << "], ";

    if (insertTableHandle->bucketProperty() != nullptr) {
      const auto bucketCount =
          insertTableHandle->bucketProperty()->bucketCount();
      const auto bucketColumns =
          insertTableHandle->bucketProperty()->bucketedBy();
      sql << " BUCKET_COUNT = " << bucketCount << ", BUCKETED_BY = ARRAY[";
      for (int i = 0; i < bucketColumns.size(); ++i) {
        appendComma(i, sql);
        sql << "'" << bucketColumns[i] << "'";
      }
      sql << "], ";

      const auto sortColumns = insertTableHandle->bucketProperty()->sortedBy();
      if (!sortColumns.empty()) {
        sql << " SORTED_BY = ARRAY[";
        for (int i = 0; i < sortColumns.size(); ++i) {
          appendComma(i, sql);
          sql << "'" << sortColumns[i]->sortColumn() << " "
              << (sortColumns[i]->sortOrder().isAscending() ? "ASC" : "DESC")
              << "'";
        }
        sql << "], ";
      }
    }
  }

  // TableWriteNode should have a single source.
  std::optional<std::string> source = toSql(tableWriteNode->sources()[0]);
  if (!source) {
    return std::nullopt;
  }
  sql << "FORMAT = 'ORC')  AS SELECT * FROM " << *source;

  return sql.str();
}

std::pair<
    std::optional<std::multiset<std::vector<velox::variant>>>,
    ReferenceQueryErrorCode>
PrestoQueryRunner::execute(const core::PlanNodePtr& plan) {
  std::pair<
      std::optional<std::vector<velox::RowVectorPtr>>,
      ReferenceQueryErrorCode>
      result = executeAndReturnVector(plan);
  if (result.first) {
    return std::make_pair(
        exec::test::materialize(*result.first), result.second);
  }
  return std::make_pair(std::nullopt, result.second);
}

std::string PrestoQueryRunner::createTable(
    const std::string& name,
    const TypePtr& type) {
  auto inputType = asRowType(type);
  std::stringstream nullValues;
  for (auto i = 0; i < inputType->size(); ++i) {
    appendComma(i, nullValues);
    nullValues << fmt::format(
        "cast(null as {})", toTypeSql(inputType->childAt(i)));
  }

  execute(fmt::format("DROP TABLE IF EXISTS {}", name));

  execute(fmt::format(
      "CREATE TABLE {}({}) WITH (format = 'DWRF') AS SELECT {}",
      name,
      folly::join(", ", inputType->names()),
      nullValues.str()));

  // Query Presto to find out table's location on disk.
  auto results = execute(fmt::format("SELECT \"$path\" FROM {}", name));

  auto filePath = extractSingleValue<StringView>(results);
  auto tableDirectoryPath = fs::path(filePath).parent_path();

  // Delete the all-null row.
  execute(fmt::format("DELETE FROM {}", name));

  return tableDirectoryPath;
}

std::pair<
    std::optional<std::vector<velox::RowVectorPtr>>,
    ReferenceQueryErrorCode>
PrestoQueryRunner::executeAndReturnVector(const core::PlanNodePtr& plan) {
  if (std::optional<std::string> sql = toSql(plan)) {
    try {
      std::unordered_map<std::string, std::vector<velox::RowVectorPtr>>
          inputMap = getAllTables(plan);
      for (const auto& [tableName, input] : inputMap) {
        auto inputType = asRowType(input[0]->type());
        if (inputType->size() == 0) {
          inputMap[tableName] = {
              makeNullRows(input, fmt::format("{}x", tableName), pool())};
        }
      }

      auto writerPool = aggregatePool()->addAggregateChild("writer");
      for (const auto& [tableName, input] : inputMap) {
        auto tableDirectoryPath = createTable(tableName, input[0]->type());

        // Create a new file in table's directory with fuzzer-generated data.
        auto filePath = fs::path(tableDirectoryPath)
                            .append(fmt::format("{}.dwrf", tableName))
                            .string()
                            .substr(strlen("file:"));

        writeToFile(filePath, input, writerPool.get());
      }

      // Run the query.
      return std::make_pair(execute(*sql), ReferenceQueryErrorCode::kSuccess);
    } catch (const VeloxRuntimeError& e) {
      // Throw if connection to Presto server is unsuccessful.
      if (e.message().find("Couldn't connect to server") != std::string::npos) {
        throw;
      }
      LOG(WARNING) << "Query failed in Presto";
      return std::make_pair(
          std::nullopt, ReferenceQueryErrorCode::kReferenceQueryFail);
    } catch (...) {
      LOG(WARNING) << "Query failed in Presto";
      return std::make_pair(
          std::nullopt, ReferenceQueryErrorCode::kReferenceQueryFail);
    }
  }

  LOG(INFO) << "Query not supported in Presto";
  return std::make_pair(
      std::nullopt, ReferenceQueryErrorCode::kReferenceQueryUnsupported);
}

std::vector<RowVectorPtr> PrestoQueryRunner::execute(const std::string& sql) {
  return execute(sql, "");
}

std::vector<RowVectorPtr> PrestoQueryRunner::execute(
    const std::string& sql,
    const std::string& sessionProperty) {
  LOG(INFO) << "Execute presto sql: " << sql;
  auto response = ServerResponse(startQuery(sql, sessionProperty));
  response.throwIfFailed();

  std::vector<RowVectorPtr> queryResults;
  for (;;) {
    for (auto& result : response.queryResults(pool_.get())) {
      queryResults.push_back(result);
    }

    if (response.queryCompleted()) {
      break;
    }

    response = ServerResponse(fetchNext(response.nextUri()));
    response.throwIfFailed();
  }

  return queryResults;
}

std::string PrestoQueryRunner::startQuery(
    const std::string& sql,
    const std::string& sessionProperty) {
  auto uri = fmt::format("{}/v1/statement?binaryResults=true", coordinatorUri_);
  cpr::Url url{uri};
  cpr::Body body{sql};
  cpr::Header header(
      {{"X-Presto-User", user_},
       {"X-Presto-Catalog", "hive"},
       {"X-Presto-Schema", "tpch"},
       {"Content-Type", "text/plain"},
       {"X-Presto-Session", sessionProperty}});
  cpr::Timeout timeout{timeout_};
  cpr::Response response = cpr::Post(url, body, header, timeout);
  VELOX_CHECK_EQ(
      response.status_code,
      200,
      "POST to {} failed: {}",
      uri,
      response.error.message);
  return response.text;
}

std::string PrestoQueryRunner::fetchNext(const std::string& nextUri) {
  cpr::Url url(nextUri);
  cpr::Header header({{"X-Presto-Client-Binary-Results", "true"}});
  cpr::Timeout timeout{timeout_};
  cpr::Response response = cpr::Get(url, header, timeout);
  VELOX_CHECK_EQ(
      response.status_code,
      200,
      "GET from {} failed: {}",
      nextUri,
      response.error.message);
  return response.text;
}

bool PrestoQueryRunner::supportsVeloxVectorResults() const {
  return true;
}

} // namespace facebook::velox::exec::test
