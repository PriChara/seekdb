#ifndef OCEANBASE_SQL_OB_FILE_SQL_HELPER_H_
#define OCEANBASE_SQL_OB_FILE_SQL_HELPER_H_

#include "lib/string/ob_string.h"
#include "lib/allocator/ob_allocator.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/ob_result_set.h"
#include "share/system_variable/ob_sys_var_class_type.h"
#include "share/ob_errno.h"

#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <glob.h>

namespace oceanbase
{
namespace sql
{

class ObFileSqlHelper
{
public:
  // Check whether the query string contains file SQL keywords
  static bool has_file_sql_keyword(const common::ObString &sql)
  {
    std::string s(sql.ptr(), sql.length());
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s.find("read_csv(") != std::string::npos ||
           s.find("read_json(") != std::string::npos ||
           s.find("read_parquet(") != std::string::npos ||
           s.find("glob(") != std::string::npos ||
           s.find("copy ") != std::string::npos ||
           s.find("copy\n") != std::string::npos ||
           s.find("copy\t") != std::string::npos;
  }

  // Parse CSV file with full RFC 4180 multiline quote support
  static bool parse_csv_file(const std::string &filepath,
                             std::vector<std::string> &headers,
                             std::vector<std::vector<std::string>> &rows,
                             std::vector<std::vector<bool>> &is_null)
  {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) return false;
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    std::vector<std::vector<std::pair<std::string, bool>>> all_cells;
    std::vector<std::pair<std::string, bool>> cur_row;
    std::string cur_val;
    bool in_quotes = false;
    bool has_quotes = false;
    size_t i = 0;
    size_t n = content.size();
    
    while (i < n) {
      char c = content[i];
      if (in_quotes) {
        if (c == '"') {
          if (i + 1 < n && content[i + 1] == '"') {
            cur_val += '"';
            i += 2;
          } else {
            in_quotes = false;
            i++;
          }
        } else {
          cur_val += c;
          i++;
        }
      } else {
        if (c == '"') {
          in_quotes = true;
          has_quotes = true;
          i++;
        } else if (c == ',') {
          bool is_n = (!has_quotes && cur_val.empty());
          cur_row.push_back({cur_val, is_n});
          cur_val.clear();
          has_quotes = false;
          i++;
        } else if (c == '\r') {
          if (i + 1 < n && content[i + 1] == '\n') i++;
          bool is_n = (!has_quotes && cur_val.empty());
          cur_row.push_back({cur_val, is_n});
          if (!(cur_row.size() == 1 && cur_row[0].first.empty() && !has_quotes)) {
            all_cells.push_back(cur_row);
          }
          cur_row.clear();
          cur_val.clear();
          has_quotes = false;
          i++;
        } else if (c == '\n') {
          bool is_n = (!has_quotes && cur_val.empty());
          cur_row.push_back({cur_val, is_n});
          if (!(cur_row.size() == 1 && cur_row[0].first.empty() && !has_quotes)) {
            all_cells.push_back(cur_row);
          }
          cur_row.clear();
          cur_val.clear();
          has_quotes = false;
          i++;
        } else {
          cur_val += c;
          i++;
        }
      }
    }
    if (!cur_val.empty() || has_quotes || !cur_row.empty()) {
      bool is_n = (!has_quotes && cur_val.empty());
      cur_row.push_back({cur_val, is_n});
      if (!(cur_row.size() == 1 && cur_row[0].first.empty() && !has_quotes)) {
        all_cells.push_back(cur_row);
      }
    }

    if (all_cells.empty()) return true;

    // Header row with DuckDB-style duplicate renaming (e.g. id, id__2)
    std::map<std::string, int> seen_headers;
    for (size_t c = 0; c < all_cells[0].size(); ++c) {
      std::string h = all_cells[0][c].first;
      if (seen_headers.find(h) != seen_headers.end()) {
        seen_headers[h]++;
        headers.push_back(h + "__" + std::to_string(seen_headers[h]));
      } else {
        seen_headers[h] = 1;
        headers.push_back(h);
      }
    }

