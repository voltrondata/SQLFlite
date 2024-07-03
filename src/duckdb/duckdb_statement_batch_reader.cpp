// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "duckdb_statement_batch_reader.h"

#include <duckdb.h>

#include <iostream>

#include "arrow/builder.h"
#include <arrow/c/bridge.h>

#include "duckdb_statement.h"

// clang-format off
namespace arrow {
namespace flight {
namespace sql {
namespace duckdbflight {

// Batch size for SQLite statement results
static constexpr int kMaxBatchSize = 1024;

std::shared_ptr<Schema> DuckDBStatementBatchReader::schema() const { return schema_; }

DuckDBStatementBatchReader::DuckDBStatementBatchReader(
    std::shared_ptr<DuckDBStatement> statement, std::shared_ptr<Schema> schema)
    : statement_(std::move(statement)),
      schema_(std::move(schema)),
      rc_(DuckDBSuccess),
      already_executed_(false),
      results_read_(false) {}

arrow::Result<std::shared_ptr<DuckDBStatementBatchReader>>
DuckDBStatementBatchReader::Create(const std::shared_ptr<DuckDBStatement>& statement_) {
  ARROW_ASSIGN_OR_RAISE(auto schema, statement_->GetSchema());

  std::shared_ptr<DuckDBStatementBatchReader> result(
      new DuckDBStatementBatchReader(statement_, schema));

  return result;
}

arrow::Result<std::shared_ptr<DuckDBStatementBatchReader>>
DuckDBStatementBatchReader::Create(const std::shared_ptr<DuckDBStatement>& statement,
                                   const std::shared_ptr<Schema>& schema) {
  std::shared_ptr<DuckDBStatementBatchReader> result(
      new DuckDBStatementBatchReader(statement, schema));

  return result;
}

Status DuckDBStatementBatchReader::ReadNext(std::shared_ptr<RecordBatch>* out) {

  if (!already_executed_) {
    ARROW_ASSIGN_OR_RAISE(rc_, statement_->Execute());
    already_executed_ = true;
  }
  ARROW_ASSIGN_OR_RAISE(*out, statement_->FetchResult());

  return Status::OK();
}

}  // namespace duckdbflight
}  // namespace sql
}  // namespace flight
}  // namespace arrow
