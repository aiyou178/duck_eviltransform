#include "duckdb_extension.h"
#include "coord.hpp"
#include "wkb.hpp"
#include "jenks.hpp"
#include "sql.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>

DUCKDB_EXTENSION_EXTERN

namespace eviltransform {

static std::string Text(duckdb_vector vector, idx_t row) {
    auto strings = static_cast<duckdb_string_t *>(duckdb_vector_get_data(vector));
    return std::string(duckdb_string_t_data(&strings[row]), duckdb_string_t_length(strings[row]));
}

static std::string NormalizeCrs(std::string input) {
    auto first = input.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) throw std::invalid_argument("CRS must not be empty");
    input = input.substr(first, input.find_last_not_of(" \t\r\n") - first + 1);
    std::string upper = input;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return std::toupper(c); });
    if (upper == "GCJ02" || upper == "GCJ-02" || upper == "990001" || upper == "EPSG:990001") return "GCJ02";
    if (upper == "BD09" || upper == "BD-09" || upper == "990002" || upper == "EPSG:990002") return "BD09";
    if (upper == "WGS84" || upper == "WGS-84" || upper == "4326" || upper == "EPSG:4326") return "EPSG:4326";
    if (upper == "CGCS2000" || upper == "CGCS-2000" || upper == "4490" || upper == "EPSG:4490") return "EPSG:4490";
    if (std::all_of(input.begin(), input.end(), [](unsigned char c) { return std::isdigit(c); })) return "EPSG:" + input;
    if (upper.rfind("EPSG:", 0) == 0) return upper;
    return input; // PROJ strings and authority identifiers are interpreted by spatial.
}

static Crs CustomCrs(const std::string &input) {
    const auto crs = NormalizeCrs(input);
    if (crs == "EPSG:4326") return Crs::Wgs84;
    if (crs == "GCJ02") return Crs::Gcj02;
    if (crs == "BD09") return Crs::Bd09;
    throw std::invalid_argument("EvilTransformWKB supports WGS84, GCJ02 and BD09; use ST_EvilTransform for other CRSs");
}

static void NormalizeFunction(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
    try {
        const auto count = duckdb_data_chunk_get_size(input);
        const auto source = duckdb_data_chunk_get_vector(input, 0);
        for (idx_t row = 0; row < count; row++) {
            if (!duckdb_validity_row_is_valid(duckdb_vector_get_validity(source), row)) {
                duckdb_vector_ensure_validity_writable(output);
                duckdb_validity_set_row_invalid(duckdb_vector_get_validity(output), row);
                continue;
            }
            const auto result = NormalizeCrs(Text(source, row));
            duckdb_vector_assign_string_element_len(output, row, result.data(), result.size());
        }
    } catch (const std::exception &error) { duckdb_scalar_function_set_error(info, error.what()); }
    catch (...) { duckdb_scalar_function_set_error(info, "CRS normalization failed"); }
}

static void WkbFunction(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
    try {
        const auto count = duckdb_data_chunk_get_size(input);
        const auto blob = duckdb_data_chunk_get_vector(input, 0);
        const auto from = duckdb_data_chunk_get_vector(input, 1);
        const auto to = duckdb_data_chunk_get_vector(input, 2);
        for (idx_t row = 0; row < count; row++) {
            if (!duckdb_validity_row_is_valid(duckdb_vector_get_validity(blob), row) ||
                !duckdb_validity_row_is_valid(duckdb_vector_get_validity(from), row) ||
                !duckdb_validity_row_is_valid(duckdb_vector_get_validity(to), row)) {
                duckdb_vector_ensure_validity_writable(output);
                duckdb_validity_set_row_invalid(duckdb_vector_get_validity(output), row);
                continue;
            }
            const auto bytes = Text(blob, row);
            const auto result = TransformWKB(bytes.data(), bytes.size(), CustomCrs(Text(from, row)), CustomCrs(Text(to, row)));
            duckdb_vector_assign_string_element_len(output, row, result.data(), result.size());
        }
    } catch (const std::exception &error) { duckdb_scalar_function_set_error(info, error.what()); }
    catch (...) { duckdb_scalar_function_set_error(info, "WKB coordinate transformation failed"); }
}

static bool RegisterFunction(duckdb_connection connection, const char *name,
                             duckdb_scalar_function_t callback, bool wkb) {
    auto function = duckdb_create_scalar_function();
    duckdb_scalar_function_set_name(function, name);
    auto varchar_type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
    auto blob_type = duckdb_create_logical_type(DUCKDB_TYPE_BLOB);
    duckdb_scalar_function_add_parameter(function, wkb ? blob_type : varchar_type);
    if (wkb) {
        duckdb_scalar_function_add_parameter(function, varchar_type);
        duckdb_scalar_function_add_parameter(function, varchar_type);
    }
    duckdb_scalar_function_set_return_type(function, wkb ? blob_type : varchar_type);
    duckdb_scalar_function_set_function(function, callback);
    const auto status = duckdb_register_scalar_function(connection, function);
    duckdb_destroy_scalar_function(&function);
    duckdb_destroy_logical_type(&varchar_type);
    duckdb_destroy_logical_type(&blob_type);
    return status == DuckDBSuccess;
}

static void Query(duckdb_connection connection, const char *sql) {
    duckdb_result result;
    if (duckdb_query(connection, sql, &result) == DuckDBError) {
        const std::string message = duckdb_result_error(&result);
        duckdb_destroy_result(&result);
        throw std::runtime_error(message);
    }
    duckdb_destroy_result(&result);
}

} // namespace eviltransform

DUCKDB_EXTENSION_ENTRYPOINT(duckdb_connection connection, duckdb_extension_info info, struct duckdb_extension_access *access) {
    try {
        // Check dependencies before registering anything, so a failed load can be retried.
        eviltransform::Query(connection, "SELECT ST_AsWKB(ST_Point(0, 0)), ST_Transform(ST_Point(0, 0), 'EPSG:4326', 'EPSG:4326', always_xy := true)");
        if (!eviltransform::RegisterFunction(connection, "__eviltransform_crs", eviltransform::NormalizeFunction, false) ||
            !eviltransform::RegisterFunction(connection, "EvilTransformWKB", eviltransform::WkbFunction, true) ||
            !eviltransform::RegisterJenks(connection)) {
            throw std::runtime_error("Failed to register eviltransform functions");
        }
        eviltransform::Query(connection, EVILTRANSFORM_SQL);
        return true;
    } catch (const std::exception &error) {
        const std::string message = std::string("eviltransform initialization failed; run INSTALL spatial; LOAD spatial first. ") + error.what();
        access->set_error(info, message.c_str());
        return false;
    } catch (...) {
        access->set_error(info, "eviltransform initialization failed");
        return false;
    }
}