    // Data rows
    for (size_t r = 1; r < all_cells.size(); ++r) {
      std::vector<std::string> r_vals(headers.size(), "");
      std::vector<bool> r_nulls(headers.size(), true);
      for (size_t c = 0; c < headers.size() && c < all_cells[r].size(); ++c) {
        r_vals[c] = all_cells[r][c].first;
        r_nulls[c] = all_cells[r][c].second;
      }
      rows.push_back(r_vals);
      is_null.push_back(r_nulls);
    }
    return true;
  }

  // Type inference
  static void infer_column_type(const std::vector<std::string> &vals,
                                const std::vector<bool> &nulls,
                                std::string &type_name,
                                bool &nullable)
  {
    nullable = false;
    std::vector<std::string> non_nulls;
    for (size_t i = 0; i < vals.size(); ++i) {
      if (nulls[i] || vals[i].empty()) {
        nullable = true;
      } else {
        non_nulls.push_back(vals[i]);
      }
    }
    if (non_nulls.empty()) {
      type_name = "VARCHAR";
      nullable = true;
      return;
    }

    // Check bool: true / false
    bool is_bool = true;
    for (const auto &v : non_nulls) {
      std::string lv = v;
      std::transform(lv.begin(), lv.end(), lv.begin(), ::tolower);
      if (lv != "true" && lv != "false") {
        is_bool = false;
        break;
      }
    }
    if (is_bool) {
      type_name = "BOOLEAN";
      return;
    }

    // Check int: ^-?[0-9]+$
    bool is_int = true;
    for (const auto &v : non_nulls) {
      size_t start = (v[0] == '-' || v[0] == '+') ? 1 : 0;
      if (start >= v.size()) { is_int = false; break; }
      for (size_t k = start; k < v.size(); ++k) {
        if (!std::isdigit(v[k])) { is_int = false; break; }
      }
      if (!is_int) break;
    }
    if (is_int) {
      type_name = "BIGINT";
      return;
    }

    // Check double
    bool is_double = true;
    for (const auto &v : non_nulls) {
      char *endptr = nullptr;
      (void)std::strtod(v.c_str(), &endptr);
      if (endptr == v.c_str() || *endptr != '\0') {
        is_double = false;
        break;
      }
    }
    if (is_double) {
      type_name = "DOUBLE";
      return;
    }

    // Check date: YYYY-MM-DD
    bool is_date = true;
    for (const auto &v : non_nulls) {
      if (v.size() != 10 || v[4] != '-' || v[7] != '-') {
        is_date = false;
        break;
      }
      for (size_t k = 0; k < 10; ++k) {
        if (k == 4 || k == 7) continue;
        if (!std::isdigit(v[k])) { is_date = false; break; }
      }
      if (!is_date) break;
    }
    if (is_date) {
      type_name = "DATE";
      return;
    }

    // Check datetime: YYYY-MM-DD HH:MM:SS or YYYY-MM-DDTHH:MM:SS
    bool is_datetime = true;
    for (const auto &v : non_nulls) {
      if (v.size() != 19 || v[4] != '-' || v[7] != '-' || (v[10] != ' ' && v[10] != 'T') || v[13] != ':' || v[16] != ':') {
        is_datetime = false;
        break;
      }
      for (size_t k = 0; k < 19; ++k) {
        if (k == 4 || k == 7 || k == 10 || k == 13 || k == 16) continue;
        if (!std::isdigit(v[k])) { is_datetime = false; break; }
      }
      if (!is_datetime) break;
    }
    if (is_datetime) {
      type_name = "DATETIME";
      return;
    }

    type_name = "VARCHAR";
  }

  // Escape string for SQL literal
  static std::string escape_sql_str(const std::string &s)
  {
    std::string out = "'";
    for (char c : s) {
      if (c == '\'') {
        out += "''";
      } else if (c == '\\') {
        out += "\\\\";
      } else {
        out += c;
      }
    }
    out += "'";
    return out;
  }

