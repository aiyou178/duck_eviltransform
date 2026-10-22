-- Explicit source and destination: DuckDB does not store the PG custom SRIDs.
-- Longitude/easting is always X; latitude/northing is always Y.
CREATE OR REPLACE MACRO ST_EvilTransform(geom, from_crs, to_crs) AS (
    SELECT CASE
        WHEN geom IS NULL OR src IS NULL OR dst IS NULL THEN NULL
        WHEN src = dst THEN geom
        WHEN src IN ('GCJ02', 'BD09') AND dst IN ('GCJ02', 'BD09') THEN
            ST_GeomFromWKB(EvilTransformWKB(ST_AsWKB(geom), src, dst))
        WHEN src IN ('GCJ02', 'BD09') THEN
            ST_Transform(
                ST_GeomFromWKB(EvilTransformWKB(ST_AsWKB(geom), src, 'EPSG:4326')),
                'EPSG:4326', dst, always_xy := true)
        WHEN dst IN ('GCJ02', 'BD09') THEN
            ST_GeomFromWKB(EvilTransformWKB(
                ST_AsWKB(ST_Transform(geom, src, 'EPSG:4326', always_xy := true)),
                'EPSG:4326', dst))
        ELSE ST_Transform(geom, src, dst, always_xy := true)
    END
    FROM (SELECT __eviltransform_crs(
                    CASE WHEN geom IS NULL OR from_crs IS NULL OR to_crs IS NULL
                         THEN NULL ELSE CAST(from_crs AS VARCHAR) END) AS src,
                 __eviltransform_crs(
                    CASE WHEN geom IS NULL OR from_crs IS NULL OR to_crs IS NULL
                         THEN NULL ELSE CAST(to_crs AS VARCHAR) END) AS dst)
);
