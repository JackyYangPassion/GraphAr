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

std::shared_ptr<arrow::Table> read_parquet_to_arrow_table(
    const std::string& parquet_file) {
  arrow::io::IOContext io_context = arrow::io::default_io_context();

  auto fs = arrow::fs::FileSystemFromUriOrPath(parquet_file).ValueOrDie();
  std::shared_ptr<arrow::io::InputStream> input =
      fs->OpenInputStream(parquet_file).ValueOrDie();

  auto maybe_reader = parquet::arrow::FileReader::Make(io_context, input);
  std::shared_ptr<parquet::arrow::FileReader> reader = *maybe_reader;

  auto maybe_table = reader->ReadTable();
  std::shared_ptr<arrow::Table> table = *maybe_table;
  return table;
}

std::shared_ptr<arrow::Table> add_index_column(
    const std::shared_ptr<arrow::Table>& table) {
  // arrow::Int64Builder index_builder;
  // Get the number of rows in the table
  int64_t num_rows = table->num_rows();
  // Create an array containing the row numbers
  auto row_numbers = ArrayFromBuilder(Int64Builder(), [&](Int64Builder& builder) {
    for (int64_t i = 0; i < num_rows; i++) {
      builder.Append(i);
    }
  });
  // Create a new table with the row numbers column
  auto new_table = Table::Make(schema({field("index", int64()),
                                       table->schema()->field(0),
                                       table->schema()->field(1)}),
                                {row_numbers, table->column(0), table->column(1)});
  return new_table;
}

std::shared_ptr<arrow::Table> DoHashJoin(
    const std::shared_ptr<arrow::dataset::Dataset>& l_dataset,
    const std::shared_ptr<arrow::dataset::Dataset>& r_dataset,
    const std::string& l_key, const std::string& r_key) {
  auto l_options = std::make_shared<arrow::dataset::ScanOptions>();
  // create empty projection: "default" projection where each field is mapped to a
  // field_ref
  l_options->projection = cp::project({}, {});

  auto r_options = std::make_shared<arrow::dataset::ScanOptions>();
  // create empty projection: "default" projection where each field is mapped to a
  // field_ref
  r_options->projection = cp::project({}, {});

  // construct the scan node
  auto l_scan_node_options = arrow::dataset::ScanNodeOptions{l_dataset, l_options};
  auto r_scan_node_options = arrow::dataset::ScanNodeOptions{r_dataset, r_options};

  arrow::acero::Declaration left{"scan", std::move(l_scan_node_options)};
  arrow::acero::Declaration right{"scan", std::move(r_scan_node_options)};

  arrow::acero::HashJoinNodeOptions join_opts{arrow::acero::JoinType::INNER,
                                              /*in_left_keys=*/{l_key},
                                              /*in_right_keys=*/{r_key},
                                              /*filter*/ arrow::compute::literal(true),
                                              /*output_suffix_for_left*/ "_l",
                                              /*output_suffix_for_right*/ "_r"};
  arrow::acero::Declaration hashjoin{
      "hashjoin", {std::move(left), std::move(right)}, join_opts};

  // expected columns l_a, l_b
  ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Table> response_table,
                        arrow::acero::DeclarationToTable(std::move(hashjoin)));
  return response_table;
}

GraphArchive::Status write_to_graphar(
    const std::string& vertex_source_file,
    const std::string& edge_source_file,
    const std::string& graph_info_file) {
    // read vertex source to arrow table
    auto vertex_table = read_csv_to_arrow_table(vertex_source_file);
    auto edge_table = read_csv_to_arrow_table(edge_source_file);
    std::string vertex_label = "person";
    // auto& vertex_info = graph_info.GetVertexInfo(vertex_label).value();
    auto new_vertex_table = add_index_column(vertex_table);
    auto join_table = DoHashJoin(new_vertex_table, edge_table, "index", "src");
    std::cout << "Join result: " << join_table->ToString() << std::endl;
    return GraphArchive::Status::OK();
}

int main(int argc, char* argv[]) {
   std::string vertex_source file = std::string(argv[1]);
   std::string edge_source_file = std::string(argv[2]);
   std::string graph_info_file = std::string(argv[3]);
   write_to_graphar(vertex_source_file, edge_source_file, graph_info_file);
}
