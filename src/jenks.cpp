#include "jenks.hpp"

#include "duckdb_extension.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

DUCKDB_EXTENSION_EXTERN

namespace eviltransform {
namespace {

using WeightedValue = std::pair<double, uint64_t>;

class Counts {
public:
    void Add(double value, uint64_t count = 1) {
        if (!std::isfinite(value)) {
            throw std::invalid_argument("Jenks input must contain only finite DOUBLE values");
        }
        // SQL regards -0 and +0 as the same value.
        if (value == 0.0) {
            value = 0.0;
        }
        uint64_t key;
        std::memcpy(&key, &value, sizeof(key));
        if (count > std::numeric_limits<uint64_t>::max() - total) {
            throw std::overflow_error("too many Jenks input values");
        }
        auto entry = counts.emplace(key, 0).first;
        if (count > std::numeric_limits<uint64_t>::max() - entry->second) {
            throw std::overflow_error("too many Jenks input values");
        }
        entry->second += count;
        total += count;
    }

    void Merge(const Counts &other) {
        for (const auto &entry : other.counts) {
            double value;
            std::memcpy(&value, &entry.first, sizeof(value));
            Add(value, entry.second);
        }
    }

    bool Empty() const {
        return total == 0;
    }

    std::vector<WeightedValue> Sorted() const {
        std::vector<WeightedValue> result;
        result.reserve(counts.size());
        for (const auto &entry : counts) {
            double value;
            std::memcpy(&value, &entry.first, sizeof(value));
            result.emplace_back(value, entry.second);
        }
        std::sort(result.begin(), result.end(), [](const WeightedValue &left, const WeightedValue &right) {
            return left.first < right.first;
        });
        return result;
    }

private:
    std::unordered_map<uint64_t, uint64_t> counts;
    uint64_t total = 0;
};

struct Prefixes {
    std::vector<long double> original;
    std::vector<long double> weights;
    std::vector<long double> weight;
    std::vector<long double> sum;
    std::vector<long double> sum_sq;
    long double scale;

    explicit Prefixes(const std::vector<WeightedValue> &input)
        : original(input.size()), weights(input.size()), weight(input.size() + 1, 0),
          sum(input.size() + 1, 0), sum_sq(input.size() + 1, 0), scale(0) {
        const long double minimum = input.front().first;
        const long double maximum = input.back().first;
        scale = std::max(std::fabs(minimum), std::fabs(maximum));
        // Translation is safe even where long double has the DOUBLE exponent
        // range: same-sign subtraction cannot overflow; crossing zero uses 0.
        const long double origin = minimum > 0 ? minimum : (maximum < 0 ? maximum : 0);
        for (size_t i = 0; i < input.size(); ++i) {
            const long double x = scale == 0 ? 0 : (static_cast<long double>(input[i].first) - origin) / scale;
            const long double w = static_cast<long double>(input[i].second);
            original[i] = input[i].first;
            weights[i] = w;
            weight[i + 1] = weight[i] + w;
            sum[i + 1] = sum[i] + x * w;
            sum_sq[i + 1] = sum_sq[i] + x * x * w;
        }
    }

