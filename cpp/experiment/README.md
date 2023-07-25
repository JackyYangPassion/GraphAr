## Experiment tool

### Usage
Transform the csv file to graphar file.

```bash
./data_write graph_info_path source_label edge_label target_label \
    source_vertex_csv_path edge_csv_path target_vertex_csv_path
```

the tool now only support to write the graph in a whole chunk and generate CSR
for the edge.
