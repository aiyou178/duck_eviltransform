# duck_eviltransform

[中文文档](README.zh-CN.md)

Initial release: **0.1**. [Build and test](https://github.com/aiyou178/duck_eviltransform/actions/workflows/ci.yml).

DuckDB extension ported from [aiyou178/pg_eviltransform](https://github.com/aiyou178/pg_eviltransform),
source version `0.0.7`, commit `434b8fb97aee24931e56c8a4686dea292e63df8c`.
It adds WGS84 / GCJ02 / BD09 coordinate transformations and exact Jenks natural breaks.
Standard coordinate projections use DuckDB's `spatial` extension and its bundled PROJ database.

## Existing DuckDB extensions

As of 2026-10-22, no equivalent GCJ02/BD09 extension was found in the official
[core](https://duckdb.org/docs/lts/core_extensions/overview) or
[community](https://duckdb.org/community_extensions/list_of_extensions) catalogs,
or in the [spatial source](https://github.com/duckdb/duckdb-spatial).
`spatial.ST_Transform` covers standard CRSs such as EPSG:4326, EPSG:3857 and
EPSG:4490, but does not implement the GCJ02/BD09 offsets.
No equivalent Jenks function was found in those catalogs either.
This is a catalog/source search result, not a claim about every unpublished third-party extension.

## Build and load

Requires a C++17 compiler, CMake 3.16+, and Python 3. No Rust, PostgreSQL, PostGIS,
or DuckDB source build is required. DuckDB C API headers are vendored under `third_party/duckdb`.

```bash
make release
duckdb -unsigned
```

```sql
INSTALL spatial;
LOAD spatial;
LOAD '/absolute/path/to/duck_eviltransform/build/release/eviltransform.duckdb_extension';

SELECT ST_AsText(ST_EvilTransform(ST_Point(120, 30), 'WGS84', 'GCJ02'));
```

Replace the load path with the absolute path to your build (required by the hardened
macOS 2.0 CLI). The binary is named `eviltransform.duckdb_extension`. This local build is unsigned;
Python clients can use `duckdb.connect(config={'allow_unsigned_extensions': True})`.
The extension is not published in the community repository, so `INSTALL eviltransform FROM community`
is not an installation method for this project yet.

The stable C extension ABI is targeted at `v1.2.0`; SQL compatibility is tested with
DuckDB `v1.4.0` and `v2.0.0` by CI. Platform binaries still need to match the host.
For a non-default ABI/platform, pass the exact value of `PRAGMA platform`:

```bash
make release CMAKE_ARGS='-DDUCKDB_PLATFORM=linux_amd64'
```

Loading registers the native functions and creates/replaces the `ST_EvilTransform`
macro in the database's `main` schema. Load `spatial` and `eviltransform` in each new
database process. Initialization needs a writable database for the macro definition.

## Coordinate transforms

```sql
ST_EvilTransform(geometry, from_crs, to_crs) -> GEOMETRY
EvilTransformWKB(blob, from_crs VARCHAR, to_crs VARCHAR) -> BLOB
```

`ST_EvilTransform` accepts text CRS definitions and integer EPSG codes on either side.
The explicit source CRS is required, including for inputs that already have CRS metadata.
All coordinates use **X = longitude/easting, Y = latitude/northing**, including
EPSG:4326 and EPSG:4490 (`always_xy := true`).

| CRS | Accepted names |
|---|---|
| WGS84 | `WGS84`, `WGS-84`, `4326`, `EPSG:4326` |
| GCJ02 | `GCJ02`, `GCJ-02`, `990001`, `EPSG:990001` |
| BD09 | `BD09`, `BD-09`, `990002`, `EPSG:990002` |
| Geographic CGCS2000 | `CGCS2000`, `CGCS-2000`, `4490`, `EPSG:4490` |
| Other projections | EPSG integer, authority identifier, or a PROJ CRS definition accepted by `spatial` |

Aliases are case-insensitive and trimmed. CGCS2000 aliases mean geographic EPSG:4490;
use the actual EPSG code for a projected Gauss–Krüger zone.
`990001` and `990002` are project aliases, not official EPSG registrations.

```sql
-- WGS84 -> GCJ02 / BD09
SELECT ST_EvilTransform(ST_Point(120, 30), 4326, 990001);
SELECT ST_EvilTransform(ST_Point(120, 30), 'WGS84', 'BD09');

-- Custom coordinates -> standard projection, via WGS84
SELECT ST_EvilTransform(ST_Point(120.011070620552, 30.0038830555128), 'BD09', 3857);

-- Standard projections and geographic CGCS2000 delegate to spatial/PROJ
SELECT ST_EvilTransform(ST_Point(120, 30), 'CGCS2000', 'GCJ02');
SELECT ST_EvilTransform(ST_Point(120, 30), 4326, 3857);

-- Lines, polygons, multi-geometries, and nested collections
SELECT ST_EvilTransform(ST_GeomFromText('LINESTRING(120 30,121 31)'), 4326, 'GCJ02');

-- WKB API: only WGS84 / GCJ02 / BD09; no standard reprojection
SELECT ST_GeomFromWKB(EvilTransformWKB(ST_AsWKB(ST_Point(120,30)), 'WGS84', 'BD09'));
```

Points, lines, polygons, multi-geometries, collections, empty geometries and NULLs are supported.
The WKB engine supports little/big endian, ISO Z/M/ZM and EWKB. It preserves Z/M
ordinates and empty point NaN payloads, updates existing EWKB SRID fields, and rejects
malformed/truncated WKB, trailing bytes, unsupported types and nesting beyond 128 levels.
The geometry interface preserves the geometry content through `spatial` WKB conversion;
it does not attach custom CRS metadata to its output. Keep the CRS alongside stored data.

The port deliberately retains the original approximate GCJ02 inverse, Earth radius
`6378137`, and its rectangular China guard (`72.004..137.8347`, `0.8293..55.8271`).
Outside that guard the custom formulas leave coordinates unchanged. Round trips are
approximate. Standard projection accuracy and Z treatment follow `spatial`/PROJ.

## Jenks natural breaks

```sql
ST_JenksBins(values DOUBLE[], breaks INTEGER [, invert BOOLEAN]) -> DOUBLE[]
ST_JenksBinsAgg(value DOUBLE, breaks INTEGER [, invert BOOLEAN]) -> DOUBLE[]
```

```sql
SELECT ST_JenksBins([1,2,NULL,3,10,11,12], 2);       -- [3,12]
SELECT ST_JenksBins([1,2,3,10,11,12], 2, true);     -- [1,10]
SELECT ST_JenksBinsAgg(value, 2)
FROM (VALUES (1), (2), (3), (10), (11), (12)) AS t(value);
```

`invert` defaults to false (upper edges); true returns lower edges. Input values
are converted to DOUBLE. NULL elements/values are ignored. Empty/all-NULL inputs
return NULL. Invalid class counts, NaN and infinities raise errors. If the number
of distinct values is no greater than `breaks`, sorted unique values are returned.

The aggregate stores distinct values and their frequencies and supports parallel
combine, without materializing an input list. `breaks` and `invert` must be constant
within each group, including rows whose value is NULL. A NULL configuration parameter
returns NULL (for aggregates, any such row makes the group's result NULL).
Memory grows with distinct values and the number of classes. The exact dynamic program
uses weighted variance and divide-and-conquer optimization, with normalized long-double
arithmetic for large finite inputs.

## Differences from PostgreSQL

- DuckDB uses the explicit three-argument CRS interface. PostgreSQL's source-SRID-based
  two-argument overload is not emulated; custom CRS metadata cannot be inferred reliably.
- `ST_JenksBins` takes a list; the streaming aggregate is named `ST_JenksBinsAgg` to avoid
  a scalar/aggregate catalog-name collision in DuckDB.
- The PostGIS GSERIALIZED/librttopo layer, pgrx, packaging, migration SQL and regex benchmark
  baseline are replaced by the DuckDB C extension API and a binary WKB vertex walker.

## Validation

After implementation/build, run:

```bash
make test
python3 scripts/fetch_duckdb.py v2.0.0 .deps/duckdb-v2
make test DUCKDB=.deps/duckdb-v2/duckdb
```

`test/integration.py` uses the real CLI and installs `spatial` before testing. It covers
reference coordinates, geometry/WKB structure, projection delegation, vectorized rows,
NULLs/errors, exact Jenks partitions and aggregate lifecycle. CI builds on Linux, macOS
and Windows; a workflow configuration is not evidence of a completed CI run.

Local validation on 2026-10-22: the same macOS ARM64 release binary passed all 9
integration groups on DuckDB 1.4.0 (`b8a06e4a22`) and 2.0.0 (`d54cbbe3df`), including
the 1,048,576-row parallel Jenks case. Linux/Windows builds and remote CI have not
been run in this checkout.

## License and attribution

MIT. Coordinate and Jenks algorithms are adapted from `pg_eviltransform` under its MIT
license, with coordinate formulas credited there to
[googollee/eviltransform](https://github.com/googollee/eviltransform).
DuckDB C API headers and the metadata-footer format are from DuckDB (MIT).
See [THIRD_PARTY.md](THIRD_PARTY.md) and [LICENSE](LICENSE).
