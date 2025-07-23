#include <iostream>
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include <variant>
#include <sstream>
#include <fstream>
#include <numeric>
#include <algorithm>
#include <utility>
#include <stdexcept>

// -----------------------------
// Custom Exception Types
// -----------------------------
class DuplicateColumnException : public std::exception {
public:
    const char* what() const noexcept override {
        return "Duplicate column";
    }
};

class RowSizeMismatchException : public std::exception {
public:
    const char* what() const noexcept override {
        return "Row size does not match column size.";
    }
};

// -----------------------------
// Column Types & Definitions
// -----------------------------
enum class ColumnType { INT, FLOAT, STRING, BOOL };

struct Column {
    virtual ~Column() = default;
    virtual ColumnType get_type() const = 0;
    virtual size_t size() const = 0;
    virtual void add_from_string(const std::string& val) = 0;
};

struct IntColumn : public Column {
    std::vector<int> data;
    ColumnType get_type() const override { return ColumnType::INT; }
    size_t size() const override { return data.size(); }
    void add_from_string(const std::string& val) override {
        data.push_back(std::stoi(val));
    }
};

struct FloatColumn : public Column {
    std::vector<float> data;
    ColumnType get_type() const override { return ColumnType::FLOAT; }
    size_t size() const override { return data.size(); }
    void add_from_string(const std::string& val) override {
        data.push_back(std::stof(val));
    }
};

struct StringColumn : public Column {
    std::vector<uint32_t> indices;
    std::vector<std::string> dictionary;
    std::unordered_map<std::string, uint32_t, std::hash<std::string>, std::equal_to<>> reverse_dict;

    ColumnType get_type() const override { return ColumnType::STRING; }
    size_t size() const override { return indices.size(); }

    void add_from_string(const std::string& val) override {
        if (!reverse_dict.contains(val)) {
            uint32_t idx = static_cast<uint32_t>(dictionary.size());
            dictionary.push_back(val);
            reverse_dict[val] = idx;
        }
        indices.push_back(reverse_dict[val]);
    }

    const std::string& get_value(size_t idx) const {
        return dictionary[indices[idx]];
    }
};

struct BoolColumn : public Column {
    std::vector<std::pair<bool, size_t>> rle_data;
    size_t total_size = 0;

    ColumnType get_type() const override { return ColumnType::BOOL; }
    size_t size() const override { return total_size; }

    void add_from_string(const std::string& val) override {
        if (bool b = (val == "1" || val == "true" || val == "BUY"); 
            rle_data.empty() || rle_data.back().first != b) {
            rle_data.emplace_back(b, 1);
        } else {
            rle_data.back().second++;
        }
        ++total_size;
    }

    std::vector<bool> decompress() const {
        std::vector<bool> result;
        result.reserve(total_size);
        for (const auto& [val, count] : rle_data) {
            result.insert(result.end(), count, val);
        }
        return result;
    }
};

// -----------------------------
// Table Definition
// -----------------------------
class Table {
private:
    std::unordered_map<std::string, std::shared_ptr<Column>, std::hash<std::string>, std::equal_to<>> columns;
    std::vector<std::string> column_order;
    size_t row_count = 0;

public:
    void add_column(const std::string& name, ColumnType type) {
        if (columns.contains(name)) {
            throw DuplicateColumnException();
        }

        using enum ColumnType;

        std::shared_ptr<Column> col;
        switch (type) {
            case INT: col = std::make_shared<IntColumn>(); break;
            case FLOAT: col = std::make_shared<FloatColumn>(); break;
            case STRING: col = std::make_shared<StringColumn>(); break;
            case BOOL: col = std::make_shared<BoolColumn>(); break;
        }
        columns[name] = col;
        column_order.push_back(name);
    }

    void add_row(const std::vector<std::string>& values) {
        if (values.size() != column_order.size()) {
            throw RowSizeMismatchException();
        }

        for (size_t i = 0; i < values.size(); ++i) {
            columns[column_order[i]]->add_from_string(values[i]);
        }
        ++row_count;
    }

    std::shared_ptr<Column> get_column(const std::string& name) const {
        return columns.at(name);
    }

    size_t get_row_count() const { return row_count; }
    const std::vector<std::string>& get_column_names() const { return column_order; }
};

// -----------------------------
// CSV Import
// -----------------------------
void load_csv(const std::string& csv, Table& table, const std::unordered_map<std::string, ColumnType, std::hash<std::string>, std::equal_to<>>& schema) {
    std::istringstream ss(csv);
    std::string line;

    std::getline(ss, line);
    std::stringstream header_ss(line);
    std::string col;
    std::vector<std::string> headers;

    while (std::getline(header_ss, col, ',')) {
        headers.push_back(col);
        if (!schema.contains(col)) {
            throw std::invalid_argument("Unknown column in schema: " + col);
        }
        table.add_column(col, schema.at(col));
    }

    while (std::getline(ss, line)) {
        std::stringstream row_ss(line);
        std::string val;
        std::vector<std::string> row;
        while (std::getline(row_ss, val, ',')) {
            row.push_back(val);
        }
        table.add_row(row);
    }
}

// -----------------------------
// Aggregation Example
// -----------------------------
int main() {
    std::string csv_data =
        "symbol,price,volume,side\n"
        "AAPL,150.5,100,BUY\n"
        "GOOG,2800.5,50,SELL\n"
        "AAPL,152.0,75,BUY\n"
        "TSLA,900.0,30,BUY\n"
        "MSFT,305.5,60,SELL";

    std::unordered_map<std::string, ColumnType, std::hash<std::string>, std::equal_to<>> schema = {
        {"symbol", ColumnType::STRING},
        {"price", ColumnType::FLOAT},
        {"volume", ColumnType::INT},
        {"side", ColumnType::BOOL}
    };

    Table trades;
    load_csv(csv_data, trades, schema);

    auto price_col = std::dynamic_pointer_cast<FloatColumn>(trades.get_column("price"));

    float sum_price = std::accumulate(price_col->data.begin(), price_col->data.end(), 0.0f);

    std::cout << "Total Price: " << sum_price << "\n";
    return 0;
}