    long double Variance(size_t start, size_t end) const {
        if (end - start <= 1) {
            return 0;
        }
        const long double w = weight[end] - weight[start];
        const long double s = sum[end] - sum[start];
        const long double ss = sum_sq[end] - sum_sq[start];
        const long double correction = s * s / w;
        const long double cost = ss - correction;
        // Prefix subtraction is fast, but can lose close clusters beside much
        // larger values. Recompute those ranges with weighted Welford updates.
        const long double uncertainty = 64 * std::numeric_limits<long double>::epsilon() *
                                        (std::fabs(sum_sq[end]) + std::fabs(sum_sq[start]) +
                                         std::fabs(correction));
        if (cost > uncertainty) {
            return cost;
        }
        long double running_weight = 0;
        long double mean = 0;
        long double m2 = 0;
        const long double origin = original[start];
        for (size_t i = start; i < end; ++i) {
            // Recenter from the original doubles so a close cluster does not
            // inherit rounding from translation against the global origin.
            const long double x = (std::signbit(original[i]) == std::signbit(origin))
                                      ? (original[i] - origin) / scale
                                      : original[i] / scale - origin / scale;
            const long double next_weight = running_weight + weights[i];
            const long double delta = x - mean;
            const long double next_mean = mean + delta * weights[i] / next_weight;
            m2 += weights[i] * delta * (x - next_mean);
            mean = next_mean;
            running_weight = next_weight;
        }
        return std::max(static_cast<long double>(0), m2);
    }
};

void ComputeRow(size_t classes, size_t left, size_t right, size_t opt_left, size_t opt_right,
                const std::vector<long double> &previous, std::vector<long double> &current,
                std::vector<size_t> &splits, const Prefixes &prefix) {
    if (left > right) {
        return;
    }
    const size_t mid = left + (right - left) / 2;
    const size_t first = std::max(opt_left, classes - 1);
    const size_t last = std::min(opt_right, mid - 1);
    long double best = std::numeric_limits<long double>::infinity();
    size_t best_split = first;
    for (size_t split = first; split <= last; ++split) {
        const long double cost = previous[split] + prefix.Variance(split, mid);
        if (cost < best) {
            best = cost;
            best_split = split;
        }
    }
    current[mid] = best;
    splits[mid] = best_split;
    if (left < mid) {
        ComputeRow(classes, left, mid - 1, opt_left, best_split, previous, current, splits, prefix);
    }
    if (mid < right) {
        ComputeRow(classes, mid + 1, right, best_split, opt_right, previous, current, splits, prefix);
    }
}

std::vector<double> Breaks(const Counts &counts, int32_t breaks, bool invert) {
    if (breaks < 1) {
        throw std::invalid_argument("Jenks breaks must be greater than or equal to 1");
    }
    if (counts.Empty()) {
        return {};
    }
    const auto values = counts.Sorted();
    const size_t n = values.size();
    const size_t classes = static_cast<size_t>(breaks);
    if (n <= classes) {
        std::vector<double> result;
        result.reserve(n);
        for (const auto &value : values) {
            result.push_back(value.first);
        }
        return result;
    }
    const Prefixes prefix(values);
    const long double infinity = std::numeric_limits<long double>::infinity();
    std::vector<long double> previous(n + 1, infinity);
    std::vector<long double> current(n + 1, infinity);
    for (size_t i = 1; i <= n; ++i) {
        previous[i] = prefix.Variance(0, i);
    }
    std::vector<std::vector<size_t>> splits(classes + 1, std::vector<size_t>(n + 1, 0));
    for (size_t c = 2; c <= classes; ++c) {
        std::fill(current.begin(), current.end(), infinity);
        ComputeRow(c, c, n, c - 1, n - 1, previous, current, splits[c], prefix);
        previous.swap(current);
    }
    std::vector<double> result(classes);
    size_t end = n;
    for (size_t c = classes; c > 1; --c) {
        const size_t split = splits[c][end];
        if (split >= end || split < c - 1) {
            throw std::runtime_error("failed to reconstruct Jenks classes");
        }
        result[c - 1] = values[invert ? split : end - 1].first;
        end = split;
    }
    result[0] = values[invert ? 0 : end - 1].first;
    return result;
}

bool Valid(uint64_t *mask, idx_t row) {
    return !mask || (mask[row / 64] & (uint64_t(1) << (row % 64))) != 0;
}

void WriteList(duckdb_vector output, idx_t row, const std::vector<double> &values) {
    auto entries = static_cast<duckdb_list_entry *>(duckdb_vector_get_data(output));
    const idx_t offset = duckdb_list_vector_get_size(output);
    entries[row].offset = offset;
    entries[row].length = 0;
    if (values.empty()) {
        duckdb_vector_ensure_validity_writable(output);
        duckdb_validity_set_row_invalid(duckdb_vector_get_validity(output), row);
        return;
    }
    if (values.size() > std::numeric_limits<idx_t>::max() - offset) {
        throw std::overflow_error("Jenks result list is too large");
    }
    const idx_t size = offset + static_cast<idx_t>(values.size());
    if (duckdb_list_vector_reserve(output, size) != DuckDBSuccess) {
        throw std::runtime_error("could not reserve Jenks result list");
    }
    const auto child = duckdb_list_vector_get_child(output);
    auto data = static_cast<double *>(duckdb_vector_get_data(child));
    std::copy(values.begin(), values.end(), data + offset);
    // Child vectors may be reused; explicitly mark newly appended values valid.
    duckdb_vector_ensure_validity_writable(child);
    auto validity = duckdb_vector_get_validity(child);
    for (idx_t i = offset; i < size; ++i) {
        duckdb_validity_set_row_valid(validity, i);
    }
    if (duckdb_list_vector_set_size(output, size) != DuckDBSuccess) {
        throw std::runtime_error("could not resize Jenks result list");
    }
    entries[row].length = values.size();
    duckdb_vector_ensure_validity_writable(output);
    duckdb_validity_set_row_valid(duckdb_vector_get_validity(output), row);
}

void Scalar(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) noexcept {
    try {
        const auto lists = duckdb_data_chunk_get_vector(input, 0);
        const auto classes = duckdb_data_chunk_get_vector(input, 1);
        const bool has_invert = duckdb_data_chunk_get_column_count(input) == 3;
        const auto inverted = has_invert ? duckdb_data_chunk_get_vector(input, 2) : nullptr;
        auto list_validity = duckdb_vector_get_validity(lists);
        auto class_validity = duckdb_vector_get_validity(classes);
        auto invert_validity = has_invert ? duckdb_vector_get_validity(inverted) : nullptr;
        auto entries = static_cast<const duckdb_list_entry *>(duckdb_vector_get_data(lists));
        auto k = static_cast<const int32_t *>(duckdb_vector_get_data(classes));
        auto invert = has_invert ? static_cast<const bool *>(duckdb_vector_get_data(inverted)) : nullptr;
        const auto child = duckdb_list_vector_get_child(lists);
        auto child_validity = duckdb_vector_get_validity(child);
        auto data = static_cast<const double *>(duckdb_vector_get_data(child));
        const idx_t rows = duckdb_data_chunk_get_size(input);
        for (idx_t row = 0; row < rows; ++row) {
            if (!Valid(list_validity, row) || !Valid(class_validity, row) ||
                (has_invert && !Valid(invert_validity, row))) {
                WriteList(output, row, {});
                continue;
            }
            if (k[row] < 1) {
                throw std::invalid_argument("Jenks breaks must be greater than or equal to 1");
            }
            Counts counts;
            const auto &entry = entries[row];
            for (idx_t i = entry.offset; i < entry.offset + entry.length; ++i) {
                if (Valid(child_validity, i)) {
                    counts.Add(data[i]);
                }
            }
            WriteList(output, row, Breaks(counts, k[row], has_invert && invert[row]));
        }
    } catch (const std::exception &error) {
        duckdb_scalar_function_set_error(info, error.what());
    } catch (...) {
        duckdb_scalar_function_set_error(info, "unknown error computing Jenks breaks");
    }
}

struct AggregateState {
    Counts counts;
    int32_t classes = 0;
    bool invert = false;
    bool configured = false;
    bool null_parameters = false;

