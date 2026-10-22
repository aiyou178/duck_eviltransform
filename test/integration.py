#!/usr/bin/env python3
"""Exercise the installed SQL API through the DuckDB CLI (standard library only).

The coordinate fixtures are fixed observations from pg_eviltransform's public
reference tests, plus the published Beijing WGS/BD pair.  The inverse transform
is deliberately approximate, so those fixtures use a 3e-5 degree tolerance.
Jenks expectations come from exhaustive partitions with exact rational costs,
rather than a second implementation of the extension's dynamic program.
"""

import argparse
from collections import Counter
from fractions import Fraction
from itertools import combinations
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import unittest


def sql_string(value):
    return "'" + str(value).replace("'", "''") + "'"


# x/y order, following WKB and DuckDB, not the Rust latitude/longitude API.
BEIJING = (116.404, 39.915)
GCJ_BEIJING = (116.41024449916938, 39.91640428150164)
BD_FROM_GCJ = (116.41036949371029, 39.92133699351021)
BD_BEIJING = (116.41662724378733, 39.922699552216216)
PATHS = (
    ("WGS84", "GCJ02", BEIJING, GCJ_BEIJING, 1e-6),
    ("GCJ02", "WGS84", GCJ_BEIJING, BEIJING, 3e-5),
    ("GCJ02", "BD09", BEIJING, BD_FROM_GCJ, 1e-6),
    ("BD09", "GCJ02", BD_FROM_GCJ, BEIJING, 1e-6),
    ("WGS84", "BD09", BEIJING, BD_BEIJING, 3e-5),
    ("BD09", "WGS84", BD_BEIJING, BEIJING, 3e-5),
)


class WKB:
    """Construct standard WKB fixtures, recording just their XY byte offsets."""

    def __init__(self, raw, points=()):
        self.raw = raw
        self.points = list(points)

    @staticmethod
    def header(kind, endian="<"):
        return struct.pack(endian + "BI", endian == "<", kind)

    @classmethod
    def point(cls, xy=BEIJING, expected=GCJ_BEIJING, endian="<", kind=1,
              extra=b"", srid=None):
        raw = cls.header(kind, endian)
        if srid is not None:
            raw += struct.pack(endian + "I", srid)
        offset = len(raw)
        raw += struct.pack(endian + "dd", *xy) + extra
        return cls(raw, [(offset, endian, expected)])

    @classmethod
    def sequence(cls, kind, coordinates, endian="<"):
        raw = cls.header(kind, endian) + struct.pack(endian + "I", len(coordinates))
        points = []
        for xy, expected in coordinates:
            points.append((len(raw), endian, expected))
            raw += struct.pack(endian + "dd", *xy)
        return cls(raw, points)

    @classmethod
    def polygon(cls, rings, endian="<"):
        raw = cls.header(3, endian) + struct.pack(endian + "I", len(rings))
        points = []
        for ring in rings:
            raw += struct.pack(endian + "I", len(ring))
            for xy, expected in ring:
                points.append((len(raw), endian, expected))
                raw += struct.pack(endian + "dd", *xy)
        return cls(raw, points)

    @classmethod
    def collection(cls, kind, children, endian="<"):
        raw = cls.header(kind, endian) + struct.pack(endian + "I", len(children))
        points = []
        for child in children:
            points.extend((offset + len(raw), order, xy) for offset, order, xy in child.points)
            raw += child.raw
        return cls(raw, points)


def optimal_jenks(counts, k, invert=False):
    """All optimal outputs, allowing legitimate ties between partitions."""
    values = sorted(counts)
    if not values:
        return {None}
    if len(values) <= k:
        return {tuple(float(v) for v in values)}
    best_cost = None
    outputs = set()
    for cuts in combinations(range(1, len(values)), k - 1):
        limits = (0,) + cuts + (len(values),)
        cost = Fraction(0)
        edges = []
        for start, end in zip(limits, limits[1:]):
            group = values[start:end]
            weight = sum(counts[value] for value in group)
            mean = sum(Fraction(value) * counts[value] for value in group) / weight
            cost += sum((Fraction(value) - mean) ** 2 * counts[value] for value in group)
            edges.append(float(group[0] if invert else group[-1]))
        if best_cost is None or cost < best_cost:
            best_cost, outputs = cost, {tuple(edges)}
        elif cost == best_cost:
            outputs.add(tuple(edges))
    return outputs