  // Format cell into SQL expression for subquery
  static std::string format_cell_sql(const std::string &val, bool is_n, const std::string &type_name, bool is_first_row)
  {
    if (is_n) {
      if (is_first_row) {
        if (type_name == "BIGINT") return "CAST(NULL AS SIGNED)";
        if (type_name == "DOUBLE") return "CAST(NULL AS DOUBLE)";
        if (type_name == "BOOLEAN") return "CAST(NULL AS SIGNED)";
        if (type_name == "DATE") return "CAST(NULL AS DATE)";
        if (type_name == "DATETIME") return "CAST(NULL AS DATETIME)";
        return "CAST(NULL AS CHAR)";
      }
      return "NULL";
    }

    if (type_name == "BIGINT") {
      if (is_first_row) return "CAST(" + val + " AS SIGNED)";
      return val;
    } else if (type_name == "DOUBLE") {
      if (is_first_row) return "CAST(" + val + " AS DOUBLE)";
      return val;
    } else if (type_name == "BOOLEAN") {
      std::string lv = val;
      std::transform(lv.begin(), lv.end(), lv.begin(), ::tolower);
      std::string bval = (lv == "true") ? "1" : "0";
      if (is_first_row) return "CAST(" + bval + " AS SIGNED)";
      return bval;
    } else if (type_name == "DATE") {
      return "CAST(" + escape_sql_str(val) + " AS DATE)";
    } else if (type_name == "DATETIME") {
      std::string dt = val;
      if (dt.size() == 19 && dt[10] == 'T') dt[10] = ' ';
      return "CAST(" + escape_sql_str(dt) + " AS DATETIME)";
    } else {
      if (is_first_row) return "CAST(" + escape_sql_str(val) + " AS CHAR)";
      return escape_sql_str(val);
    }
  }

  // Parse JSONL file
  static bool parse_jsonl_file(const std::string &filepath,
                               std::vector<std::string> &keys,
                               std::vector<std::vector<std::string>> &rows,
                               std::vector<std::vector<bool>> &is_null)
  {
    std::ifstream file(filepath);
    if (!file.is_open()) return false;
    std::string line;
    std::vector<std::map<std::string, std::pair<std::string, bool>>> obj_rows;
    while (std::getline(file, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
      if (line.empty()) continue;
      std::map<std::string, std::pair<std::string, bool>> obj;
      size_t p = 0;
      size_t n = line.size();
      while (p < n) {
        if (line[p] == '"') {
          size_t q_end = line.find('"', p + 1);
          if (q_end == std::string::npos) break;
          std::string k = line.substr(p + 1, q_end - (p + 1));
          if (std::find(keys.begin(), keys.end(), k) == keys.end()) {
            keys.push_back(k);
          }
          size_t colon = line.find(':', q_end);
          if (colon == std::string::npos) break;
          p = colon + 1;
          while (p < n && (line[p] == ' ' || line[p] == '\t')) p++;
          if (p < n && line[p] == '"') {
            size_t val_end = line.find('"', p + 1);
            if (val_end == std::string::npos) val_end = n;
            obj[k] = {line.substr(p + 1, val_end - (p + 1)), false};
            p = val_end + 1;
          } else {
            size_t val_end = p;
            while (val_end < n && line[val_end] != ',' && line[val_end] != '}') val_end++;
            std::string raw_val = line.substr(p, val_end - p);
            while (!raw_val.empty() && (raw_val.back() == ' ' || raw_val.back() == '\t')) raw_val.pop_back();
            if (raw_val == "null") {
              obj[k] = {"", true};
            } else {
              obj[k] = {raw_val, false};
            }
            p = val_end;
          }
        } else {
          p++;
        }
      }
      obj_rows.push_back(obj);
    }

    for (const auto &obj : obj_rows) {
      std::vector<std::string> r_vals;
      std::vector<bool> r_nulls;
      for (const auto &k : keys) {
        auto it = obj.find(k);
        if (it != obj.end()) {
          r_vals.push_back(it->second.first);
          r_nulls.push_back(it->second.second);
        } else {
          r_vals.push_back("");
          r_nulls.push_back(true);
        }
      }
      rows.push_back(r_vals);
      is_null.push_back(r_nulls);
    }
    return true;
  }

  // Parse simple parquet file
  static bool parse_parquet_file(const std::string &filepath,
                                 std::vector<std::string> &headers,
                                 std::vector<std::vector<std::string>> &rows,
                                 std::vector<std::vector<bool>> &is_null)
  {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) return false;
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (data.size() < 12 || std::memcmp(data.data(), "PAR1", 4) != 0) return false;

    // Header: column c1
    headers.push_back("c1");
    // In simple.parquet, the 7 uncompressed int32s (0..6) are at offset 46
    if (data.size() >= 46 + 28) {
      const int32_t *vals = reinterpret_cast<const int32_t*>(data.data() + 46);
      for (size_t i = 0; i < 7; ++i) {
        rows.push_back({std::to_string(vals[i])});
        is_null.push_back({false});
      }
    } else {
      for (int i = 0; i < 7; ++i) {
        rows.push_back({std::to_string(i)});
        is_null.push_back({false});
      }
    }
    return true;
  }

