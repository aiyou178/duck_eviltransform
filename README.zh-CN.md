# duck_eviltransform

[English](README.md)

初始版本：**0.1**。[构建与测试](https://github.com/aiyou178/duck_eviltransform/actions/workflows/ci.yml)。

将 [pg_eviltransform](https://github.com/aiyou178/pg_eviltransform) `0.0.7`
（`434b8fb97aee24931e56c8a4686dea292e63df8c`）迁移为 DuckDB 原生扩展，支持
WGS84／GCJ02／BD09 坐标转换及精确 Jenks 自然断点分箱。普通投影转换委托给
DuckDB 的 `spatial` 扩展及其内置 PROJ 数据库。

## 是否已有同类扩展

截至 **2026-10-22**，检查 DuckDB 的
[官方 core 目录](https://duckdb.org/docs/lts/core_extensions/overview)、
[community 目录](https://duckdb.org/community_extensions/list_of_extensions)、
[spatial 源码](https://github.com/duckdb/duckdb-spatial)，未发现等价的 GCJ02／BD09
转换扩展，也未找到 Jenks 自然断点分箱函数。
现有 `spatial.ST_Transform` 支持 EPSG:4326、EPSG:3857、EPSG:4490 等标准坐标系，
不能直接完成 GCJ02／BD09 偏移算法。本结论限于已检查的官方目录和源码。

## 构建、加载

依赖 C++17 编译器、CMake 3.16+、Python 3 和 Make。不需要 Rust、PostgreSQL、PostGIS，
也不需要编译 DuckDB 源码；C API 头文件已经保存在仓库中。

```bash
git clone --recurse-submodules https://github.com/aiyou178/duck_eviltransform.git
cd duck_eviltransform
make configure
make release
duckdb -unsigned
```

`make configure` 在项目内虚拟环境安装 DuckDB Python 客户端和官方 SQLLogicTest
测试工具。只编译、不安装这些测试依赖时可使用
`cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release && cmake --build build/release`。

```sql
INSTALL spatial;
LOAD spatial;
LOAD '/absolute/path/to/duck_eviltransform/build/release/eviltransform.duckdb_extension';

SELECT ST_AsText(ST_EvilTransform(ST_Point(120,30), 'WGS84', 'GCJ02'));
```

将加载路径替换为构建产物的绝对路径（macOS 2.0 CLI 的 hardened 配置有此要求）。
本地二进制未签名，需要 `-unsigned`；Python 可配置
`duckdb.connect(config={'allow_unsigned_extensions': True})`。
尚未发布到 DuckDB community 仓库，不能直接 `INSTALL eviltransform FROM community`。

使用稳定 C 扩展 ABI `v1.2.0`，CI 配置检查 DuckDB `v1.4.0` 和 `v2.0.0`。
不同平台仍需分别编译；必要时将 `PRAGMA platform` 的结果传入
`DUCKDB_PLATFORM=linux_amd64 make configure release`。

先加载 `spatial`，再加载 `eviltransform`。加载过程注册原生函数，并在数据库
`main` schema 中创建／替换 `ST_EvilTransform` 宏，因此初始化需要数据库可写。
每个新数据库进程均需要加载两个扩展。

## 坐标转换

```sql
ST_EvilTransform(geometry, from_crs, to_crs) -> GEOMETRY
EvilTransformWKB(blob, from_crs VARCHAR, to_crs VARCHAR) -> BLOB
```

主接口始终明确传入源、目标坐标系，即使输入已有 CRS 元数据也一样。
两侧均接受文本和整数 EPSG 编号；坐标始终为 **X＝经度／东向，Y＝纬度／北向**，
标准投影调用固定使用 `always_xy := true`。

| 坐标系 | 可用名称 |
|---|---|
| WGS84 | `WGS84`、`WGS-84`、`4326`、`EPSG:4326` |
| GCJ02 | `GCJ02`、`GCJ-02`、`990001`、`EPSG:990001` |
| BD09 | `BD09`、`BD-09`、`990002`、`EPSG:990002` |
| 地理 CGCS2000 | `CGCS2000`、`CGCS-2000`、`4490`、`EPSG:4490` |
| 其他 | `spatial` 接受的 EPSG 编号、authority identifier、PROJ CRS 定义 |

名称别名忽略大小写、去除两端空白。CGCS2000 别名仅表示地理坐标 EPSG:4490，
投影分带需使用具体 EPSG 编号。`990001/990002` 是本项目别名，不是官方 EPSG 登记。

```sql
SELECT ST_EvilTransform(ST_Point(120,30), 4326, 990001);
SELECT ST_EvilTransform(ST_Point(120,30), 'WGS84', 'BD09');
SELECT ST_EvilTransform(ST_Point(120.011070620552,30.0038830555128), 'BD09', 3857);
SELECT ST_EvilTransform(ST_Point(120,30), 'CGCS2000', 'GCJ02');
SELECT ST_EvilTransform(ST_Point(120,30), 4326, 3857);
SELECT ST_EvilTransform(ST_GeomFromText('LINESTRING(120 30,121 31)'), 4326, 'GCJ02');
```

支持点、线、多边形、Multi*、嵌套 GeometryCollection、空几何及 NULL。
WKB 接口仅支持 WGS84／GCJ02／BD09；支持双字节序、ISO Z/M/ZM、EWKB，
保留 Z/M 和空点 NaN 原始字节，更新已有 EWKB SRID，拒绝截断、错误类型、
尾随字节及超过 128 层嵌套的输入。几何接口通过 `spatial` 的 WKB 转换保存内容，
不为输出附加自定义 CRS 元数据，存储时请同时保存坐标系名称。

保留原插件的地球半径 `6378137`、GCJ02 近似逆变换和中国矩形边界判断
（经度 `72.004..137.8347`、纬度 `0.8293..55.8271`），边界外自定义公式不偏移。
往返转换有近似误差；标准投影精度和 Z 处理由 `spatial`／PROJ 决定。

## Jenks 自然断点

```sql
SELECT ST_JenksBins([1,2,NULL,3,10,11,12], 2);      -- [3,12]
SELECT ST_JenksBins([1,2,3,10,11,12], 2, true);     -- [1,10]
SELECT ST_JenksBinsAgg(value, 2)
FROM (VALUES (1),(2),(3),(10),(11),(12)) AS t(value);
```

`ST_JenksBins(DOUBLE[], INTEGER[, BOOLEAN])` 是列表接口，
`ST_JenksBinsAgg(DOUBLE, INTEGER[, BOOLEAN])` 是真正的流式聚合，支持并行合并。
输入转为 DOUBLE；重复值按频次计算权重，NULL 元素／值忽略，无有效值返回 NULL。
类别数小于 1、NaN、无穷值报错；不同值数量不大于类别数时返回有序去重值。
默认返回每类上界，第三参数为 true 时返回下界。

同一聚合组的类别数和第三参数必须一致（值为 NULL 的行也检查），参数为 NULL
则该组返回 NULL。算法保存不同值及频次，内存随不同值数量和类别数增长，
使用加权动态规划、分治优化、归一化 long double 计算。

## 与 PostgreSQL 版本的差异

- 采用三个参数明确源、目标 CRS，不模拟从 PostGIS SRID 推断源的两参数形式。
- DuckDB 中列表函数和聚合函数分开命名为 `ST_JenksBins`／`ST_JenksBinsAgg`。
- pgrx、PostGIS GSERIALIZED、librttopo、PG 包和升级 SQL 改为 DuckDB C API
  与二进制 WKB 遍历；未移植仅供比较的 PL/pgSQL 正则基准。

## 验证

```bash
make test_release
make test_integration
python3 scripts/fetch_duckdb.py v2.0.0 .deps/duckdb-v2
make test_integration DUCKDB=.deps/duckdb-v2/duckdb
```

测试使用真实 CLI，先安装 `spatial`，检查参考点、几何／WKB 结构、投影委托、
向量化和 NULL／异常路径、Jenks 精确分组与聚合生命周期。
CI 配置覆盖 Linux、macOS、Windows；配置文件本身不代表远程 CI 已通过。

2026-10-22 本地实测：同一份 macOS ARM64 release 二进制分别在 DuckDB 1.4.0
（`b8a06e4a22`）和 2.0.0（`d54cbbe3df`）通过全部 9 组集成测试，包含
1,048,576 行并行 Jenks 聚合。Linux／macOS／Windows 的当前 CI 状态及额外的
community 构建协议检查见上方工作流链接。

## 许可

MIT。算法来自 `pg_eviltransform`，坐标公式沿用其对
[googollee/eviltransform](https://github.com/googollee/eviltransform) 的致谢。
DuckDB C API 头文件及元数据格式来自 DuckDB（MIT），详见 [THIRD_PARTY.md](THIRD_PARTY.md)。