class CLI:
    def __init__(self, executable, extension):
        self.executable = executable
        self.extension = str(Path(extension).resolve())

    def run(self, sql, load=True, database=":memory:"):
        prefix = ".bail on\n"
        if load:
            prefix += "LOAD spatial;\nLOAD " + sql_string(self.extension) + ";\n"
        result = subprocess.run(
            [self.executable, "-json", "-batch", "-unsigned", str(database)],
            input=prefix + sql + "\n", text=True, capture_output=True, timeout=180,
            env={key: value for key, value in os.environ.items() if key != "CODEX_CI"},
        )
        return result

    @staticmethod
    def diagnostic(result, sql):
        return (f"SQL:\n{sql}\nexit code: {result.returncode}\n"
                f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")

    def query(self, sql, load=True, database=":memory:"):
        result = self.run(sql, load=load, database=database)
        if result.returncode:
            raise AssertionError(self.diagnostic(result, sql))
        try:
            rows = json.loads(result.stdout) if result.stdout.strip() else []
            # The 1.4 CLI emits nested SQL lists as JSON strings; 2.0 emits arrays.
            for row in rows:
                for key in ("bins", "defaults", "upper_edges", "lower_edges"):
                    if isinstance(row.get(key), str):
                        row[key] = json.loads(row[key])
            return rows
        except json.JSONDecodeError as error:
            raise AssertionError(self.diagnostic(result, sql)) from error


class IntegrationTests(unittest.TestCase):
    cli = None

    def assert_error(self, sql, pattern, load=True):
        result = self.cli.run(sql, load=load)
        self.assertNotEqual(result.returncode, 0, self.cli.diagnostic(result, sql))
        self.assertRegex(result.stderr, re.compile(pattern, re.IGNORECASE),
                         self.cli.diagnostic(result, sql))

    def assert_xy(self, actual, expected, tolerance):
        for value, want in zip(actual, expected):
            if math.isnan(want):
                self.assertTrue(math.isnan(value), (actual, expected))
            else:
                self.assertAlmostEqual(value, want, delta=tolerance)

    def test_load_dependency_and_persistent_database_reopen(self):
        self.assert_error("LOAD " + sql_string(self.cli.extension) + ";", "spatial", load=False)
        with tempfile.TemporaryDirectory(prefix="eviltransform-sql-") as directory:
            database = Path(directory) / "points.duckdb"
            self.cli.query("CREATE TABLE points AS SELECT ST_Point(120, 30) AS geom;",
                           database=database)
            row = self.cli.query("SELECT ST_X(ST_EvilTransform(geom, 'WGS84', 'GCJ02')) AS x "
                                 "FROM points;", database=database)[0]
            self.assertAlmostEqual(row["x"], 120.00466044559735, delta=1e-6)

    def test_six_coordinate_paths_and_original_pg_point(self):
        fixtures = ",".join(
            f"({i},{sql_string(src)},{sql_string(dst)},{xy[0]},{xy[1]})"
            for i, (src, dst, xy, _, _) in enumerate(PATHS)
        )
        rows = self.cli.query(f"""
            SELECT id, ST_X(g) AS x, ST_Y(g) AS y
            FROM (SELECT id, ST_EvilTransform(ST_Point(x,y),src,dst) AS g
                  FROM (VALUES {fixtures}) AS v(id,src,dst,x,y)) ORDER BY id;
        """)
        self.assertEqual(len(rows), len(PATHS))
        for row, (_, _, _, expected, tolerance) in zip(rows, PATHS):
            with self.subTest(path=row["id"]):
                self.assert_xy((row["x"], row["y"]), expected, tolerance)
        row = self.cli.query("SELECT ST_X(ST_EvilTransform(ST_Point(120,30),4326,990001)) AS x;")[0]
        self.assertAlmostEqual(row["x"], 120.00466044559735, delta=1e-6)

    def test_aliases_standard_projection_and_native_crs_boundary(self):
        row = self.cli.query("""
            SELECT
              ST_AsWKB(ST_EvilTransform(ST_Point(120,30),'WGS-84','3857')) =
                ST_AsWKB(ST_Transform(ST_Point(120,30),'EPSG:4326','EPSG:3857',always_xy:=true)) AS projected,
              ST_AsWKB(ST_EvilTransform(ST_Point(120,30),'CGCS2000','GCJ-02')) =
                ST_AsWKB(ST_EvilTransform(ST_Transform(ST_Point(120,30),'EPSG:4490','EPSG:4326',always_xy:=true),'WGS84','GCJ02')) AS cgcs,
              ST_AsWKB(ST_EvilTransform(ST_Point(120,30),'EPSG:4490','EPSG:3857')) =
                ST_AsWKB(ST_Transform(ST_Point(120,30),'EPSG:4490','EPSG:3857',always_xy:=true)) AS cgcs_projection,
              ST_AsWKB(ST_EvilTransform(ST_Point(120,30),'  wgs84  ','bd-09')) =
                ST_AsWKB(ST_EvilTransform(ST_Point(120,30),'EPSG:4326','EPSG:990002')) AS aliases;
        """)[0]
        self.assertTrue(all(row.values()), row)
        for crs in ("CGCS2000", "EPSG:3857"):
            with self.subTest(native_crs=crs):
                self.assert_error(f"SELECT EvilTransformWKB(ST_AsWKB(ST_Point(120,30)),{sql_string(crs)},'GCJ02');",
                                  "WGS84.*GCJ02.*BD09|supports")
        self.assert_error("SELECT ST_EvilTransform(ST_Point(120,30),'WGS84','nonsense:bad');", "CRS|projection|transform")

    def test_geometry_families_endian_dimensions_and_empty(self):
        inside = (BEIJING, GCJ_BEIJING)
        outside = ((-120.0, 30.0), (-120.0, 30.0))
        line = WKB.sequence(2, [inside, outside])
        polygon = WKB.polygon([[inside, outside, inside, inside]])
        nan = float("nan")
        fixtures = {
            "point_little": WKB.point(),
            "point_big": WKB.point(endian=">"),
            "line": line,
            "polygon": polygon,
            "multipoint": WKB.collection(4, [WKB.point(), WKB.point(*outside, endian=">")]),
            "multiline": WKB.collection(5, [line]),
            "multipolygon": WKB.collection(6, [polygon]),
            "geometrycollection": WKB.collection(7, [WKB.point(), line, WKB.collection(7, [polygon])]),
            "point_empty": WKB.point((nan, nan), (nan, nan)),
            "line_empty": WKB.sequence(2, []),
            "polygon_empty": WKB.polygon([]),
            "collection_empty": WKB.collection(7, []),
            "point_z": WKB.point(kind=1001, extra=struct.pack("<Q", 0x7FF0000000000001)),
            "point_m": WKB.point(kind=2001, endian=">", extra=struct.pack(">d", 9876.5)),
            "point_zm": WKB.point(kind=3001, extra=struct.pack("<dd", -12.5, 9876.5)),
            "ewkb_zm_srid": WKB.point(kind=0xE0000001, extra=struct.pack("<dd", -12.5, 9876.5), srid=4326),
        }
        values = ",".join(f"({sql_string(name)},from_hex('{fixture.raw.hex()}'))"
                          for name, fixture in fixtures.items())
        rows = self.cli.query(f"SELECT name,hex(EvilTransformWKB(raw,'WGS84','GCJ02')) AS h "
                              f"FROM (VALUES {values}) AS v(name,raw);")
        self.assertEqual(len(rows), len(fixtures))
        for row in rows:
            with self.subTest(geometry=row["name"]):
                fixture = fixtures[row["name"]]
                actual = bytes.fromhex(row["h"])
                structural_expected = bytearray(fixture.raw)
                self.assertEqual(len(actual), len(structural_expected))
                for offset, endian, expected in fixture.points:
                    self.assert_xy(struct.unpack_from(endian + "dd", actual, offset), expected, 1e-6)
                    # Everything besides XY must survive byte for byte, including
                    # endian, geometry structure, Z/M and unusual NaN payloads.
                    structural_expected[offset:offset + 16] = actual[offset:offset + 16]
                if row["name"] == "ewkb_zm_srid":
                    struct.pack_into("<I", structural_expected, 5, 990001)
                self.assertEqual(actual, bytes(structural_expected))
        row = self.cli.query("""
            SELECT ST_IsEmpty(ST_EvilTransform(ST_GeomFromText('LINESTRING EMPTY'),'WGS84','GCJ02')) AS empty,
                   ST_EvilTransform(NULL::GEOMETRY,'WGS84','GCJ02') IS NULL AS geom_null,
                   ST_EvilTransform(ST_Point(1,2),NULL::VARCHAR,'GCJ02') IS NULL AS src_null,
                   ST_EvilTransform(ST_Point(1,2),NULL::VARCHAR,'') IS NULL AS null_before_invalid_target,
                   ST_EvilTransform(ST_Point(1,2),'',NULL::VARCHAR) IS NULL AS null_before_invalid_source,
                   EvilTransformWKB(NULL::BLOB,'WGS84','GCJ02') IS NULL AS blob_null;
        """)[0]
        self.assertTrue(all(row.values()), row)

    def test_wkb_rejects_corruption_at_the_public_boundary(self):
        header = WKB.header
        nested = WKB.collection(7, []).raw
        for _ in range(129):
            nested = header(7) + struct.pack("<I", 1) + nested
        cases = {
            "header": (b"\x01\x01", "truncated|header"),
            "byte_order": (b"\x02" + struct.pack("<I", 1) + b"\0" * 16, "byte.order"),
            "geometry_type": (header(99), "type|unsupported"),
            "point_payload": (header(1) + b"\0" * 8, "truncated|payload"),
            "count": (header(2) + struct.pack("<I", 0xFFFFFFFF), "count|payload"),
            "member": (header(4) + struct.pack("<I", 1) + WKB.sequence(2, []).raw, "member|type"),
            "nesting": (nested, "depth|nest"),
            "trailing": (WKB.point().raw + b"\0", "trailing"),
        }
        for name, (raw, pattern) in cases.items():
            with self.subTest(corruption=name):
                self.assert_error(f"SELECT EvilTransformWKB(from_hex('{raw.hex()}'),'WGS84','GCJ02');", pattern)

    def test_vectorized_varying_crs_filtered_rows_and_nulls(self):
        fixtures = ",".join(
            f"({i},{sql_string(src)},{sql_string(dst)},{xy[0]},{xy[1]},'{WKB.point(xy, expected).raw.hex()}')"
            for i, (src, dst, xy, expected, _) in enumerate(PATHS)
        )
        rows = self.cli.query(f"""
            WITH source AS (
              SELECT i, CASE WHEN i%13=0 THEN NULL ELSE from_hex(h) END AS raw,
                     CASE WHEN i%13=0 THEN NULL ELSE ST_Point(x,y) END AS geom,
                     CASE WHEN i%17=0 THEN NULL ELSE src END AS src,
                     CASE WHEN i%19=0 THEN NULL ELSE dst END AS dst
              FROM range(10007) AS r(i)
              JOIN (VALUES {fixtures}) AS f(id,src,dst,x,y,h) ON i%6=id
              WHERE i%7<>0
            ), result AS (
              SELECT i, hex(EvilTransformWKB(raw,src,dst)) AS h,
                     ST_EvilTransform(geom,src,dst) AS g FROM source
            ) SELECT i,h,ST_X(g) AS x,ST_Y(g) AS y FROM result ORDER BY i;
        """)
        self.assertEqual(len(rows), sum(i % 7 != 0 for i in range(10007)))
        for row in rows:
            i = row["i"]
            if any(i % period == 0 for period in (13, 17, 19)):
                self.assertIsNone(row["h"], i)
                self.assertIsNone(row["x"], i)
                self.assertIsNone(row["y"], i)
            else:
                *_, expected, tolerance = PATHS[i % len(PATHS)]
                self.assert_xy(struct.unpack_from("<dd", bytes.fromhex(row["h"]), 5), expected, tolerance)
                self.assert_xy((row["x"], row["y"]), expected, tolerance)

    def test_jenks_lists_against_exhaustive_weighted_partitions(self):
        cases = [
            ([1, 1, 2, 3, 10, 11, 11, 12, 30], 3),
            ([9, -4, -4, -3, 0, 1, 1, 12], 2),
            ([0.25, 0.25, 0.5, 1.5, 9, 9, 10], 3),
            ([0, 1, 2, 3], 2),
            ([5, 5, 5], 4),
            ([3, 1, 2, 1], 8),
            ([1, None, 10, 11, None], 1),
            ([1e200, 1.01e200, 1.02e200, 4e200, 4.01e200], 2),
            ([-1e308, -9e307, 9e307, 1e308], 2),
            ([], 2),
            ([None, None], 2),
        ]
        fixtures = []
        expected = {}
        for i, (values, k) in enumerate(cases):
            for invert in (False, True):
                ident = i * 2 + int(invert)
                literal = "[" + ",".join("NULL" if value is None else repr(value) for value in values) + "]::DOUBLE[]"
                fixtures.append(f"({ident},{literal},{k},{str(invert).lower()})")
                expected[ident] = optimal_jenks(Counter(value for value in values if value is not None), k, invert)
        rows = self.cli.query("SELECT id,ST_JenksBins(vals,k,invert) AS bins FROM (VALUES "
                              + ",".join(fixtures) + ") AS v(id,vals,k,invert) ORDER BY id;")
        self.assertEqual(len(rows), len(expected))
        for row in rows:
            actual = None if row["bins"] is None else tuple(row["bins"])
            self.assertIn(actual, expected[row["id"]], row)
        row = self.cli.query("SELECT ST_JenksBins([1,2,10,11],2) AS defaults, "
                             "ST_JenksBins(NULL::DOUBLE[],2) AS null_list, "
                             "ST_JenksBins([1,2],NULL::INTEGER) AS null_k, "
                             "ST_JenksBins([1,2],2,NULL::BOOLEAN) AS null_invert;")[0]
        self.assertEqual(row["defaults"], [2.0, 11.0])
        self.assertIsNone(row["null_list"])
        self.assertIsNone(row["null_k"])
        self.assertIsNone(row["null_invert"])

    def test_jenks_parallel_aggregate_grouping_nulls_and_combine(self):
        pattern = [1, 1, 2, 10, 11, 11, 12, 30]
        size = 1048576
        counts = {group: Counter() for group in (0, 1, 2, 9, None)}
        for i in range(size):
            if i % 97:
                value = pattern[i % len(pattern)]
                counts[(i // 8) % 3][value] += 1
                counts[None][value] += 1
        rows = self.cli.query(f"""
            SET threads=4;
            CREATE TABLE samples AS
              SELECT ((i//8)%3)::INTEGER AS grp,
                     CASE WHEN i%97=0 THEN NULL ELSE [1,1,2,10,11,11,12,30][i%8+1] END::DOUBLE AS value
              FROM range({size}) AS r(i);
            INSERT INTO samples VALUES (9,NULL);
            SELECT grp, ST_JenksBinsAgg(value,2) AS upper_edges,
                        ST_JenksBinsAgg(value,2,true) AS lower_edges
            FROM samples GROUP BY GROUPING SETS ((grp),());
        """)
        self.assertEqual(len(rows), len(counts))
        for row in rows:
            for column, invert in (("upper_edges", False), ("lower_edges", True)):
                actual = None if row[column] is None else tuple(row[column])
                self.assertIn(actual, optimal_jenks(counts[row["grp"]], 2, invert), row)
        row = self.cli.query("SELECT ST_JenksBinsAgg(value,2) AS bins "
                             "FROM (SELECT 1::DOUBLE AS value WHERE false);")[0]
        self.assertIsNone(row["bins"])
        rows = self.cli.query("SELECT grp,ST_JenksBinsAgg(value,k) AS bins FROM "
                              "(VALUES (0,1.0,1),(0,9.0,1),(1,1.0,2),(1,9.0,2)) AS v(grp,value,k) "
                              "GROUP BY grp ORDER BY grp;")
        self.assertEqual(rows, [{"grp": 0, "bins": [9.0]}, {"grp": 1, "bins": [1.0, 9.0]}])
        row = self.cli.query("SELECT ST_JenksBinsAgg(value,k) AS bins FROM "
                             "(VALUES (1.0,2),(9.0,NULL::INTEGER)) AS v(value,k);")[0]
        self.assertIsNone(row["bins"])
        row = self.cli.query("SELECT ST_JenksBinsAgg(value,2,invert) AS bins FROM "
                             "(VALUES (1.0,false),(9.0,NULL::BOOLEAN)) AS v(value,invert);")[0]
        self.assertIsNone(row["bins"])

    def test_jenks_rejects_invalid_values_and_inconsistent_group_parameters(self):
        cases = [
            ("SELECT ST_JenksBins([1,2],0);", "break|class|positive|least|greater"),
            ("SELECT ST_JenksBinsAgg(value,-1) FROM (VALUES (1.0)) AS v(value);", "break|class|positive|least|greater"),
            ("SELECT ST_JenksBins([1,'NaN'::DOUBLE],2);", "finite|NaN"),
            ("SELECT ST_JenksBinsAgg(value,2) FROM (VALUES (1.0),('Infinity'::DOUBLE)) AS v(value);", "finite|infinity"),
            ("SELECT ST_JenksBinsAgg(value,k) FROM (VALUES (1.0,1),(2.0,2)) AS v(value,k);", "constant|same|differ|inconsistent|identical"),
            ("SELECT ST_JenksBinsAgg(value,2,invert) FROM (VALUES (1.0,false),(2.0,true)) AS v(value,invert);", "constant|same|differ|inconsistent|identical"),
            ("SELECT ST_JenksBinsAgg(value,k) FROM (VALUES (1.0,1),(NULL::DOUBLE,2)) AS v(value,k);", "identical|constant|same|inconsistent"),
        ]
        for sql, pattern in cases:
            with self.subTest(sql=sql):
                self.assert_error(sql, pattern)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duckdb", default="duckdb", help="DuckDB CLI executable")
    parser.add_argument("--extension", required=True, help="eviltransform.duckdb_extension path")
    arguments = parser.parse_args()
    if not Path(arguments.extension).is_file():
        parser.error("extension does not exist: " + arguments.extension)
    cli = CLI(arguments.duckdb, arguments.extension)
    # Install once explicitly. Subsequent connections only LOAD the dependency.
    result = cli.run("INSTALL spatial;", load=False)
    if result.returncode:
        print(cli.diagnostic(result, "INSTALL spatial;"), file=sys.stderr)
        return 1
    IntegrationTests.cli = cli
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(IntegrationTests)
    return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
