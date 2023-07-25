/** Copyright 2022 Alibaba Group Holding Limited.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "arrow/api.h"
#include "arrow/csv/api.h"
#include "arrow/filesystem/api.h"
#include "arrow/io/api.h"
#include "arrow/stl.h"
#include "arrow/util/uri.h"
#include "parquet/arrow/reader.h"
#include "parquet/arrow/writer.h"

/*
#include "arrow/acero/exec_plan.h"
#include "arrow/compute/api.h"
#include "arrow/compute/expression.h"
#include "arrow/dataset/dataset.h"
#include "arrow/dataset/plan.h"
#include "arrow/dataset/scanner.h"
*/

#include "gar/graph_info.h"
#include "gar/writer/arrow_chunk_writer.h"

std::shared_ptr<arrow::Table> read_csv_to_arrow_table(
    const std::string& csv_file) {
  arrow::io::IOContext io_context = arrow::io::default_io_context();

  auto fs = arrow::fs::FileSystemFromUriOrPath(csv_file).ValueOrDie();
  std::shared_ptr<arrow::io::InputStream> input =
      fs->OpenInputStream(csv_file).ValueOrDie();

  auto read_options = arrow::csv::ReadOptions::Defaults();
  auto parse_options = arrow::csv::ParseOptions::Defaults();
  parse_options.delimiter = '|';
  auto convert_options = arrow::csv::ConvertOptions::Defaults();

  // Instantiate TableReader from input stream and options
  auto maybe_reader = arrow::csv::TableReader::Make(
      io_context, input, read_options, parse_options, convert_options);
  std::shared_ptr<arrow::csv::TableReader> reader = *maybe_reader;

  // Read table from CSV file
  auto maybe_table = reader->Read();
  std::shared_ptr<arrow::Table> table = *maybe_table;
  return table;
}

std::shared_ptr<arrow::Table> add_index_column(
    const std::shared_ptr<arrow::Table>& table, bool is_src=true) {
  // arrow::Int64Builder index_builder;
  // Get the number of rows in the table
  int64_t num_rows = table->num_rows();
  // Create an array containing the row numbers
  arrow::Int64Builder builder;
  for (int64_t i = 0; i < num_rows; i++) {
    builder.Append(i);
  }
  std::shared_ptr<arrow::Array> row_numbers;
  builder.Finish(&row_numbers);
  // std::shard_ptr<arrow::Field> field = std::make_shared<arrow::Field>(
  //     "index", arrow::int64(), /*nullable=*/false);
  std::string col_name = is_src ? GAR_NAMESPACE_INTERNAL::GeneralParams::kSrcIndexCol : GAR_NAMESPACE_INTERNAL::GeneralParams::kDstIndexCol ;
  auto new_field = arrow::field(col_name, arrow::int64());
  auto chunked_array = arrow::ChunkedArray::Make({row_numbers}).ValueOrDie();
  // Create a new table with the row numbers column
  auto new_table = table->AddColumn(0, new_field, chunked_array).ValueOrDie();
  return new_table;
}

GraphArchive::Status write_to_graphar(
    const std::string& graph_info_file,
    const std::string& src_label,
    const std::string& edge_label,
    const std::string& dst_label,
    const std::string& src_vertex_source_file,
    const std::string& edge_source_file,
    const std::string& dst_vertex_source_file) {
    // read vertex source to arrow table
    auto dst_vertex_table = read_csv_to_arrow_table(dst_vertex_source_file);
    auto edge_table = read_csv_to_arrow_table(edge_source_file);
    auto dst_vertex_table_with_index = add_index_column(dst_vertex_table, false);
    dst_vertex_table.reset();
    // auto& vertex_info = graph_info.GetVertexInfo(vertex_label).value();
    auto edge_table_with_dst_index = GAR_NAMESPACE::DoHashJoin(dst_vertex_table_with_index, edge_table, GAR_NAMESPACE_INTERNAL::GeneralParams::kDstIndexCol, "dst");
    edge_table.reset();
    std::shared_ptr<arrow::Table> src_vertex_table_with_index;
    if (src_label == dst_label) {
      // rename the column name
      auto old_schema = dst_vertex_table_with_index->schema();
      std::vector<std::string> new_names;
      for (int i = 0; i < old_schema->num_fields(); i++) {
        if (old_schema->field(i)->name() == GAR_NAMESPACE::GeneralParams::kDstIndexCol) {
          // Create a new field with the new name and the same data type as the old field
          new_names.push_back(GAR_NAMESPACE::GeneralParams::kSrcIndexCol);
        } else {
          // Copy over the old field
          new_names.push_back(old_schema->field(i)->name());
        }
      }
      src_vertex_table_with_index = dst_vertex_table_with_index->RenameColumns(new_names).ValueOrDie();
    } else {
      auto src_vertex_table = read_csv_to_arrow_table(src_vertex_source_file);
      src_vertex_table_with_index = add_index_column(src_vertex_table, true);
    }
    dst_vertex_table_with_index.reset();
    auto edge_table_with_src_dst_index = GAR_NAMESPACE::DoHashJoin(src_vertex_table_with_index, edge_table_with_dst_index, GAR_NAMESPACE_INTERNAL::GeneralParams::kSrcIndexCol, "src");

    auto graph_info_result = GAR_NAMESPACE::GraphInfo::Load(graph_info_file);
    if (graph_info_result.has_error()) {
      std::cout << graph_info_result.status().message() << std::endl;
    }
    auto& graph_info = graph_info_result.value();
    auto& edge_info = graph_info.GetEdgeInfo(src_label, edge_label, dst_label).value();
    auto adj_list_type = GAR_NAMESPACE::AdjListType::ordered_by_source;
    GAR_NAMESPACE::EdgeChunkWriter writer(edge_info, graph_info.GetPrefix(), adj_list_type);
    writer.WriteTable(edge_table_with_src_dst_index, 0, 0);
    writer.WriteEdgesNum(0, edge_table_with_src_dst_index->num_rows());
    writer.WriteVerticesNum(src_vertex_table_with_index->num_rows());
    return GraphArchive::Status::OK();
}

int main(int argc, char* argv[]) {
   std::string graph_info_file = std::string(argv[1]);
   std::string src_label = std::string(argv[2]);
   std::string edge_label = std::string(argv[3]);
   std::string dst_label = std::string(argv[4]);
   std::string src_vertex_source_file = std::string(argv[5]);
   std::string edge_source_file = std::string(argv[6]);
   std::string dst_vertex_source_file = std::string(argv[7]);
   write_to_graphar(graph_info_file, src_label, edge_label, dst_label, src_vertex_source_file, edge_source_file, dst_vertex_source_file);
}
