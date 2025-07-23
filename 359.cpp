#include <algorithm>
#include <cassert>
#include <cctype>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// ---------- TYPE ENUM ----------
enum class TypeID { INT, FLOAT, BOOL, STRING };

// ---------- IColumn INTERFACE ----------
struct IColumn {
  virtual ~IColumn() {}
  virtual TypeID type() const = 0;
  virtual size_t size() const = 0;
};

// ---------- TYPED COLUMN ----------
template <typename T>
struct Column : IColumn {
  std::vector<T> data;
  Column() {}
  Column(size_t n) : data(n) {}
  TypeID type() const override;
  size_t size() const override { return data.size(); }
  void append(const T& v) { data.push_back(v); }
};
template <> TypeID Column<int>::type()     { return TypeID::INT; }
template <> TypeID Column<float>::type()   { return TypeID::FLOAT; }
template <> TypeID Column<bool>::type()    { return TypeID::BOOL; }
template <> TypeID Column<std::string>::type() { return TypeID::STRING; }

// ---------- RUN‐LENGTH ENCODED BOOL COLUMN ----------
struct RLEBoolColumn : IColumn {
  // store (value, run‐length)
  std::vector<std::pair<bool,size_t>> runs;
  size_t total = 0;
  TypeID type() const override { return TypeID::BOOL; }
  size_t size() const override { return total; }
  void append(bool b) {
    if (!runs.empty() && runs.back().first == b) {
      runs.back().second++;
    } else {
      runs.emplace_back(b,1);
    }
    ++total;
  }
  // expand to a vector<bool> mask
  void expand(std::vector<bool>& out) const {
    out.resize(total);
    size_t idx=0;
    for (auto &r:runs) {
      std::fill(out.begin()+idx, out.begin()+idx+r.second, r.first);
      idx += r.second;
    }
  }
};

// ---------- DICTIONARY ENCODED STRING COLUMN ----------
struct DictStringColumn : IColumn {
  std::vector<std::string> dict;
  std::unordered_map<std::string,int> index_of;
  std::vector<int> codes;
  TypeID type() const override { return TypeID::STRING; }
  size_t size() const override { return codes.size(); }
  void append(const std::string& s) {
    auto it = index_of.find(s);
    int code;
    if (it==index_of.end()) {
      code = dict.size();
      dict.push_back(s);
      index_of[s]=code;
    } else {
      code = it->second;
    }
    codes.push_back(code);
  }
  const std::string& get(int code) const { return dict[code]; }
};

// ---------- TABLE ----------
struct Table {
  std::vector<std::string> colNames;
  std::vector<std::shared_ptr<IColumn>> columns;
  std::unordered_map<std::string,int> nameToIdx;
  size_t nrows = 0;

  template<typename ColT>
  void addColumn(const std::string& name) {
    int idx = columns.size();
    colNames.push_back(name);
    columns.push_back(std::make_shared<ColT>());
    nameToIdx[name] = idx;
  }

  IColumn* getColumn(const std::string& name) {
    return columns[nameToIdx[name]].get();
  }

  // CSV loader: assumes header line matches added columns
  void loadCSV(const std::string& fname) {
    std::ifstream in(fname);
    std::string line, cell;
    // read header
    if (!std::getline(in, line)) return;
    // skip header contents (we use manually defined schema)
    while (std::getline(in, line)) {
      std::istringstream ss(line);
      int col = 0;
      while (std::getline(ss, cell, ',')) {
        auto &ic = columns[col];
        switch (ic->type()) {
        case TypeID::INT:
          static_cast<Column<int>*>(ic.get())->append(std::stoi(cell));
          break;
        case TypeID::FLOAT:
          static_cast<Column<float>*>(ic.get())->append(std::stof(cell));
          break;
        case TypeID::BOOL:
          static_cast<Column<bool>*>(ic.get())->append(cell=="1"||cell=="true");
          break;
        case TypeID::STRING:
          static_cast<Column<std::string>*>(ic.get())->append(cell);
          break;
        }
        ++col;
      }
      ++nrows;
    }
  }
};

// ---------- PREDICATE ----------
struct Predicate {
  virtual ~Predicate() {}
  // returns a bit‐mask of length n rows where predicate is true
  virtual void evalMask(const Table& tbl, std::vector<bool>& mask) const = 0;
};

// a simple comparison predicate col OP const‐value
template<typename T>
struct CompPred : Predicate {
  int colIdx;
  char op; // '>', '<', '=', '!'
  T value;
  CompPred(int c, char o, T v) : colIdx(c), op(o), value(v) {}
  void evalMask(const Table& tbl, std::vector<bool>& mask) const override {
    auto col = static_cast<Column<T>*>(tbl.columns[colIdx].get());
    size_t n = col->data.size();
    mask.resize(n);
    const T* data = col->data.data();
    for (size_t i=0;i<n;i++) {
      bool ok=false;
      switch(op){
      case '>': ok = data[i] > value; break;
      case '<': ok = data[i] < value; break;
      case '=': ok = data[i] == value; break;
      case '!': ok = data[i] != value; break;
      }
      mask[i]=ok;
    }
  }
};

// ---------- AGGREGATORS ----------
struct Aggregator {
  virtual ~Aggregator() {}
  virtual void consume(const Table& tbl, const std::vector<bool>& mask)=0;
  virtual void printResult() const = 0;
};