    void Configure(int32_t requested_classes, bool requested_invert) {
        if (requested_classes < 1) {
            throw std::invalid_argument("Jenks breaks must be greater than or equal to 1");
        }
        if (configured && (classes != requested_classes || invert != requested_invert)) {
            throw std::invalid_argument("ST_JenksBinsAgg requires identical breaks and invert parameters within each group");
        }
        classes = requested_classes;
        invert = requested_invert;
        configured = true;
    }
};

struct AggregateSlot {
    AggregateState *state;
};

AggregateState &GetState(duckdb_aggregate_state slot) {
    auto state = reinterpret_cast<AggregateSlot *>(slot)->state;
    if (!state) {
        throw std::runtime_error("could not initialize Jenks aggregate state");
    }
    return *state;
}

idx_t StateSize(duckdb_function_info) noexcept {
    return sizeof(AggregateSlot);
}

void Initialize(duckdb_function_info info, duckdb_aggregate_state slot) noexcept {
    auto state = reinterpret_cast<AggregateSlot *>(slot);
    state->state = nullptr;
    try {
        state->state = new AggregateState();
    } catch (const std::exception &error) {
        duckdb_aggregate_function_set_error(info, error.what());
    } catch (...) {
        duckdb_aggregate_function_set_error(info, "could not initialize Jenks aggregate state");
    }
}

void Destroy(duckdb_aggregate_state *states, idx_t count) noexcept {
    for (idx_t i = 0; i < count; ++i) {
        auto slot = reinterpret_cast<AggregateSlot *>(states[i]);
        delete slot->state;
        slot->state = nullptr;
    }
}

void Update(duckdb_function_info info, duckdb_data_chunk input, duckdb_aggregate_state *states) noexcept {
    try {
        const auto values = duckdb_data_chunk_get_vector(input, 0);
        const auto classes = duckdb_data_chunk_get_vector(input, 1);
        const bool has_invert = duckdb_data_chunk_get_column_count(input) == 3;
        const auto inverted = has_invert ? duckdb_data_chunk_get_vector(input, 2) : nullptr;
        auto value_validity = duckdb_vector_get_validity(values);
        auto class_validity = duckdb_vector_get_validity(classes);
        auto invert_validity = has_invert ? duckdb_vector_get_validity(inverted) : nullptr;
        auto value = static_cast<const double *>(duckdb_vector_get_data(values));
        auto k = static_cast<const int32_t *>(duckdb_vector_get_data(classes));
        auto invert = has_invert ? static_cast<const bool *>(duckdb_vector_get_data(inverted)) : nullptr;
        const idx_t rows = duckdb_data_chunk_get_size(input);
        for (idx_t row = 0; row < rows; ++row) {
            auto &state = GetState(states[row]);
            // A NULL parameter poisons this group's final result. Non-NULL
            // parameters are still checked on every row, including NULL values.
            if (!Valid(class_validity, row) || (has_invert && !Valid(invert_validity, row))) {
                state.null_parameters = true;
                continue;
            }
            state.Configure(k[row], has_invert && invert[row]);
            if (Valid(value_validity, row)) {
                state.counts.Add(value[row]);
            }
        }
    } catch (const std::exception &error) {
        duckdb_aggregate_function_set_error(info, error.what());
    } catch (...) {
        duckdb_aggregate_function_set_error(info, "unknown error updating Jenks aggregate");
    }
}

void Combine(duckdb_function_info info, duckdb_aggregate_state *source, duckdb_aggregate_state *target,
             idx_t count) noexcept {
    try {
        for (idx_t i = 0; i < count; ++i) {
            auto &from = GetState(source[i]);
            auto &to = GetState(target[i]);
            if (&from == &to) {
                continue;
            }
            if (from.configured) {
                to.Configure(from.classes, from.invert);
            }
            to.null_parameters = to.null_parameters || from.null_parameters;
            to.counts.Merge(from.counts);
        }
    } catch (const std::exception &error) {
        duckdb_aggregate_function_set_error(info, error.what());
    } catch (...) {
        duckdb_aggregate_function_set_error(info, "unknown error combining Jenks aggregates");
    }
}

void Finalize(duckdb_function_info info, duckdb_aggregate_state *source, duckdb_vector output,
              idx_t count, idx_t offset) noexcept {
    try {
        for (idx_t i = 0; i < count; ++i) {
            const auto &state = GetState(source[i]);
            WriteList(output, offset + i,
                      !state.configured || state.null_parameters
                          ? std::vector<double>()
                          : Breaks(state.counts, state.classes, state.invert));
        }
    } catch (const std::exception &error) {
        duckdb_aggregate_function_set_error(info, error.what());
    } catch (...) {
        duckdb_aggregate_function_set_error(info, "unknown error finalizing Jenks aggregate");
    }
}

struct LogicalType {
    duckdb_logical_type value;
    explicit LogicalType(duckdb_type type) : value(duckdb_create_logical_type(type)) {
    }
    explicit LogicalType(duckdb_logical_type type) : value(type) {
    }
    ~LogicalType() {
        duckdb_destroy_logical_type(&value);
    }
};

bool RegisterScalar(duckdb_connection connection, duckdb_logical_type list, duckdb_logical_type integer,
                    duckdb_logical_type boolean) {
    auto functions = duckdb_create_scalar_function_set("ST_JenksBins");
    bool success = true;
    for (int arguments = 2; arguments <= 3; ++arguments) {
        auto function = duckdb_create_scalar_function();
        duckdb_scalar_function_set_name(function, "ST_JenksBins");
        duckdb_scalar_function_add_parameter(function, list);
        duckdb_scalar_function_add_parameter(function, integer);
        if (arguments == 3) {
            duckdb_scalar_function_add_parameter(function, boolean);
        }
        duckdb_scalar_function_set_return_type(function, list);
        duckdb_scalar_function_set_special_handling(function);
        duckdb_scalar_function_set_function(function, Scalar);
        success = duckdb_add_scalar_function_to_set(functions, function) == DuckDBSuccess && success;
        duckdb_destroy_scalar_function(&function);
    }
    if (success) {
        success = duckdb_register_scalar_function_set(connection, functions) == DuckDBSuccess;
    }
    duckdb_destroy_scalar_function_set(&functions);
    return success;
}

bool RegisterAggregate(duckdb_connection connection, duckdb_logical_type number, duckdb_logical_type list,
                       duckdb_logical_type integer, duckdb_logical_type boolean) {
    auto functions = duckdb_create_aggregate_function_set("ST_JenksBinsAgg");
    bool success = true;
    for (int arguments = 2; arguments <= 3; ++arguments) {
        auto function = duckdb_create_aggregate_function();
        duckdb_aggregate_function_set_name(function, "ST_JenksBinsAgg");
        duckdb_aggregate_function_add_parameter(function, number);
        duckdb_aggregate_function_add_parameter(function, integer);
        if (arguments == 3) {
            duckdb_aggregate_function_add_parameter(function, boolean);
        }
        duckdb_aggregate_function_set_return_type(function, list);
        duckdb_aggregate_function_set_special_handling(function);
        duckdb_aggregate_function_set_functions(function, StateSize, Initialize, Update, Combine, Finalize);
        duckdb_aggregate_function_set_destructor(function, Destroy);
        success = duckdb_add_aggregate_function_to_set(functions, function) == DuckDBSuccess && success;
        duckdb_destroy_aggregate_function(&function);
    }
    if (success) {
        success = duckdb_register_aggregate_function_set(connection, functions) == DuckDBSuccess;
    }
    duckdb_destroy_aggregate_function_set(&functions);
    return success;
}

} // namespace

bool RegisterJenks(duckdb_connection connection) {
    try {
        LogicalType number(DUCKDB_TYPE_DOUBLE);
        LogicalType integer(DUCKDB_TYPE_INTEGER);
        LogicalType boolean(DUCKDB_TYPE_BOOLEAN);
        LogicalType list(duckdb_create_list_type(number.value));
        return RegisterScalar(connection, list.value, integer.value, boolean.value) &&
               RegisterAggregate(connection, number.value, list.value, integer.value, boolean.value);
    } catch (...) {
        return false;
    }
}

} // namespace eviltransform