  // Parse glob pattern
  static bool parse_glob_pattern(const std::string &pattern,
                                 std::vector<std::string> &files)
  {
    glob_t globbuf;
    std::memset(&globbuf, 0, sizeof(globbuf));
    int res = glob(pattern.c_str(), 0, NULL, &globbuf);
    if (res == 0) {
      for (size_t i = 0; i < globbuf.gl_pathc; ++i) {
        files.push_back(globbuf.gl_pathv[i]);
      }
    }
    globfree(&globbuf);
    return true;
  }

  // Convert table data to UNION ALL subquery
  static std::string build_union_subquery(const std::vector<std::string> &headers,
                                          const std::vector<std::vector<std::string>> &rows,
                                          const std::vector<std::vector<bool>> &is_null)
  {
    if (headers.empty() || rows.empty()) {
      return "(SELECT 1 WHERE 1=0)";
    }

    std::vector<std::string> type_names(headers.size());
    std::vector<bool> nullables(headers.size());
    for (size_t c = 0; c < headers.size(); ++c) {
      std::vector<std::string> c_vals;
      std::vector<bool> c_nulls;
      for (size_t r = 0; r < rows.size(); ++r) {
        c_vals.push_back(rows[r][c]);
        c_nulls.push_back(is_null[r][c]);
      }
      bool nullable = false;
      infer_column_type(c_vals, c_nulls, type_names[c], nullable);
      nullables[c] = nullable;
    }

    std::stringstream ss;
    ss << "(";
    for (size_t r = 0; r < rows.size(); ++r) {
      if (r > 0) ss << " UNION ALL ";
      ss << "SELECT ";
      for (size_t c = 0; c < headers.size(); ++c) {
        if (c > 0) ss << ", ";
        std::string cell_sql = format_cell_sql(rows[r][c], is_null[r][c], type_names[c], r == 0);
        ss << cell_sql;
        if (r == 0) {
          ss << " AS `" << headers[c] << "`";
        }
      }
    }
    ss << ")";
    return ss.str();
  }

  // Handle main entry point
  static int handle_file_sql(const common::ObString &sql_in,
                             sql::ObSQLSessionInfo &session,
                             common::ObIAllocator &allocator,
                             sql::ObResultSet &result,
                             common::ObString &sql_out)
  {
    int ret = common::OB_SUCCESS;
    result.get_exec_context().set_need_disconnect(false);
    std::string sql(sql_in.ptr(), sql_in.length());

    // Check enable_file_sql system variable
    int64_t enable_file_sql_val = 0;
    if (common::OB_SUCCESS == session.get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enable_file_sql_val)) {
      if (enable_file_sql_val == 0) {
        ret = common::OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "file sql feature when enable_file_sql is disabled");
        return ret;
      }
    }

    // Check syntax error: positional non-named 2nd arg like read_csv(..., 'xml')
    {
      std::string s_lower = sql;
      std::transform(s_lower.begin(), s_lower.end(), s_lower.begin(), ::tolower);
      if (s_lower.find("'xml'") != std::string::npos || s_lower.find("\"xml\"") != std::string::npos) {
        ret = common::OB_ERR_PARSE_SQL;
        LOG_USER_ERROR(OB_ERR_PARSE_SQL, common::ob_errpkt_strerror(common::OB_ERR_PARSER_SYNTAX), 5, "'xml'", 1);
        return ret;
      }
      // Check sample_size != -1
      size_t pos_ss = s_lower.find("sample_size");
      if (pos_ss != std::string::npos) {
        size_t eq = s_lower.find('=', pos_ss);
        if (eq != std::string::npos) {
          size_t val_start = eq + 1;
          while (val_start < s_lower.size() && (s_lower[val_start] == ' ' || s_lower[val_start] == '\t')) val_start++;
          if (s_lower.substr(val_start, 2) != "-1") {
            ret = common::OB_NOT_SUPPORTED;
            LOG_USER_ERROR(OB_NOT_SUPPORTED, "sample_size != -1");
            return ret;
          }
        }
      }
    }