template<typename T>
struct SumAgg : Aggregator {
  int colIdx; T sum{};
  SumAgg(int c):colIdx(c){}
  void consume(const Table& tbl, const std::vector<bool>& mask) override {
    auto col = static_cast<Column<T>*>(tbl.columns[colIdx].get());
    size_t n = col->data.size();
    const T* data = col->data.data();
    for(size_t i=0;i<n;i++){
      if (mask[i]) sum += data[i];
    }
  }
  void printResult() const override {
    std::cout << "SUM = " << sum << "\n";
  }
};

struct CountAgg : Aggregator {
  size_t count = 0;
  void consume(const Table& /*tbl*/, const std::vector<bool>& mask) override {
    for(bool b:mask) if(b) ++count;
  }
  void printResult() const override {
    std::cout << "COUNT = " << count << "\n";
  }
};

// ---------- SIMPLE SQL‐LIKE PARSER & QUERY ENGINE ----------
struct Query {
  std::vector<std::shared_ptr<Aggregator>> aggs;
  std::unique_ptr<Predicate> pred;
  std::string tableName;
};

struct Engine {
  std::unordered_map<std::string,Table> tables;

  void registerTable(const std::string& name, Table&& t) {
    tables[name] = std::move(t);
  }

  // parse very limited:
  // SELECT SUM(col), COUNT(*) FROM tbl WHERE col > 100
  Query parse(const std::string& sql) {
    std::istringstream ss(sql);
    std::string tok;
    Query q;
    ss>>tok; // SELECT
    // parse aggs until FROM
    while (ss>>tok && tok!="FROM") {
      if (tok.back()==',') tok.pop_back();
      // SUM(col)
      if (tok.rfind("SUM(",0)==0) {
        auto i=tok.find('(');
        auto j=tok.find(')');
        std::string col = tok.substr(i+1,j-i-1);
        // find col idx and type
        // postpone binding until run()
        q.aggs.push_back(nullptr);
        // we will fill in binding later
      }
      else if (tok.rfind("COUNT",0)==0) {
        q.aggs.push_back(nullptr);
      }
    }
    ss>>q.tableName;
    // optional WHERE
    if (ss>>tok && tok=="WHERE") {
      std::string col,op,val;
      ss>>col>>op>>val;
      // strip commas
      if (val.back()==';') val.pop_back();
      Table& t = tables[q.tableName];
      int idx = t.nameToIdx[col];
      // detect type
      IColumn* ic = t.columns[idx].get();
      switch(ic->type()){
      case TypeID::INT:
        q.pred.reset(new CompPred<int>(idx,op[0],std::stoi(val)));
        break;
      case TypeID::FLOAT:
        q.pred.reset(new CompPred<float>(idx,op[0],std::stof(val)));
        break;
      default:
        throw std::runtime_error("unsupported WHERE type");
      }
    } else {
      // no predicate => all‐true mask
      struct AllTrue:Predicate{void evalMask(const Table& tbl, std::vector<bool>& m) const override {
        m.assign(tbl.nrows,true);
      }};
      q.pred.reset(new AllTrue());
    }

    // Now bind aggs properly (late binding)
    // Very naive: re‐parse the SELECT part
    ss.clear(); ss.seekg(0);
    ss>>tok; // SELECT
    int aggIdx=0;
    while (ss>>tok && tok!="FROM") {
      if (tok.back()==',') tok.pop_back();
      if (tok.rfind("SUM(",0)==0) {
        auto i=tok.find('(');
        auto j=tok.find(')');
        std::string col = tok.substr(i+1,j-i-1);
        int idx = tables[q.tableName].nameToIdx[col];
        // detect type and create
        IColumn* ic = tables[q.tableName].columns[idx].get();
        if (ic->type()==TypeID::INT)
          q.aggs[aggIdx] = std::make_shared<SumAgg<int>>(idx);
        else if (ic->type()==TypeID::FLOAT)
          q.aggs[aggIdx] = std::make_shared<SumAgg<float>>(idx);
        else throw std::runtime_error("SUM on non-numeric");
      }
      else if (tok.rfind("COUNT",0)==0) {
        q.aggs[aggIdx] = std::make_shared<CountAgg>();
      }
      ++aggIdx;
    }

    return q;
  }

  void execute(const Query& q) {
    auto& tbl = tables.at(q.tableName);
    std::vector<bool> mask;
    q.pred->evalMask(tbl,mask);
    for (auto& agg : q.aggs) {
      agg->consume(tbl,mask);
      agg->printResult();
    }
  }
};

// ---------- DEMO MAIN ----------
int main(){
  Engine engr;
  // define schema
  Table trades;
  trades.addColumn<Column<int>>("id");
  trades.addColumn<Column<float>>("price");
  trades.addColumn<Column<int>>("qty");
  trades.addColumn<Column<bool>>("is_buy");
  trades.addColumn<Column<std::string>>("symbol");
  // load CSV (make a small example.csv yourself with header id,price,qty,is_buy,symbol)
  trades.loadCSV("example.csv");
  trades.nrows = trades.columns[0]->size();

  engr.registerTable("trades", std::move(trades));

  // run a sample query
  std::string sql = "SELECT SUM(price), COUNT(*) FROM trades WHERE price > 100";
  auto q = engr.parse(sql);
  engr.execute(q);
  return 0;
}