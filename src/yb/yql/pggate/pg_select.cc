//--------------------------------------------------------------------------------------------------
// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
//--------------------------------------------------------------------------------------------------

#include "yb/yql/pggate/pg_select.h"
#include "yb/yql/pggate/util/pg_doc_data.h"
#include "yb/client/yb_op.h"

namespace yb {
namespace pggate {

using std::make_shared;

//--------------------------------------------------------------------------------------------------
// PgSelect
//--------------------------------------------------------------------------------------------------

PgSelect::PgSelect(PgSession::ScopedRefPtr pg_session,
                   const char *database_name,
                   const char *schema_name,
                   const char *table_name)
    : PgDml(std::move(pg_session), database_name, schema_name, table_name, StmtOp::STMT_SELECT) {
}

PgSelect::~PgSelect() {
}

Status PgSelect::Prepare() {
  RETURN_NOT_OK(LoadTable(false /* for_write */));

  // Allocate SELECT request.
  read_op_.reset(table_->NewPgsqlSelect());
  read_req_ = read_op_->mutable_request();
  PrepareColumns();

  return Status::OK();
}

void PgSelect::PrepareColumns() {
  // Setting protobuf.
  column_refs_ = read_req_->mutable_column_refs();

  // When reading, only values of partition columns are special-cased in protobuf.
  // Because Kudu API requires that partition columns must be listed in their created-order, the
  // slots for partition column bind expressions are allocated here in correct order.
  for (PgColumn &col : columns_) {
    col.AllocPartitionBindPB(read_req_);
  }
}

//--------------------------------------------------------------------------------------------------
// DML support.
// TODO(neil) WHERE clause is not yet supported. Revisit this function when it is.

PgsqlExpressionPB *PgSelect::AllocColumnBindPB(PgColumn *col) {
  return col->AllocBindPB(read_req_);
}

PgsqlExpressionPB *PgSelect::AllocTargetPB() {
  return read_req_->add_targets();
}

//--------------------------------------------------------------------------------------------------
// RESULT SET SUPPORT.
// For now, selected expressions are just a list of column names (ref).
//   SELECT column_l, column_m, column_n FROM ...

Status PgSelect::Exec() {
  // TODO(neil) The following code is a simple read and cache. It operates once and done.
  // - This will be extended to do scanning and caching chunk by chunk.
  // - "result_set_" field need to be locked and release. Available rows are fetched from the
  //   beginning while the arriving rows are append at the end.

  // Update bind values for constants and placeholders.
  RETURN_NOT_OK(UpdateBindPBs());

  // Execute select statement.
  RETURN_NOT_OK(pg_session_->Apply(read_op_));

  // Append the data and wait for the fetch request.
  result_set_.push_back(read_op_->rows_data());
  if (cursor_.empty()) {
    RETURN_NOT_OK(PgDocData::LoadCache(result_set_.front(), &total_row_count_, &cursor_));
  }

  return Status::OK();
}

}  // namespace pggate
}  // namespace yb