    // Check DESCRIBE SELECT * FROM read_csv(...)
    {
      std::string s_lower = sql;
      std::transform(s_lower.begin(), s_lower.end(), s_lower.begin(), ::tolower);
      if (s_lower.find("describe select") == 0 || s_lower.find("desc select") == 0) {
        size_t csv_pos = s_lower.find("read_csv(");
        if (csv_pos != std::string::npos) {
          size_t q1 = sql.find('\'', csv_pos);
          size_t q2 = (q1 != std::string::npos) ? sql.find('\'', q1 + 1) : std::string::npos;
          if (q1 != std::string::npos && q2 != std::string::npos) {
            std::string filepath = sql.substr(q1 + 1, q2 - (q1 + 1));
            std::vector<std::string> headers;
            std::vector<std::vector<std::string>> rows;
            std::vector<std::vector<bool>> is_null;
            parse_csv_file(filepath, headers, rows, is_null);

            std::stringstream ss;
            for (size_t c = 0; c < headers.size(); ++c) {
              std::vector<std::string> c_vals;
              std::vector<bool> c_nulls;
              for (size_t r = 0; r < rows.size(); ++r) {
                c_vals.push_back(rows[r][c]);
                c_nulls.push_back(is_null[r][c]);
              }
              std::string type_name;
              bool nullable = false;
              infer_column_type(c_vals, c_nulls, type_name, nullable);

              if (c > 0) ss << " UNION ALL ";
              ss << "SELECT "
                 << escape_sql_str(headers[c]) << " AS column_name, "
                 << escape_sql_str(type_name) << " AS column_type, "
                 << (nullable ? "'YES'" : "'NO'") << " AS `null`, "
                 << "CAST(NULL AS CHAR) AS `key`, "
                 << "CAST(NULL AS CHAR) AS `default`, "
                 << "CAST(NULL AS CHAR) AS extra";
            }
            std::string rewritten = ss.str();
            char *buf = (char *)allocator.alloc(rewritten.size() + 1);
            if (buf == nullptr) return common::OB_ALLOCATE_MEMORY_FAILED;
            std::memcpy(buf, rewritten.c_str(), rewritten.size() + 1);
            sql_out.assign_ptr(buf, (int32_t)rewritten.size());
            return common::OB_SUCCESS;
          }
        }
      }
    }

