# Third-party sources

| Source | Version | Used for | License |
|---|---|---|---|
| [aiyou178/pg_eviltransform](https://github.com/aiyou178/pg_eviltransform/tree/434b8fb97aee24931e56c8a4686dea292e63df8c) | 0.0.7, `434b8fb97aee24931e56c8a4686dea292e63df8c` | `src/coord.rs` formulas and weighted Jenks algorithm adapted to C++ | MIT, see LICENSE |
| [DuckDB](https://github.com/duckdb/duckdb/tree/v1.4.0/src/include) | v1.4.0 headers, stable API v1.2.0 | vendored `duckdb.h`, `duckdb_extension.h` | MIT, see third_party/duckdb/LICENSE |
| [DuckDB extension-ci-tools](https://github.com/duckdb/extension-ci-tools/blob/main/scripts/append_extension_metadata.py) | metadata format retrieved 2026-10-22 | `scripts/append_metadata.py` footer writer | MIT, see third_party/duckdb/LICENSE |

Coordinate formulas are credited upstream to [googollee/eviltransform](https://github.com/googollee/eviltransform).
The PostgreSQL-only regex SQL comparison implementation is not included in this port.