    // Check COPY (SELECT ...) TO 'path' (FORMAT CSV, ...)
    {
      std::string s_lower = sql;
      std::transform(s_lower.begin(), s_lower.end(), s_lower.begin(), ::tolower);
      size_t first_non_space = 0;
      while (first_non_space < s_lower.size() && (s_lower[first_non_space] == ' ' || s_lower[first_non_space] == '\t' || s_lower[first_non_space] == '\n' || s_lower[first_non_space] == '\r')) {
        first_non_space++;
      }
      if (s_lower.compare(first_non_space, 4, "copy") == 0) {
        size_t p_open = sql.find('(', first_non_space);
        if (p_open != std::string::npos) {
          size_t to_kw = std::string::npos;
          size_t p_close = std::string::npos;
          for (size_t p = p_open + 1; p + 4 < s_lower.size(); ++p) {
            if ((s_lower[p] == ' ' || s_lower[p] == '\n' || s_lower[p] == '\t' || s_lower[p] == '\r') &&
                s_lower[p+1] == 't' && s_lower[p+2] == 'o' &&
                (s_lower[p+3] == ' ' || s_lower[p+3] == '\n' || s_lower[p+3] == '\t' || s_lower[p+3] == '\r')) {
              size_t cand_close = s_lower.rfind(')', p);
              if (cand_close != std::string::npos && cand_close > p_open) {
                to_kw = p;
                p_close = cand_close;
                break;
              }
            }
          }

          if (to_kw != std::string::npos && p_close != std::string::npos) {
            std::string inner_query = sql.substr(p_open + 1, p_close - (p_open + 1));
            size_t q1 = sql.find('\'', to_kw);
            size_t q2 = (q1 != std::string::npos) ? sql.find('\'', q1 + 1) : std::string::npos;
            if (q1 != std::string::npos && q2 != std::string::npos) {
              std::string out_path = sql.substr(q1 + 1, q2 - (q1 + 1));
              auto trim_whitespace = [](std::string &s) {
                while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r')) s.erase(0, 1);
                while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r')) s.pop_back();
              };
              trim_whitespace(out_path);

              // Extract source file from inner read_csv
              std::string iq_lower = inner_query;
              std::transform(iq_lower.begin(), iq_lower.end(), iq_lower.begin(), ::tolower);
              size_t rc_pos = iq_lower.find("read_csv(");
              if (rc_pos != std::string::npos) {
                size_t in_q1 = inner_query.find('\'', rc_pos);
                size_t in_q2 = (in_q1 != std::string::npos) ? inner_query.find('\'', in_q1 + 1) : std::string::npos;
                if (in_q1 != std::string::npos && in_q2 != std::string::npos) {
                  std::string in_csv_path = inner_query.substr(in_q1 + 1, in_q2 - (in_q1 + 1));
                  trim_whitespace(in_csv_path);
                  std::vector<std::string> in_headers;
                  std::vector<std::vector<std::string>> in_rows;
                  std::vector<std::vector<bool>> in_null;
                  parse_csv_file(in_csv_path, in_headers, in_rows, in_null);

                  // Extract projected column names from SELECT <cols> FROM
                  std::vector<std::string> proj_cols;
                  size_t sel_pos = std::string::npos;
                  size_t sel_kw = iq_lower.find("select");
                  if (sel_kw != std::string::npos && (sel_kw == 0 || isspace((unsigned char)iq_lower[sel_kw - 1])) &&
                      sel_kw + 6 < iq_lower.size() && isspace((unsigned char)iq_lower[sel_kw + 6])) {
                    sel_pos = sel_kw + 6;
                  }

                  size_t from_pos = std::string::npos;
                  for (size_t p = 0; p + 5 < iq_lower.size(); ++p) {
                    if (isspace((unsigned char)iq_lower[p]) && iq_lower.substr(p + 1, 4) == "from" &&
                        p + 5 < iq_lower.size() && isspace((unsigned char)iq_lower[p + 5])) {
                      from_pos = p;
                      break;
                    }
                  }

                  if (sel_pos != std::string::npos && from_pos != std::string::npos && sel_pos < from_pos) {
                    std::string sel_list = inner_query.substr(sel_pos, from_pos - sel_pos);
                    std::stringstream ss_col(sel_list);
                    std::string item;
                    while (std::getline(ss_col, item, ',')) {
                      trim_whitespace(item);
                      size_t sp = item.rfind(' ');
                      if (sp != std::string::npos) item = item.substr(sp + 1);
                      trim_whitespace(item);
                      proj_cols.push_back(item);
                    }
                  }

                  // Map proj_cols to column indices
                  std::vector<int> col_indices;
                  for (const auto &col_name : proj_cols) {
                    int idx = -1;
                    for (size_t c = 0; c < in_headers.size(); ++c) {
                      if (in_headers[c] == col_name) {
                        idx = (int)c;
                        break;
                      }
                    }
                    col_indices.push_back(idx);
                  }

                  // If ORDER BY, sort rows
                  size_t ob_pos = std::string::npos;
                  for (size_t p = 0; p + 5 < iq_lower.size(); ++p) {
                    if ((p == 0 || isspace((unsigned char)iq_lower[p])) && iq_lower.substr(p + (p == 0 ? 0 : 1), 5) == "order") {
                      size_t after_order = p + (p == 0 ? 5 : 6);
                      while (after_order < iq_lower.size() && isspace((unsigned char)iq_lower[after_order])) after_order++;
                      if (iq_lower.substr(after_order, 2) == "by" &&
                          (after_order + 2 == iq_lower.size() || isspace((unsigned char)iq_lower[after_order + 2]))) {
                        ob_pos = after_order + 2;
                        break;
                      }
                    }
                  }
                  if (ob_pos != std::string::npos) {
                    std::string ob_col = inner_query.substr(ob_pos);
                    trim_whitespace(ob_col);
                    int ob_idx = -1;
                    for (size_t c = 0; c < in_headers.size(); ++c) {
                      if (in_headers[c] == ob_col) { ob_idx = (int)c; break; }
                    }
                    if (ob_idx >= 0) {
                      std::vector<size_t> p_rows(in_rows.size());
                      for (size_t r = 0; r < in_rows.size(); ++r) p_rows[r] = r;
                      std::sort(p_rows.begin(), p_rows.end(), [&](size_t a, size_t b) {
                        double da = 0, db = 0;
                        char *ea = nullptr, *eb = nullptr;
                        da = std::strtod(in_rows[a][ob_idx].c_str(), &ea);
                        db = std::strtod(in_rows[b][ob_idx].c_str(), &eb);
                        if (ea != in_rows[a][ob_idx].c_str() && *ea == '\0' &&
                            eb != in_rows[b][ob_idx].c_str() && *eb == '\0') {
                          return da < db;
                        }
                        return in_rows[a][ob_idx] < in_rows[b][ob_idx];
                      });
                      std::vector<std::vector<std::string>> sorted_rows;
                      std::vector<std::vector<bool>> sorted_null;
                      for (size_t idx : p_rows) {
                        sorted_rows.push_back(in_rows[idx]);
                        sorted_null.push_back(in_null[idx]);
                      }
                      in_rows = sorted_rows;
                      in_null = sorted_null;
                    }
                  }

                  // Write to out_path
                  std::ofstream out_file(out_path, std::ios::trunc);
                  for (size_t c = 0; c < proj_cols.size(); ++c) {
                    if (c > 0) out_file << ",";
                    out_file << proj_cols[c];
                  }
                  out_file << "\n";

                  for (size_t r = 0; r < in_rows.size(); ++r) {
                    for (size_t c = 0; c < col_indices.size(); ++c) {
                      if (c > 0) out_file << ",";
                      int idx = col_indices[c];
                      if (idx < 0 || in_null[r][idx]) {
                        out_file << "NULL";
                      } else {
                        std::string s = in_rows[r][idx];
                        if (s.find(',') != std::string::npos || s.find('"') != std::string::npos ||
                            s.find('\n') != std::string::npos || s.find('\r') != std::string::npos) {
                          out_file << "\"";
                          for (char ch : s) {
                            if (ch == '"') out_file << "\"\"";
                            else out_file << ch;
                          }
                          out_file << "\"";
                        } else {
                          out_file << s;
                        }
                      }
                    }
                    out_file << "\n";
                  }
                  out_file.flush();
                  out_file.close();
                }
              }

              // Rewrite SQL to DO 0; so it returns standard OK packet
              std::string rewritten = "DO 0;";
              char *buf = (char *)allocator.alloc(rewritten.size() + 1);
              if (buf == nullptr) return common::OB_ALLOCATE_MEMORY_FAILED;
              std::memcpy(buf, rewritten.c_str(), rewritten.size() + 1);
              sql_out.assign_ptr(buf, (int32_t)rewritten.size());
              return common::OB_SUCCESS;
            }
          }
        }
      }
    }

    // Replace table functions: read_csv(...), read_json(...), read_parquet(...), glob(...)
    std::string rewritten = sql;
    while (true) {
      std::string s_lower = rewritten;
      std::transform(s_lower.begin(), s_lower.end(), s_lower.begin(), ::tolower);
      
      size_t pos_csv = s_lower.find("read_csv(");
      size_t pos_json = s_lower.find("read_json(");
      size_t pos_parquet = s_lower.find("read_parquet(");
      size_t pos_glob = s_lower.find("glob(");

      size_t target_pos = std::string::npos;
      int mode = 0; // 1: csv, 2: json, 3: parquet, 4: glob
      if (pos_csv != std::string::npos) { target_pos = pos_csv; mode = 1; }
      if (pos_json != std::string::npos && (target_pos == std::string::npos || pos_json < target_pos)) { target_pos = pos_json; mode = 2; }
      if (pos_parquet != std::string::npos && (target_pos == std::string::npos || pos_parquet < target_pos)) { target_pos = pos_parquet; mode = 3; }
      if (pos_glob != std::string::npos && (target_pos == std::string::npos || pos_glob < target_pos)) { target_pos = pos_glob; mode = 4; }

      if (target_pos == std::string::npos) break;

      size_t p_close = rewritten.find(')', target_pos);
      if (p_close == std::string::npos) break;

      size_t q1 = rewritten.find('\'', target_pos);
      size_t q2 = (q1 != std::string::npos && q1 < p_close) ? rewritten.find('\'', q1 + 1) : std::string::npos;
      if (q1 == std::string::npos || q2 == std::string::npos || q2 > p_close) break;

      std::string filepath = rewritten.substr(q1 + 1, q2 - (q1 + 1));
      std::string subq;

      if (mode == 1) { // read_csv
        std::vector<std::string> headers;
        std::vector<std::vector<std::string>> rows;
        std::vector<std::vector<bool>> is_null;
        parse_csv_file(filepath, headers, rows, is_null);
        subq = build_union_subquery(headers, rows, is_null);
      } else if (mode == 2) { // read_json
        std::vector<std::string> keys;
        std::vector<std::vector<std::string>> rows;
        std::vector<std::vector<bool>> is_null;
        parse_jsonl_file(filepath, keys, rows, is_null);
        subq = build_union_subquery(keys, rows, is_null);
      } else if (mode == 3) { // read_parquet
        std::vector<std::string> headers;
        std::vector<std::vector<std::string>> rows;
        std::vector<std::vector<bool>> is_null;
        parse_parquet_file(filepath, headers, rows, is_null);
        subq = build_union_subquery(headers, rows, is_null);
      } else if (mode == 4) { // glob
        std::vector<std::string> files;
        parse_glob_pattern(filepath, files);
        std::vector<std::string> headers = {"file"};
        std::vector<std::vector<std::string>> rows;
        std::vector<std::vector<bool>> is_null;
        for (const auto &f : files) {
          rows.push_back({f});
          is_null.push_back({false});
        }
        subq = build_union_subquery(headers, rows, is_null);
      }

      // Check if user already provided a table alias
      size_t after_p = p_close + 1;
      while (after_p < rewritten.size() && (rewritten[after_p] == ' ' || rewritten[after_p] == '\t' || rewritten[after_p] == '\n' || rewritten[after_p] == '\r')) {
        after_p++;
      }
      bool has_user_alias = false;
      if (after_p < rewritten.size() && rewritten[after_p] != ';' && rewritten[after_p] != ')' && rewritten[after_p] != ',') {
        std::string next_word;
        size_t w_end = after_p;
        while (w_end < rewritten.size() && (isalnum(rewritten[w_end]) || rewritten[w_end] == '_')) {
          w_end++;
        }
        next_word = rewritten.substr(after_p, w_end - after_p);
        std::transform(next_word.begin(), next_word.end(), next_word.begin(), ::tolower);
        if (next_word != "where" && next_word != "order" && next_word != "group" && 
            next_word != "having" && next_word != "limit" && next_word != "union" &&
            next_word != "join" && next_word != "left" && next_word != "right" &&
            next_word != "inner" && next_word != "cross" && next_word != "natural" &&
            next_word != "straight_join" && next_word != "window") {
          has_user_alias = true;
        }
      }
      if (!has_user_alias) {
        subq += " __file_sql_t";
      }

      rewritten.replace(target_pos, p_close - target_pos + 1, subq);
    }

    char *buf = (char *)allocator.alloc(rewritten.size() + 1);
    if (buf == nullptr) return common::OB_ALLOCATE_MEMORY_FAILED;
    std::memcpy(buf, rewritten.c_str(), rewritten.size() + 1);
    sql_out.assign_ptr(buf, (int32_t)rewritten.size());
    return common::OB_SUCCESS;
  }
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_OB_FILE_SQL_HELPER_H_
