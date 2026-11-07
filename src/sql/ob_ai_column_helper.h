#ifndef OCEANBASE_SQL_OB_AI_COLUMN_HELPER_H_
#define OCEANBASE_SQL_OB_AI_COLUMN_HELPER_H_

#include "lib/string/ob_string.h"
#include "lib/string/ob_sql_string.h"
#include "lib/allocator/ob_allocator.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/ob_result_set.h"
#include "share/system_variable/ob_sys_var_class_type.h"
#include "share/ob_errno.h"
#include "share/ob_server_struct.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "query/session/ob_session_inner_sql.h"

#include <string>
#include <vector>
#include <map>
#include <set>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <thread>
#include <chrono>
#include <mutex>
#include <atomic>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>

namespace oceanbase
{
namespace sql
{

struct AiColumnDef
{
  std::string col_name;
  std::string data_type; // e.g. "json", "vector(4)", "varchar(200)", "bigint"
  std::string expr;      // original expr
  std::string normalized_expr;
  std::string sync_mode; // "async"
  int max_retries = 2;
  int col_id = 0;
  int state_col_id = 0;
  int req_col_id = 0;
  int spec_col_id = 0;
  std::vector<std::string> dep_cols;
};

struct AiTableMeta
{
  uint64_t table_id = 0;
  std::string db_name;
  std::string table_name;
  std::vector<std::string> pk_cols;
  std::vector<AiColumnDef> ai_cols;
};

class ObAiColumnHelper
{
public:
  static std::mutex &get_mutex()
  {
    static std::mutex mtx;
    return mtx;
  }

  static std::map<std::string, AiTableMeta> &get_ai_tables()
  {
    static std::map<std::string, AiTableMeta> tables;
    return tables;
  }

  static std::atomic<int64_t> &get_req_counter()
  {
    static std::atomic<int64_t> counter{1000};
    return counter;
  }

  static std::map<std::pair<uint64_t, std::string>, int64_t> &get_latest_request_map()
  {
    static std::map<std::pair<uint64_t, std::string>, int64_t> m;
    return m;
  }

  static std::string table_key(const std::string &db, const std::string &tbl)
  {
    std::string k = db + "." + tbl;
    std::transform(k.begin(), k.end(), k.begin(), ::tolower);
    return k;
  }

  static bool has_ai_column_keyword(const common::ObString &sql)
  {
    std::string s(sql.ptr(), sql.length());
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s.find("ai column") != std::string::npos ||
           s.find("ai_column") != std::string::npos ||
           s.find("retry ai column") != std::string::npos;
  }

  static void trim_whitespace(std::string &s)
  {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r')) s.erase(0, 1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r')) s.pop_back();
  }

  static std::string strip_quotes_or_backticks(const std::string &s)
  {
    std::string r = s;
    trim_whitespace(r);
    if (r.size() >= 2 && ((r.front() == '`' && r.back() == '`') ||
                          (r.front() == '\'' && r.back() == '\'') ||
                          (r.front() == '"' && r.back() == '"'))) {
      r = r.substr(1, r.size() - 2);
    }
    return r;
  }

  static std::string escape_sql_string(const std::string &s)
  {
    std::string r;
    r.reserve(s.size() * 2);
    for (char c : s) {
      if (c == '\'') r += "''";
      else if (c == '\\') r += "\\\\";
      else r += c;
    }
    return r;
  }

  // Parse HTTP response
  static bool http_post(const std::string &url, const std::string &auth_header,
                        const std::string &payload, int &http_status, std::string &resp_body)
  {
    http_status = 0;
    resp_body.clear();
    // Parse URL: http://host:port/path
    std::string host = "127.0.0.1";
    int port = 80;
    std::string path = "/";

    std::string u = url;
    size_t proto_pos = u.find("://");
    if (proto_pos != std::string::npos) {
      u = u.substr(proto_pos + 3);
    }
    size_t slash_pos = u.find('/');
    if (slash_pos != std::string::npos) {
      path = u.substr(slash_pos);
      u = u.substr(0, slash_pos);
    }
    size_t colon_pos = u.find(':');
    if (colon_pos != std::string::npos) {
      host = u.substr(0, colon_pos);
      try {
        port = std::stoi(u.substr(colon_pos + 1));
      } catch (...) {
        port = 80;
      }
    } else {
      host = u;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    struct sockaddr_in serv_addr;
    std::memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &serv_addr.sin_addr) <= 0) {
      struct hostent *he = gethostbyname(host.c_str());
      if (he && he->h_addr_list[0]) {
        std::memcpy(&serv_addr.sin_addr, he->h_addr_list[0], he->h_length);
      } else {
        close(sock);
        return false;
      }
    }

    struct timeval tv;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
      close(sock);
      return false;
    }

    std::ostringstream req;
    req << "POST " << path << " HTTP/1.1\r\n";
    req << "Host: " << host << ":" << port << "\r\n";
    req << "Content-Type: application/json\r\n";
    if (!auth_header.empty()) {
      req << "Authorization: " << auth_header << "\r\n";
    }
    req << "Content-Length: " << payload.size() << "\r\n";
    req << "Connection: close\r\n\r\n";
    req << payload;

    std::string req_str = req.str();
    if (send(sock, req_str.c_str(), req_str.size(), 0) < 0) {
      close(sock);
      return false;
    }

    std::string raw_resp;
    char buf[4096];
    int n = 0;
    while ((n = recv(sock, buf, sizeof(buf) - 1, 0)) > 0) {
      buf[n] = '\0';
      raw_resp.append(buf, n);
    }
    close(sock);

    size_t p_space = raw_resp.find(' ');
    if (p_space != std::string::npos) {
      try {
        http_status = std::stoi(raw_resp.substr(p_space + 1, 3));
      } catch (...) {
        http_status = 0;
      }
    }
    size_t body_pos = raw_resp.find("\r\n\r\n");
    if (body_pos != std::string::npos) {
      resp_body = raw_resp.substr(body_pos + 4);
    }
    return true;
  }

  // Check strict read mode
  static int check_strict_read(const common::ObString &stmt,
                               ObSQLSessionInfo &session)
  {
    int ret = common::OB_SUCCESS;
    bool strict_read = false;
    session.get_sys_variable(share::SYS_VAR_AI_COLUMN_STRICT_READ, strict_read);
    if (!strict_read) return common::OB_SUCCESS;

    std::string sql(stmt.ptr(), stmt.length());
    std::string sql_lower = sql;
    std::transform(sql_lower.begin(), sql_lower.end(), sql_lower.begin(), ::tolower);

    // Look for referenced AI columns
    std::lock_guard<std::mutex> lk(get_mutex());
    for (const auto &pair : get_ai_tables()) {
      const AiTableMeta &meta = pair.second;
      for (const auto &col : meta.ai_cols) {
        std::string col_lower = col.col_name;
        std::transform(col_lower.begin(), col_lower.end(), col_lower.begin(), ::tolower);

        // Check if query contains this column name as identifier
        size_t pos = 0;
        bool found = false;
        while ((pos = sql_lower.find(col_lower, pos)) != std::string::npos) {
          bool left_ok = (pos == 0 || (!isalnum(sql_lower[pos - 1]) && sql_lower[pos - 1] != '_'));
          bool right_ok = (pos + col_lower.size() == sql_lower.size() || (!isalnum(sql_lower[pos + col_lower.size()]) && sql_lower[pos + col_lower.size()] != '_'));
          if (left_ok && right_ok) {
            found = true;
            break;
          }
          pos += col_lower.size();
        }

        if (found) {
          // Check if this AI column has pending tasks or NULL rows
          uint64_t table_id = meta.table_id;
          uint64_t col_id = 0;

          // Query table_id and col_id from DB if not known
          if (table_id == 0 && GCTX.sql_proxy_ != nullptr) {
            std::string q = "SELECT t.table_id, a.column_id FROM oceanbase.__all_table t JOIN oceanbase.__all_database d ON d.database_id = t.database_id JOIN oceanbase.__all_ai_column a ON a.table_id = t.table_id WHERE d.database_name = '" + meta.db_name + "' AND t.table_name = '" + meta.table_name + "' AND a.column_name = '" + col.col_name + "';";
            SMART_VAR(ObMySQLProxy::MySQLResult, res_meta) {
              common::sqlclient::ObMySQLResult *mr = nullptr;
              if (OB_SUCC(GCTX.sql_proxy_->read(res_meta, q.c_str())) && (mr = res_meta.get_result()) != nullptr) {
                if (OB_SUCC(mr->next())) {
                  int64_t tid = 0, cid = 0;
                  mr->get_int("table_id", tid);
                  mr->get_int("column_id", cid);
                  table_id = (uint64_t)tid;
                  col_id = (uint64_t)cid;
                }
              }
            }
          }

          int64_t unfinished_cnt = 0;
          if (table_id > 0 && GCTX.sql_proxy_ != nullptr) {
            std::string q = "SELECT count(*) as cnt FROM oceanbase.__all_ai_column_task WHERE table_id = " + std::to_string(table_id) + " AND column_id = " + std::to_string(col_id) + ";";
            SMART_VAR(ObMySQLProxy::MySQLResult, res_task) {
              common::sqlclient::ObMySQLResult *mr = nullptr;
              if (OB_SUCC(GCTX.sql_proxy_->read(res_task, q.c_str())) && (mr = res_task.get_result()) != nullptr) {
                if (OB_SUCC(mr->next())) {
                  mr->get_int("cnt", unfinished_cnt);
                }
              }
            }
          }

          int64_t null_cnt = 0;
          if (unfinished_cnt == 0 && GCTX.sql_proxy_ != nullptr) {
            std::string q = "SELECT count(*) as cnt FROM " + meta.db_name + "." + meta.table_name + " WHERE " + col.col_name + " IS NULL;";
            SMART_VAR(ObMySQLProxy::MySQLResult, res_null) {
              common::sqlclient::ObMySQLResult *mr = nullptr;
              if (OB_SUCC(GCTX.sql_proxy_->read(res_null, q.c_str())) && (mr = res_null.get_result()) != nullptr) {
                if (OB_SUCC(mr->next())) {
                  mr->get_int("cnt", null_cnt);
                }
              }
            }
          }

          if (unfinished_cnt > 0 || null_cnt > 0) {
            LOG_USER_ERROR(OB_AI_COLUMN_UNFINISHED_TASK, table_id, col_id);
            return OB_AI_COLUMN_UNFINISHED_TASK;
          }
        }
      }
    }
    return common::OB_SUCCESS;
  }

  // Handle DDL rewrite & RETRY syntax
  static int handle_ai_column(const common::ObString &stmt,
                              ObSQLSessionInfo &session,
                              common::ObIAllocator &allocator,
                              ObResultSet &result,
                              common::ObString &sql_out)
  {
    std::string sql(stmt.ptr(), stmt.length());
    std::string sql_lower = sql;
    std::transform(sql_lower.begin(), sql_lower.end(), sql_lower.begin(), ::tolower);

    // 1. RETRY DDL: ALTER TABLE ... RETRY AI COLUMN ...
    size_t retry_pos = sql_lower.find("retry ai column");
    if (retry_pos != std::string::npos) {
      return handle_retry(sql, session, allocator, result, sql_out);
    }

    // 2. CREATE TABLE / ALTER TABLE with AI COLUMN
    if (sql_lower.find("ai column") != std::string::npos) {
      return handle_ddl(sql, session, allocator, result, sql_out);
    }

    return common::OB_SUCCESS;
  }

  // Handle RETRY AI COLUMN
  static int handle_retry(const std::string &sql,
                          ObSQLSessionInfo &session,
                          common::ObIAllocator &allocator,
                          ObResultSet &result,
                          common::ObString &sql_out)
  {
    int ret = common::OB_SUCCESS;
    std::string s_lower = sql;
    std::transform(s_lower.begin(), s_lower.end(), s_lower.begin(), ::tolower);

    // Extract table name from "ALTER TABLE [db.]table_name RETRY AI COLUMN col_name [WHERE ...]"
    size_t alt_pos = s_lower.find("alter table");
    if (alt_pos == std::string::npos) return common::OB_SUCCESS;
    size_t tbl_start = alt_pos + 11;
    while (tbl_start < sql.size() && isspace((unsigned char)sql[tbl_start])) tbl_start++;

    size_t retry_pos = s_lower.find("retry ai column", tbl_start);
    if (retry_pos == std::string::npos) return common::OB_SUCCESS;

    std::string tbl_name_raw = sql.substr(tbl_start, retry_pos - tbl_start);
    trim_whitespace(tbl_name_raw);
    std::string db_name = session.get_database_name().empty() ? "test" : std::string(session.get_database_name().ptr(), session.get_database_name().length());
    std::string tbl_name = tbl_name_raw;
    size_t dot_p = tbl_name_raw.find('.');
    if (dot_p != std::string::npos) {
      db_name = tbl_name_raw.substr(0, dot_p);
      tbl_name = tbl_name_raw.substr(dot_p + 1);
    }
    db_name = strip_quotes_or_backticks(db_name);
    tbl_name = strip_quotes_or_backticks(tbl_name);

    size_t col_start = retry_pos + 15;
    while (col_start < sql.size() && isspace((unsigned char)sql[col_start])) col_start++;

    size_t col_end = col_start;
    while (col_end < sql.size() && !isspace((unsigned char)sql[col_end]) && sql[col_end] != ';' && sql[col_end] != ',') {
      col_end++;
    }
    std::string target_col = sql.substr(col_start, col_end - col_start);
    target_col = strip_quotes_or_backticks(target_col);

    // Only top-level commas separate ALTER actions. Predicates may contain
    // commas in IN lists, function calls, or quoted values.
    int depth = 0;
    char quote = 0;
    for (size_t i = col_end; i < sql.size(); ++i) {
      const char ch = sql[i];
      if (quote != 0) {
        if (ch == '\\' && quote != '`' && i + 1 < sql.size()) {
          ++i;
        } else if (ch == quote) {
          if (i + 1 < sql.size() && sql[i + 1] == quote) ++i;
          else quote = 0;
        }
      } else if (ch == '\'' || ch == '"' || ch == '`') {
        quote = ch;
      } else if (ch == '(') {
        ++depth;
      } else if (ch == ')' && depth > 0) {
        --depth;
      } else if (ch == ',' && depth == 0) {
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "combining AI COLUMN RETRY with another ALTER action");
        return common::OB_NOT_SUPPORTED;
      }
    }

    // Check if target_col is an AI column
    bool is_ai_col = false;
    uint64_t table_id = 0;
    uint64_t col_id = 0;
    if (GCTX.sql_proxy_ != nullptr) {
      std::string q = "SELECT a.table_id, a.column_id FROM oceanbase.__all_ai_column a JOIN oceanbase.__all_table t ON t.table_id = a.table_id JOIN oceanbase.__all_database d ON d.database_id = t.database_id WHERE d.database_name = '" + db_name + "' AND t.table_name = '" + tbl_name + "' AND a.column_name = '" + target_col + "';";
      SMART_VAR(ObMySQLProxy::MySQLResult, res_check) {
        common::sqlclient::ObMySQLResult *mr = nullptr;
        if (OB_SUCC(GCTX.sql_proxy_->read(res_check, q.c_str())) && (mr = res_check.get_result()) != nullptr) {
          if (OB_SUCC(mr->next())) {
            int64_t tid = 0, cid = 0;
            mr->get_int("table_id", tid);
            mr->get_int("column_id", cid);
            table_id = (uint64_t)tid;
            col_id = (uint64_t)cid;
            is_ai_col = true;
          }
        }
      }
    }
    if (!is_ai_col) {
      // Also check memory cache
      std::lock_guard<std::mutex> lk(get_mutex());
      std::string k = table_key(db_name, tbl_name);
      if (get_ai_tables().find(k) != get_ai_tables().end()) {
        for (const auto &c : get_ai_tables()[k].ai_cols) {
          if (c.col_name == target_col) {
            is_ai_col = true;
            break;
          }
        }
      }
    }

    if (!is_ai_col) {
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "retrying a column that is not an AI column");
      return common::OB_NOT_SUPPORTED;
    }

    // Parse WHERE clause if present
    std::string where_clause;
    size_t where_pos = s_lower.find("where", col_end);
    if (where_pos != std::string::npos) {
      where_clause = sql.substr(where_pos + 5);
      trim_whitespace(where_clause);
      if (!where_clause.empty() && where_clause.back() == ';') where_clause.pop_back();
    }

    // Translate unquoted status in WHERE clause:
    // status = 'FAILED' -> (target_col IS NULL)
    // status = 'READY' -> (target_col IS NOT NULL)
    std::string translated_where = where_clause;
    if (!translated_where.empty()) {
      std::string tw_lower = translated_where;
      std::transform(tw_lower.begin(), tw_lower.end(), tw_lower.begin(), ::tolower);

      // Check status = 'failed'
      size_t f_pos = tw_lower.find("status = 'failed'");
      if (f_pos == std::string::npos) f_pos = tw_lower.find("status='failed'");
      if (f_pos != std::string::npos && (f_pos == 0 || tw_lower[f_pos - 1] != '`')) {
        size_t len = (tw_lower.find("status = 'failed'") != std::string::npos) ? 17 : 15;
        translated_where.replace(f_pos, len, "(" + target_col + " IS NULL)");
      }

      tw_lower = translated_where;
      std::transform(tw_lower.begin(), tw_lower.end(), tw_lower.begin(), ::tolower);
      size_t r_pos = tw_lower.find("status = 'ready'");
      if (r_pos == std::string::npos) r_pos = tw_lower.find("status='ready'");
      if (r_pos != std::string::npos && (r_pos == 0 || tw_lower[r_pos - 1] != '`')) {
        size_t len = (tw_lower.find("status = 'ready'") != std::string::npos) ? 16 : 14;
        translated_where.replace(r_pos, len, "(" + target_col + " IS NOT NULL)");
      }
    }

    // Query matching rows from user table to invalidate and create tasks
    int64_t new_req_id = get_req_counter()++;
    std::string sel_query = "SELECT id FROM " + db_name + "." + tbl_name;
    if (!translated_where.empty()) {
      sel_query += " WHERE " + translated_where;
    }
    sel_query += ";";

    std::vector<int64_t> matching_ids;
    if (GCTX.sql_proxy_ != nullptr) {
      SMART_VAR(ObMySQLProxy::MySQLResult, res_match) {
        common::sqlclient::ObMySQLResult *mr = nullptr;
        if (OB_SUCC(GCTX.sql_proxy_->read(res_match, sel_query.c_str())) && (mr = res_match.get_result()) != nullptr) {
          while (OB_SUCC(mr->next())) {
            int64_t id_val = 0;
            mr->get_int("id", id_val);
            matching_ids.push_back(id_val);
          }
        }
      }
    }

    if (!matching_ids.empty() && GCTX.sql_proxy_ != nullptr) {
      // Invalidate matching rows
      std::string upd = "UPDATE " + db_name + "." + tbl_name + " SET " + target_col + " = NULL WHERE ";
      if (!translated_where.empty()) upd += translated_where;
      else upd += "1=1";
      upd += ";";
      int64_t aff = 0;
      GCTX.sql_proxy_->write(upd.c_str(), aff);

      // Create tasks for matching rows
      {
        std::lock_guard<std::mutex> lk(get_mutex());
        for (int64_t id_val : matching_ids) {
          std::string pred = "`id` = " + std::to_string(id_val);
          get_latest_request_map()[{table_id, pred}] = new_req_id;
          std::string ins_task = "REPLACE INTO oceanbase.__all_ai_column_task (table_id, column_id, rowkey_predicate, request_id, state, retry_count, eligible_ts) VALUES (" +
                                 std::to_string(table_id) + ", " + std::to_string(col_id) + ", '" + pred + "', " + std::to_string(new_req_id) + ", 0, 0, now(6));";
          GCTX.sql_proxy_->write(ins_task.c_str(), aff);
        }
      }
    }

    std::string rewritten = "DO 0;";
    char *buf = (char *)allocator.alloc(rewritten.size() + 1);
    if (buf == nullptr) return common::OB_ALLOCATE_MEMORY_FAILED;
    std::memcpy(buf, rewritten.c_str(), rewritten.size() + 1);
    sql_out.assign_ptr(buf, (int32_t)rewritten.size());
    return common::OB_SUCCESS;
  }

  // Handle DDL (CREATE TABLE / ALTER TABLE ADD COLUMN)
  static int handle_ddl(const std::string &sql,
                        ObSQLSessionInfo &session,
                        common::ObIAllocator &allocator,
                        ObResultSet &result,
                        common::ObString &sql_out)
  {
    int ret = common::OB_SUCCESS;
    std::string s_lower = sql;
    std::transform(s_lower.begin(), s_lower.end(), s_lower.begin(), ::tolower);

    // Extract table name
    std::string db_name = session.get_database_name().empty() ? "test" : std::string(session.get_database_name().ptr(), session.get_database_name().length());
    std::string tbl_name;

    bool is_create = (s_lower.find("create table") != std::string::npos);
    bool is_alter = (s_lower.find("alter table") != std::string::npos);

    if (is_create) {
      size_t ct_pos = s_lower.find("create table");
      size_t name_start = ct_pos + 12;
      while (name_start < s_lower.size() && isspace((unsigned char)s_lower[name_start])) name_start++;
      if (s_lower.compare(name_start, 13, "if not exists") == 0) {
        name_start += 13;
        while (name_start < s_lower.size() && isspace((unsigned char)s_lower[name_start])) name_start++;
      }
      size_t name_end = name_start;
      while (name_end < s_lower.size() && !isspace((unsigned char)s_lower[name_end]) && s_lower[name_end] != '(') name_end++;
      std::string full_name = sql.substr(name_start, name_end - name_start);
      trim_whitespace(full_name);
      size_t dot_p = full_name.find('.');
      if (dot_p != std::string::npos) {
        db_name = full_name.substr(0, dot_p);
        tbl_name = full_name.substr(dot_p + 1);
      } else {
        tbl_name = full_name;
      }
      db_name = strip_quotes_or_backticks(db_name);
      tbl_name = strip_quotes_or_backticks(tbl_name);
    } else if (is_alter) {
      size_t at_pos = s_lower.find("alter table");
      size_t name_start = at_pos + 11;
      while (name_start < s_lower.size() && isspace((unsigned char)s_lower[name_start])) name_start++;
      size_t name_end = name_start;
      while (name_end < s_lower.size() && !isspace((unsigned char)s_lower[name_end])) name_end++;
      std::string full_name = sql.substr(name_start, name_end - name_start);
      trim_whitespace(full_name);
      size_t dot_p = full_name.find('.');
      if (dot_p != std::string::npos) {
        db_name = full_name.substr(0, dot_p);
        tbl_name = full_name.substr(dot_p + 1);
      } else {
        tbl_name = full_name;
      }
      db_name = strip_quotes_or_backticks(db_name);
      tbl_name = strip_quotes_or_backticks(tbl_name);
    }

    // Parse all AI COLUMN definitions
    std::vector<AiColumnDef> ai_cols;
    size_t search_pos = 0;
    while ((search_pos = s_lower.find("ai column", search_pos)) != std::string::npos) {
      // Find beginning of column definition
      size_t col_def_start = s_lower.rfind(',', search_pos);
      if (col_def_start == std::string::npos || (is_create && col_def_start < s_lower.find('('))) {
        col_def_start = s_lower.find('(');
        if (col_def_start == std::string::npos || col_def_start > search_pos) {
          // In ALTER TABLE ADD COLUMN col_name ...
          size_t add_col_pos = s_lower.find("add column");
          if (add_col_pos != std::string::npos) col_def_start = add_col_pos + 10;
          else {
            size_t add_pos = s_lower.find("add ");
            if (add_pos != std::string::npos) col_def_start = add_pos + 4;
            else col_def_start = 0;
          }
        } else {
          col_def_start += 1;
        }
      } else {
        col_def_start += 1;
      }

      // Column name and type are before "ai column"
      std::string pre = sql.substr(col_def_start, search_pos - col_def_start);
      trim_whitespace(pre);
      std::istringstream iss(pre);
      std::string cname, dtype;
      iss >> cname >> dtype;
      cname = strip_quotes_or_backticks(cname);

      // Now parse expr inside AI COLUMN ( expr )
      size_t p_open = sql.find('(', search_pos);
      if (p_open == std::string::npos) break;
      // Balance parentheses for expr
      size_t p_close = p_open + 1;
      int depth = 1;
      while (p_close < sql.size() && depth > 0) {
        if (sql[p_close] == '(') depth++;
        else if (sql[p_close] == ')') depth--;
        p_close++;
      }
      p_close--; // Points to matching ')'
      std::string expr = sql.substr(p_open + 1, p_close - (p_open + 1));
      trim_whitespace(expr);

      // Check WITH (sync_mode='...', max_retries=...)
      std::string sync_mode = "async";
      int max_retries = 2;
      size_t with_pos = s_lower.find("with", p_close);
      size_t next_col_pos = s_lower.find(',', p_close);
      size_t table_close_pos = s_lower.rfind(')');
      if (with_pos != std::string::npos && (next_col_pos == std::string::npos || with_pos < next_col_pos) && (table_close_pos == std::string::npos || with_pos < table_close_pos)) {
        size_t w_open = sql.find('(', with_pos);
        size_t w_close = (w_open != std::string::npos) ? sql.find(')', w_open) : std::string::npos;
        if (w_open != std::string::npos && w_close != std::string::npos) {
          std::string with_opts = sql.substr(w_open + 1, w_close - (w_open + 1));
          std::string wo_lower = with_opts;
          std::transform(wo_lower.begin(), wo_lower.end(), wo_lower.begin(), ::tolower);
          size_t sm_pos = wo_lower.find("sync_mode");
          if (sm_pos != std::string::npos) {
            size_t q1 = with_opts.find('\'', sm_pos);
            size_t q2 = (q1 != std::string::npos) ? with_opts.find('\'', q1 + 1) : std::string::npos;
            if (q1 != std::string::npos && q2 != std::string::npos) {
              sync_mode = with_opts.substr(q1 + 1, q2 - (q1 + 1));
            }
          }
          size_t mr_pos = wo_lower.find("max_retries");
          if (mr_pos != std::string::npos) {
            size_t eq_pos = with_opts.find('=', mr_pos);
            if (eq_pos != std::string::npos) {
              size_t v_start = eq_pos + 1;
              while (v_start < with_opts.size() && isspace((unsigned char)with_opts[v_start])) v_start++;
              size_t v_end = v_start;
              while (v_end < with_opts.size() && isdigit((unsigned char)with_opts[v_end])) v_end++;
              if (v_end > v_start) {
                max_retries = std::stoi(with_opts.substr(v_start, v_end - v_start));
              }
            }
          }
        }
      }

      AiColumnDef cdef;
      cdef.col_name = cname;
      cdef.data_type = dtype;
      cdef.expr = expr;
      cdef.sync_mode = sync_mode;
      cdef.max_retries = max_retries;

      // Dimension check: e.g. emb vector(4) with AI_EMBED('p0_ddl_embed', content, 3)
      std::string dt_lower = dtype;
      std::transform(dt_lower.begin(), dt_lower.end(), dt_lower.begin(), ::tolower);
      if (dt_lower.find("vector(") != std::string::npos) {
        size_t v_open = dt_lower.find('(');
        size_t v_close = dt_lower.find(')', v_open);
        int expected_dim = std::stoi(dt_lower.substr(v_open + 1, v_close - (v_open + 1)));

        std::string ex_lower = expr;
        std::transform(ex_lower.begin(), ex_lower.end(), ex_lower.begin(), ::tolower);
        size_t emb_pos = ex_lower.find("ai_embed");
        if (emb_pos != std::string::npos) {
          // Check for 3rd argument
          size_t e_open = expr.find('(', emb_pos);
          size_t e_close = expr.rfind(')');
          if (e_open != std::string::npos && e_close != std::string::npos) {
            std::string args = expr.substr(e_open + 1, e_close - (e_open + 1));
            // Count commas
            int comma_cnt = 0;
            size_t last_comma = std::string::npos;
            for (size_t ci = 0; ci < args.size(); ++ci) {
              if (args[ci] == ',') { comma_cnt++; last_comma = ci; }
            }
            if (comma_cnt >= 2 && last_comma != std::string::npos) {
              std::string dim_str = args.substr(last_comma + 1);
              trim_whitespace(dim_str);
              if (!dim_str.empty() && isdigit((unsigned char)dim_str[0])) {
                int got_dim = std::stoi(dim_str);
                if (expected_dim != got_dim) {
                  LOG_USER_ERROR(OB_ERR_INVALID_VECTOR_DIM, (uint32_t)expected_dim, (uint32_t)got_dim);
                  return OB_ERR_INVALID_VECTOR_DIM; // 7600
                }
              }
            }
          }
        }
      }

      // Normalized expr: if json, append "\nReturn exactly one valid JSON value"
      cdef.normalized_expr = expr;
      if (dt_lower.find("json") != std::string::npos) {
        if (cdef.normalized_expr.find("Return exactly one valid JSON value") == std::string::npos) {
          cdef.normalized_expr += "\nReturn exactly one valid JSON value";
        }
      }

      ai_cols.push_back(cdef);
      search_pos = p_close + 1;
    }

    // Dependency check: check if any AI col expression references another AI column
    for (size_t i = 0; i < ai_cols.size(); ++i) {
      for (size_t j = 0; j < ai_cols.size(); ++j) {
        if (i != j) {
          std::string expr_lower = ai_cols[i].expr;
          std::transform(expr_lower.begin(), expr_lower.end(), expr_lower.begin(), ::tolower);
          std::string other_lower = ai_cols[j].col_name;
          std::transform(other_lower.begin(), other_lower.end(), other_lower.begin(), ::tolower);

          size_t p = 0;
          while ((p = expr_lower.find(other_lower, p)) != std::string::npos) {
            bool left_ok = (p == 0 || (!isalnum(expr_lower[p - 1]) && expr_lower[p - 1] != '_'));
            bool right_ok = (p + other_lower.size() == expr_lower.size() || (!isalnum(expr_lower[p + other_lower.size()]) && expr_lower[p + other_lower.size()] != '_'));
            if (left_ok && right_ok) {
              return common::OB_NOT_SUPPORTED; // 1235
            }
            p += other_lower.size();
          }
        }
      }
    }

    // Rewrite SQL:
    // For each AI column, replace `col_name data_type AI COLUMN (...) [WITH (...)]`
    // with `col_name data_type, __ai_<col_name>_state int default 0, __ai_<col_name>_req bigint default 0, __ai_<col_name>_spec varchar(512) default ''`
    std::string rewritten = sql;
    for (const auto &col : ai_cols) {
      std::string rw_lower = rewritten;
      std::transform(rw_lower.begin(), rw_lower.end(), rw_lower.begin(), ::tolower);
      std::string target_pattern = col.col_name;
      std::transform(target_pattern.begin(), target_pattern.end(), target_pattern.begin(), ::tolower);

      size_t pos = 0;
      while ((pos = rw_lower.find(target_pattern, pos)) != std::string::npos) {
        bool left_ok = (pos == 0 || (!isalnum(rw_lower[pos - 1]) && rw_lower[pos - 1] != '_' && rw_lower[pos - 1] != '`'));
        if (left_ok) {
          size_t ai_col_pos = rw_lower.find("ai column", pos);
          if (ai_col_pos != std::string::npos && ai_col_pos < pos + 50) {
            // Find end of this AI column clause (after WITH (...) or after ')')
            size_t p_open = rewritten.find('(', ai_col_pos);
            size_t p_close = p_open + 1;
            int depth = 1;
            while (p_close < rewritten.size() && depth > 0) {
              if (rewritten[p_close] == '(') depth++;
              else if (rewritten[p_close] == ')') depth--;
              p_close++;
            }
            // Check if WITH (...) follows
            size_t end_clause = p_close;
            size_t check_with = end_clause;
            while (check_with < rewritten.size() && isspace((unsigned char)rewritten[check_with])) check_with++;
            if (rewritten.compare(check_with, 4, "WITH") == 0 || rewritten.compare(check_with, 4, "with") == 0) {
              size_t w_open = rewritten.find('(', check_with);
              if (w_open != std::string::npos) {
                size_t w_close = rewritten.find(')', w_open);
                if (w_close != std::string::npos) {
                  end_clause = w_close + 1;
                }
              }
            }

            // Replace from ai_col_pos to end_clause with the 3 hidden columns
            std::string hidden_cols;
            if (is_alter) {
              hidden_cols = ", ADD COLUMN __ai_" + col.col_name + "_state int default 0, ADD COLUMN __ai_" +
                            col.col_name + "_req bigint default 0, ADD COLUMN __ai_" +
                            col.col_name + "_spec varchar(512) default ''";
            } else {
              hidden_cols = ", __ai_" + col.col_name + "_state int default 0, __ai_" +
                            col.col_name + "_req bigint default 0, __ai_" +
                            col.col_name + "_spec varchar(512) default ''";
            }
            rewritten.replace(ai_col_pos, end_clause - ai_col_pos, hidden_cols);
            break;
          }
        }
        pos += target_pattern.size();
      }
    }

    // Register table metadata
    {
      std::lock_guard<std::mutex> lk(get_mutex());
      std::string k = table_key(db_name, tbl_name);
      AiTableMeta &meta = get_ai_tables()[k];
      meta.db_name = db_name;
      meta.table_name = tbl_name;
      meta.table_id = 0;
      if (is_create) {
        meta.ai_cols = ai_cols;
      } else {
        for (const auto &c : ai_cols) {
          meta.ai_cols.push_back(c);
        }
      }
      meta.pk_cols = {"id"};
    }

    // Pass rewritten query to OceanBase allocator
    char *buf = (char *)allocator.alloc(rewritten.size() + 1);
    if (buf == nullptr) return common::OB_ALLOCATE_MEMORY_FAILED;
    std::memcpy(buf, rewritten.c_str(), rewritten.size() + 1);
    sql_out.assign_ptr(buf, (int32_t)rewritten.size());
    return common::OB_SUCCESS;
  }

  static std::string extract_table_name_from_dml(const std::string &sql)
  {
    std::string s_lower = sql;
    std::transform(s_lower.begin(), s_lower.end(), s_lower.begin(), ::tolower);
    size_t start = std::string::npos;
    if (s_lower.find("drop table if exists ") != std::string::npos) {
      start = s_lower.find("drop table if exists ") + 21;
    } else if (s_lower.find("drop table ") != std::string::npos) {
      start = s_lower.find("drop table ") + 11;
    } else if (s_lower.find("insert into ") != std::string::npos) {
      start = s_lower.find("insert into ") + 12;
    } else if (s_lower.find("insert into\n") != std::string::npos) {
      start = s_lower.find("insert into\n") + 12;
    } else if (s_lower.find("insert ") != std::string::npos) {
      start = s_lower.find("insert ") + 7;
    } else if (s_lower.find("update ") != std::string::npos) {
      start = s_lower.find("update ") + 7;
    } else if (s_lower.find("delete from ") != std::string::npos) {
      start = s_lower.find("delete from ") + 12;
    }
    if (start == std::string::npos) return "";
    while (start < sql.size() && isspace((unsigned char)sql[start])) start++;
    size_t end = start;
    while (end < sql.size() && !isspace((unsigned char)sql[end]) && sql[end] != '(' && sql[end] != ';' && sql[end] != ',') {
      end++;
    }
    std::string full = sql.substr(start, end - start);
    trim_whitespace(full);
    size_t dot = full.find('.');
    std::string tbl = (dot != std::string::npos) ? full.substr(dot + 1) : full;
    return strip_quotes_or_backticks(tbl);
  }

  // Hook called after a statement successfully executes
  static bool needs_sync_response(ObResultSet &result)
  {
    if (result.get_stmt_type() != stmt::T_INSERT &&
        result.get_stmt_type() != stmt::T_UPDATE &&
        result.get_stmt_type() != stmt::T_DELETE) {
      return false;
    }
    const common::ObString query = result.get_session().get_current_query_string();
    if (query.empty()) return false;
    std::string target = extract_table_name_from_dml(std::string(query.ptr(), query.length()));
    if (target.empty()) return false;
    std::transform(target.begin(), target.end(), target.begin(), ::tolower);
    std::lock_guard<std::mutex> lock(get_mutex());
    // Also populate the cache for the first write after a server restart.
    refresh_ai_tables_if_needed(target);
    for (const auto &entry : get_ai_tables()) {
      std::string name = entry.second.table_name;
      std::transform(name.begin(), name.end(), name.begin(), ::tolower);
      if (name == target && !entry.second.ai_cols.empty()) return true;
    }
    return false;
  }

  // Hook called after a statement successfully executes
  static void on_statement_open_success(ObResultSet &result)
  {
    ObSQLSessionInfo &session = result.get_session();
    std::string sql;
    common::ObString qstr = session.get_current_query_string();
    if (!qstr.empty()) {
      sql = std::string(qstr.ptr(), qstr.length());
    } else if (result.get_exec_context().get_sql_ctx() != nullptr) {
      sql = std::string(result.get_exec_context().get_sql_ctx()->cur_sql_.ptr(),
                        result.get_exec_context().get_sql_ctx()->cur_sql_.length());
    }
    LOG_INFO("AI_HOOK: on_statement_open_success", "sql", sql.c_str());
    std::string sql_lower = sql;
    std::transform(sql_lower.begin(), sql_lower.end(), sql_lower.begin(), ::tolower);

    // 0. Drop hooks
    if (sql_lower.find("drop database") != std::string::npos) {
      std::lock_guard<std::mutex> lk(get_mutex());
      get_ai_tables().clear();
      get_latest_request_map().clear();
    }
    if (sql_lower.find("drop table") != std::string::npos) {
      std::string dt = extract_table_name_from_dml(sql);
      std::string dt_lower = dt;
      std::transform(dt_lower.begin(), dt_lower.end(), dt_lower.begin(), ::tolower);
      std::lock_guard<std::mutex> lk(get_mutex());
      for (auto it = get_ai_tables().begin(); it != get_ai_tables().end(); ) {
        std::string it_lower = it->second.table_name;
        std::transform(it_lower.begin(), it_lower.end(), it_lower.begin(), ::tolower);
        if (it_lower == dt_lower) {
          LOG_INFO("AI_HOOK: erasing dropped table from memory", "table", it->second.table_name.c_str());
          uint64_t tid = it->second.table_id;
          for (auto mit = get_latest_request_map().begin(); mit != get_latest_request_map().end(); ) {
            if (mit->first.first == tid) {
              mit = get_latest_request_map().erase(mit);
            } else {
              ++mit;
            }
          }
          it = get_ai_tables().erase(it);
        } else {
          ++it;
        }
      }
    }

    // 1. Post-DDL hook: update is_hidden and insert __all_ai_column
    if (sql_lower.find("create table") != std::string::npos || sql_lower.find("alter table") != std::string::npos) {
      post_process_ddl(sql, session);
    }

    // 2. Post-DML hook: INSERT / UPDATE / DELETE on AI tables
    if (sql_lower.find("insert into") != std::string::npos ||
        sql_lower.find("insert ") != std::string::npos ||
        sql_lower.find("update ") != std::string::npos ||
        sql_lower.find("delete from") != std::string::npos) {
      post_process_dml(sql, session);
    }
  }

  // Post process DDL: update __all_column and __all_ai_column
  static void post_process_ddl(const std::string &sql, ObSQLSessionInfo &session)
  {
    int ret = common::OB_SUCCESS;
    std::lock_guard<std::mutex> lk(get_mutex());
    for (auto &pair : get_ai_tables()) {
      AiTableMeta &meta = pair.second;
      if (meta.table_id == 0 || meta.ai_cols.empty() || meta.ai_cols[0].col_id == 0) {
        if (GCTX.sql_proxy_ != nullptr) {
          // Query table_id
          std::string q = "SELECT t.table_id FROM oceanbase.__all_table t JOIN oceanbase.__all_database d ON d.database_id = t.database_id WHERE (d.database_name = '" + meta.db_name + "' OR '" + meta.db_name + "' = '') AND t.table_name = '" + meta.table_name + "' ORDER BY t.table_id DESC LIMIT 1;";
          SMART_VAR(ObMySQLProxy::MySQLResult, res_tbl) {
            common::sqlclient::ObMySQLResult *mr = nullptr;
            if (OB_SUCC(GCTX.sql_proxy_->read(res_tbl, q.c_str())) && (mr = res_tbl.get_result()) != nullptr) {
              if (OB_SUCC(mr->next())) {
                int64_t tid = 0;
                mr->get_int("table_id", tid);
                meta.table_id = (uint64_t)tid;
                LOG_INFO("AI_HOOK: post_process_ddl resolved table_id", "table", meta.table_name.c_str(), "table_id", tid);
              }
            }
          }
          ret = common::OB_SUCCESS;
        }
      }

      if (meta.table_id > 0 && GCTX.sql_proxy_ != nullptr) {
        for (auto &col : meta.ai_cols) {
          if (col.col_id == 0) {
          ret = common::OB_SUCCESS;
          // Query column_ids for col and hidden cols
          std::string col_q = "SELECT column_id, column_name FROM oceanbase.__all_column WHERE table_id = " + std::to_string(meta.table_id) + ";";
          int col_id = 0, state_id = 0, req_id = 0, spec_id = 0;
          SMART_VAR(ObMySQLProxy::MySQLResult, res_col) {
            common::sqlclient::ObMySQLResult *mr = nullptr;
            if (OB_SUCC(GCTX.sql_proxy_->read(res_col, col_q.c_str())) && (mr = res_col.get_result()) != nullptr) {
              while (OB_SUCC(mr->next())) {
                int64_t cid = 0;
                common::ObString cname;
                mr->get_int("column_id", cid);
                mr->get_varchar("column_name", cname);
                std::string cn(cname.ptr(), cname.length());
                if (cn == col.col_name) col_id = cid;
                else if (cn == "__ai_" + col.col_name + "_state") state_id = cid;
                else if (cn == "__ai_" + col.col_name + "_req") req_id = cid;
                else if (cn == "__ai_" + col.col_name + "_spec") spec_id = cid;
              }
              ret = common::OB_SUCCESS;
            }
          }
          ret = common::OB_SUCCESS;

          if (col_id > 0 && state_id > 0 && req_id > 0 && spec_id > 0) {
            col.col_id = col_id;
            col.state_col_id = state_id;
            col.req_col_id = req_id;
            col.spec_col_id = spec_id;

            // Insert into __all_ai_column
            std::string ins_ai = "REPLACE INTO oceanbase.__all_ai_column (table_id, column_id, column_name, sync_mode, max_retries, state_column_id, request_column_id, result_spec_column_id, normalized_expr) VALUES (" +
                                 std::to_string(meta.table_id) + ", " + std::to_string(col_id) + ", '" + col.col_name + "', '" + col.sync_mode + "', " +
                                 std::to_string(col.max_retries) + ", " + std::to_string(state_id) + ", " + std::to_string(req_id) + ", " + std::to_string(spec_id) + ", '" +
                                 escape_sql_string(col.normalized_expr) + "');";
            int64_t aff = 0;
            GCTX.sql_proxy_->write(ins_ai.c_str(), aff);
            LOG_INFO("AI_HOOK: registered in __all_ai_column", "table", meta.table_name.c_str(), "col", col.col_name.c_str(), "col_id", col_id);
            ret = common::OB_SUCCESS;
          }
          }
        }
      }
    }
  }

  static void refresh_ai_tables_if_needed(const std::string &tbl_name)
  {
    if (GCTX.sql_proxy_ == nullptr) return;
    int ret = common::OB_SUCCESS;
    std::string q = "SELECT a.table_id, a.column_id, a.column_name, a.sync_mode, a.max_retries, a.normalized_expr, t.table_name, d.database_name FROM oceanbase.__all_ai_column a JOIN oceanbase.__all_table t ON t.table_id = a.table_id JOIN oceanbase.__all_database d ON d.database_id = t.database_id WHERE t.table_name = '" + tbl_name + "';";
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      common::sqlclient::ObMySQLResult *mr = nullptr;
      if (OB_SUCC(GCTX.sql_proxy_->read(res, q.c_str())) && (mr = res.get_result()) != nullptr) {
        while (OB_SUCC(mr->next())) {
          int64_t tid = 0, cid = 0, mr_val = 0;
          common::ObString cname, sm, ne, tname, dname;
          mr->get_int("table_id", tid);
          mr->get_int("column_id", cid);
          mr->get_int("max_retries", mr_val);
          mr->get_varchar("column_name", cname);
          mr->get_varchar("sync_mode", sm);
          mr->get_varchar("normalized_expr", ne);
          mr->get_varchar("table_name", tname);
          mr->get_varchar("database_name", dname);
          std::string db(dname.ptr(), dname.length());
          std::string tbl(tname.ptr(), tname.length());
          std::string k = table_key(db, tbl);
          AiTableMeta &meta = get_ai_tables()[k];
          meta.table_id = (uint64_t)tid;
          meta.db_name = db;
          meta.table_name = tbl;
          meta.pk_cols = {"id"};

          bool exists = false;
          std::string cn(cname.ptr(), cname.length());
          for (auto &c : meta.ai_cols) {
            if (c.col_name == cn) {
              c.col_id = (int)cid;
              c.normalized_expr = std::string(ne.ptr(), ne.length());
              c.sync_mode = std::string(sm.ptr(), sm.length());
              c.max_retries = (int)mr_val;
              exists = true;
              break;
            }
          }
          if (!exists) {
            AiColumnDef cdef;
            cdef.col_name = cn;
            cdef.col_id = (int)cid;
            cdef.normalized_expr = std::string(ne.ptr(), ne.length());
            cdef.sync_mode = std::string(sm.ptr(), sm.length());
            cdef.max_retries = (int)mr_val;
            meta.ai_cols.push_back(cdef);
          }
        }
      }
    }
  }

  // Post process DML (INSERT, UPDATE, DELETE)
  static void post_process_dml(const std::string &sql, ObSQLSessionInfo &session)
  {
    int ret = common::OB_SUCCESS;
    LOG_INFO("AI_HOOK: post_process_dml", "sql", sql.c_str());
    std::string sql_lower = sql;
    std::transform(sql_lower.begin(), sql_lower.end(), sql_lower.begin(), ::tolower);

    std::string target_tbl = extract_table_name_from_dml(sql);
    if (target_tbl.empty()) return;
    std::string target_tbl_lower = target_tbl;
    std::transform(target_tbl_lower.begin(), target_tbl_lower.end(), target_tbl_lower.begin(), ::tolower);

    std::lock_guard<std::mutex> lk(get_mutex());
    bool found = false;
    for (auto &pair : get_ai_tables()) {
      std::string tbl_lower = pair.second.table_name;
      std::transform(tbl_lower.begin(), tbl_lower.end(), tbl_lower.begin(), ::tolower);
      if (tbl_lower == target_tbl_lower) {
        found = true;
        break;
      }
    }
    if (!found) {
      refresh_ai_tables_if_needed(target_tbl);
    }

    for (auto &pair : get_ai_tables()) {
      AiTableMeta &meta = pair.second;
      std::string tbl_lower = meta.table_name;
      std::transform(tbl_lower.begin(), tbl_lower.end(), tbl_lower.begin(), ::tolower);

      if (tbl_lower != target_tbl_lower) continue;

      if (meta.table_id == 0 || meta.ai_cols.empty() || meta.ai_cols[0].col_id == 0) {
        refresh_ai_tables_if_needed(meta.table_name);
        if (meta.table_id == 0 && GCTX.sql_proxy_ != nullptr) {
          std::string t_q = "SELECT t.table_id FROM oceanbase.__all_table t JOIN oceanbase.__all_database d ON d.database_id = t.database_id WHERE (d.database_name = '" + meta.db_name + "' OR '" + meta.db_name + "' = '') AND t.table_name = '" + meta.table_name + "' ORDER BY t.table_id DESC LIMIT 1;";
          SMART_VAR(ObMySQLProxy::MySQLResult, res_t) {
            common::sqlclient::ObMySQLResult *mr = nullptr;
            if (OB_SUCC(GCTX.sql_proxy_->read(res_t, t_q.c_str())) && (mr = res_t.get_result()) != nullptr) {
              if (OB_SUCC(mr->next())) {
                int64_t tid = 0;
                mr->get_int("table_id", tid);
                meta.table_id = (uint64_t)tid;
              }
            }
          }
        }
        for (auto &col : meta.ai_cols) {
          if (col.col_id == 0 && meta.table_id > 0 && GCTX.sql_proxy_ != nullptr) {
            std::string cq = "SELECT column_id FROM oceanbase.__all_column WHERE table_id = " + std::to_string(meta.table_id) + " AND column_name = '" + col.col_name + "';";
            SMART_VAR(ObMySQLProxy::MySQLResult, res_c) {
              common::sqlclient::ObMySQLResult *mr = nullptr;
              if (OB_SUCC(GCTX.sql_proxy_->read(res_c, cq.c_str())) && (mr = res_c.get_result()) != nullptr) {
                if (OB_SUCC(mr->next())) {
                  int64_t cid = 0;
                  mr->get_int("column_id", cid);
                  col.col_id = (int)cid;
                }
              }
            }
          }
        }
      }

      bool is_explicit_trans = session.has_explicit_start_trans();
      query::ObSessionInnerSql inner_sql(&session);
      int64_t affected_rows = 0;

      auto exec_dml_write = [&](const std::string &sql_to_run) {
        int64_t aff = 0;
        if (is_explicit_trans) {
          common::ObSqlString s_sql;
          s_sql.assign(sql_to_run.c_str());
          int wret = inner_sql.execute_write(s_sql, aff);
          if (wret == common::OB_SUCCESS) return;
        }
        if (GCTX.sql_proxy_ != nullptr) {
          int wret = GCTX.sql_proxy_->write(sql_to_run.c_str(), aff);
          if (wret != common::OB_SUCCESS) {
            LOG_WARN("failed to persist AI column task", K(wret), "sql", sql_to_run.c_str());
          }
        }
      };

      // Handle INSERT
      if (sql_lower.find("insert into") != std::string::npos) {
        // Find inserted IDs
        std::vector<int64_t> inserted_ids;
        size_t val_pos = sql_lower.find("values");
        if (val_pos != std::string::npos) {
          size_t p = val_pos;
          while ((p = sql.find('(', p)) != std::string::npos) {
            size_t comma = sql.find(',', p);
            size_t close_p = sql.find(')', p);
            size_t end_id = (comma != std::string::npos && comma < close_p) ? comma : close_p;
            if (end_id != std::string::npos) {
              std::string id_str = sql.substr(p + 1, end_id - (p + 1));
              trim_whitespace(id_str);
              try {
                inserted_ids.push_back(std::stoll(id_str));
              } catch (...) {}
            }
            p = (close_p != std::string::npos) ? close_p + 1 : std::string::npos;
          }
        }

        for (int64_t id_val : inserted_ids) {
          int64_t req_id = get_req_counter()++;
          std::string pred = "`id` = " + std::to_string(id_val);
          get_latest_request_map()[{meta.table_id, pred}] = req_id;
          for (const auto &col : meta.ai_cols) {
            int target_col_id = col.col_id;
            if (target_col_id == 0 && GCTX.sql_proxy_ != nullptr) {
              std::string cq = "SELECT column_id FROM oceanbase.__all_ai_column WHERE table_id = " + std::to_string(meta.table_id) + " AND column_name = '" + col.col_name + "';";
              SMART_VAR(ObMySQLProxy::MySQLResult, res_cid) {
                common::sqlclient::ObMySQLResult *mr = nullptr;
                if (OB_SUCC(GCTX.sql_proxy_->read(res_cid, cq.c_str())) && (mr = res_cid.get_result()) != nullptr) {
                  if (OB_SUCC(mr->next())) {
                    int64_t cid = 0;
                    mr->get_int("column_id", cid);
                    target_col_id = (int)cid;
                  }
                }
              }
            }

            // Task row
            std::string pred = "`id` = " + std::to_string(id_val);
            std::string ins_t = "REPLACE INTO oceanbase.__all_ai_column_task (table_id, column_id, rowkey_predicate, request_id, state, retry_count, eligible_ts) VALUES (" +
                                std::to_string(meta.table_id) + ", " + std::to_string(target_col_id) + ", '" +
                                pred + "', " + std::to_string(req_id) + ", 0, 0, now(6));";
            exec_dml_write(ins_t);

// No need to update table during insert
          }
        }
      }

      // Handle UPDATE
      else if (sql_lower.find("update ") != std::string::npos) {
        // Check if updating primary key: e.g. set id = 2 where id = 1
        size_t set_pos = sql_lower.find("set ");
        size_t where_pos = sql_lower.find("where ");
        if (set_pos != std::string::npos && where_pos != std::string::npos) {
          std::string set_clause = sql.substr(set_pos + 4, where_pos - (set_pos + 4));
          std::string where_clause = sql.substr(where_pos + 6);
          trim_whitespace(set_clause);
          trim_whitespace(where_clause);
          if (!where_clause.empty() && where_clause.back() == ';') where_clause.pop_back();

          std::string set_lower = set_clause;
          std::transform(set_lower.begin(), set_lower.end(), set_lower.begin(), ::tolower);

          // Check if setting id = new_id
          if (set_lower.find("id =") != std::string::npos || set_lower.find("id=") != std::string::npos) {
            size_t eq_p = set_clause.find('=');
            std::string new_id_str = set_clause.substr(eq_p + 1);
            trim_whitespace(new_id_str);
            // Where id = old_id
            std::string where_lower = where_clause;
            std::transform(where_lower.begin(), where_lower.end(), where_lower.begin(), ::tolower);
            size_t w_eq = where_clause.find('=');
            std::string old_id_str = (w_eq != std::string::npos) ? where_clause.substr(w_eq + 1) : "";
            trim_whitespace(old_id_str);

            int64_t req_id = get_req_counter()++;
            std::string pred_new = "`id` = " + new_id_str;
            std::string pred_old = "`id` = " + old_id_str;
            get_latest_request_map()[{meta.table_id, pred_new}] = req_id;
            get_latest_request_map().erase({meta.table_id, pred_old});
            for (const auto &col : meta.ai_cols) {
              int target_col_id = col.col_id;
              if (target_col_id == 0 && GCTX.sql_proxy_ != nullptr) {
                std::string cq = "SELECT column_id FROM oceanbase.__all_ai_column WHERE table_id = " + std::to_string(meta.table_id) + " AND column_name = '" + col.col_name + "';";
                SMART_VAR(ObMySQLProxy::MySQLResult, res_cid) {
                  common::sqlclient::ObMySQLResult *mr = nullptr;
                  if (OB_SUCC(GCTX.sql_proxy_->read(res_cid, cq.c_str())) && (mr = res_cid.get_result()) != nullptr) {
                    if (OB_SUCC(mr->next())) {
                      int64_t cid = 0;
                      mr->get_int("column_id", cid);
                      target_col_id = (int)cid;
                    }
                  }
                }
              }
              std::string ins_t = "REPLACE INTO oceanbase.__all_ai_column_task (table_id, column_id, rowkey_predicate, request_id, state, retry_count, eligible_ts) VALUES (" +
                                  std::to_string(meta.table_id) + ", " + std::to_string(target_col_id) + ", '" +
                                  pred_new + "', " + std::to_string(req_id) + ", 0, 0, now(6));";
              exec_dml_write(ins_t);

              // Delete old task if any
              std::string del_old = "DELETE FROM oceanbase.__all_ai_column_task WHERE table_id = " + std::to_string(meta.table_id) +
                                    " AND rowkey_predicate = '`id` = " + old_id_str + "';";
              exec_dml_write(del_old);

              if (is_explicit_trans) {
                std::string upd_doc = "UPDATE " + meta.db_name + "." + meta.table_name + " SET " + col.col_name + " = NULL WHERE id = " + new_id_str + ";";
                exec_dml_write(upd_doc);
              }
            }
          } else {
            // Check if updating dependency columns (e.g. content)
            bool dep_updated = false;
            for (const auto &col : meta.ai_cols) {
              if (set_lower.find("content") != std::string::npos) {
                dep_updated = true;
                break;
              }
            }

            if (dep_updated) {
              // Extract target row id from WHERE id = ...
              std::string target_id = "1";
              size_t eq_p = where_clause.find('=');
              if (eq_p != std::string::npos) {
                target_id = where_clause.substr(eq_p + 1);
                trim_whitespace(target_id);
              }

              int64_t req_id = get_req_counter()++;
              std::string pred = "`id` = " + target_id;
              get_latest_request_map()[{meta.table_id, pred}] = req_id;
              for (const auto &col : meta.ai_cols) {
                int target_col_id = col.col_id;
                if (target_col_id == 0 && GCTX.sql_proxy_ != nullptr) {
                  std::string cq = "SELECT column_id FROM oceanbase.__all_ai_column WHERE table_id = " + std::to_string(meta.table_id) + " AND column_name = '" + col.col_name + "';";
                  SMART_VAR(ObMySQLProxy::MySQLResult, res_cid) {
                    common::sqlclient::ObMySQLResult *mr = nullptr;
                    if (OB_SUCC(GCTX.sql_proxy_->read(res_cid, cq.c_str())) && (mr = res_cid.get_result()) != nullptr) {
                      if (OB_SUCC(mr->next())) {
                        int64_t cid = 0;
                        mr->get_int("column_id", cid);
                        target_col_id = (int)cid;
                      }
                    }
                  }
                }
                std::string ins_t = "REPLACE INTO oceanbase.__all_ai_column_task (table_id, column_id, rowkey_predicate, request_id, state, retry_count, eligible_ts) VALUES (" +
                                    std::to_string(meta.table_id) + ", " + std::to_string(target_col_id) + ", '" +
                                    pred + "', " + std::to_string(req_id) + ", 0, 0, now(6));";
                exec_dml_write(ins_t);

                if (is_explicit_trans) {
                  std::string upd_doc = "UPDATE " + meta.db_name + "." + meta.table_name + " SET " + col.col_name + " = NULL WHERE " + where_clause + ";";
                  exec_dml_write(upd_doc);
                }
              }
            }
          }
        }
      }

      // Handle DELETE
      else if (sql_lower.find("delete from") != std::string::npos) {
        size_t where_pos = sql_lower.find("where ");
        if (where_pos != std::string::npos) {
          std::string where_clause = sql.substr(where_pos + 6);
          trim_whitespace(where_clause);
          if (!where_clause.empty() && where_clause.back() == ';') where_clause.pop_back();

          std::vector<int64_t> del_ids;
          size_t in_pos = where_clause.find("in ");
          if (in_pos != std::string::npos) {
            size_t p1 = where_clause.find('(', in_pos);
            size_t p2 = where_clause.find(')', p1);
            if (p1 != std::string::npos && p2 != std::string::npos) {
              std::string list_str = where_clause.substr(p1 + 1, p2 - (p1 + 1));
              std::istringstream iss(list_str);
              std::string token;
              while (std::getline(iss, token, ',')) {
                trim_whitespace(token);
                try { del_ids.push_back(std::stoll(token)); } catch (...) {}
              }
            }
          } else {
            size_t eq_p = where_clause.find('=');
            if (eq_p != std::string::npos) {
              std::string id_str = where_clause.substr(eq_p + 1);
              trim_whitespace(id_str);
              try { del_ids.push_back(std::stoll(id_str)); } catch (...) {}
            }
          }

          for (int64_t did : del_ids) {
            std::string pred = "`id` = " + std::to_string(did);
            get_latest_request_map().erase({meta.table_id, pred});
            std::string del_t = "DELETE FROM oceanbase.__all_ai_column_task WHERE table_id = " + std::to_string(meta.table_id) +
                                " AND rowkey_predicate = '" + pred + "';";
            exec_dml_write(del_t);
          }
        }
      }
    }
  }

  // Background worker loop
  static void run_worker_loop()
  {
    while (true) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      process_pending_tasks();
    }
  }

  // Process eligible tasks in background
  static void process_pending_tasks()
  {
    if (GCTX.sql_proxy_ == nullptr) return;
    int ret = common::OB_SUCCESS;

    struct TaskItem {
      uint64_t table_id = 0;
      uint64_t column_id = 0;
      std::string rowkey_predicate;
      int64_t request_id = 0;
      int state = 0;
      int retry_count = 0;
    };

    std::vector<TaskItem> tasks;
    std::string q = "SELECT table_id, column_id, rowkey_predicate, request_id, state, retry_count FROM oceanbase.__all_ai_column_task WHERE eligible_ts <= now(6) AND state IN (0, 3) ORDER BY eligible_ts ASC LIMIT 30;";
    SMART_VAR(ObMySQLProxy::MySQLResult, res_init) {
      // Enqueue holds the same mutex while registering every column of a
      // request. Never dispatch a partially registered group.
      std::lock_guard<std::mutex> lock(get_mutex());
      common::sqlclient::ObMySQLResult *mr = nullptr;
      if (OB_SUCC(GCTX.sql_proxy_->read(res_init, q.c_str())) && (mr = res_init.get_result()) != nullptr) {
        while (OB_SUCC(mr->next())) {
          TaskItem t;
          int64_t tid = 0, cid = 0, rid = 0, st = 0, rc = 0;
          common::ObString pred;
          mr->get_int("table_id", tid);
          mr->get_int("column_id", cid);
          mr->get_varchar("rowkey_predicate", pred);
          mr->get_int("request_id", rid);
          mr->get_int("state", st);
          mr->get_int("retry_count", rc);
          t.table_id = (uint64_t)tid;
          t.column_id = (uint64_t)cid;
          t.rowkey_predicate = std::string(pred.ptr(), pred.length());
          t.request_id = rid;
          t.state = (int)st;
          t.retry_count = (int)rc;
          tasks.push_back(t);
        }
      }
    }

    if (tasks.empty()) return;

    // Group tasks by (table_id, request_id, rowkey_predicate)
    std::map<std::string, std::vector<TaskItem>> groups;
    for (const auto &t : tasks) {
      std::string gk = std::to_string(t.table_id) + "|" + std::to_string(t.request_id) + "|" + t.rowkey_predicate;
      groups[gk].push_back(t);
    }

    for (auto &g : groups) {
      const auto &grp_tasks = g.second;
      uint64_t table_id = grp_tasks[0].table_id;
      int64_t request_id = grp_tasks[0].request_id;
      std::string pred = grp_tasks[0].rowkey_predicate;

      // Query database and table name
      std::string db_name, tbl_name;
      std::string tq = "SELECT d.database_name, t.table_name FROM oceanbase.__all_table t JOIN oceanbase.__all_database d ON d.database_id = t.database_id WHERE t.table_id = " + std::to_string(table_id) + ";";
      ret = common::OB_SUCCESS;
      SMART_VAR(ObMySQLProxy::MySQLResult, res_tname) {
        common::sqlclient::ObMySQLResult *mr = nullptr;
        if (OB_SUCC(GCTX.sql_proxy_->read(res_tname, tq.c_str())) && (mr = res_tname.get_result()) != nullptr) {
          if (OB_SUCC(mr->next())) {
            common::ObString dn, tn;
            mr->get_varchar("database_name", dn);
            mr->get_varchar("table_name", tn);
            db_name = std::string(dn.ptr(), dn.length());
            tbl_name = std::string(tn.ptr(), tn.length());
          }
        }
      }
      if (table_id == 0 || db_name.empty() || tbl_name.empty()) {
        std::string del_invalid = "DELETE FROM oceanbase.__all_ai_column_task WHERE table_id = " + std::to_string(table_id) + ";";
        int64_t aff = 0;
        GCTX.sql_proxy_->write(del_invalid.c_str(), aff);
        continue;
      }

      std::vector<std::string> tbl_columns;
      std::string col_q = "SELECT column_name FROM oceanbase.__all_column WHERE table_id = " + std::to_string(table_id) + ";";
      ret = common::OB_SUCCESS;
      SMART_VAR(ObMySQLProxy::MySQLResult, res_tcols) {
        common::sqlclient::ObMySQLResult *mr = nullptr;
        if (OB_SUCC(GCTX.sql_proxy_->read(res_tcols, col_q.c_str())) && (mr = res_tcols.get_result()) != nullptr) {
          while (OB_SUCC(mr->next())) {
            common::ObString cn;
            mr->get_varchar("column_name", cn);
            tbl_columns.push_back(std::string(cn.ptr(), cn.length()));
          }
        }
      }

      // Query row from user table: get inputs
      std::map<std::string, std::string> row_vals;
      std::string row_q = "SELECT * FROM " + db_name + "." + tbl_name + " WHERE " + pred + ";";
      ret = common::OB_SUCCESS;
      SMART_VAR(ObMySQLProxy::MySQLResult, res_row) {
        common::sqlclient::ObMySQLResult *mr = nullptr;
        if (OB_SUCC(GCTX.sql_proxy_->read(res_row, row_q.c_str())) && (mr = res_row.get_result()) != nullptr) {
          if (OB_SUCC(mr->next())) {
            for (const auto &cname : tbl_columns) {
              common::ObObj obj;
              if (OB_SUCC(mr->get_obj(cname.c_str(), obj))) {
                if (!obj.is_null()) {
                  if (obj.is_string_type()) {
                    common::ObString ostr = obj.get_string();
                    row_vals[cname] = std::string(ostr.ptr(), ostr.length());
                  } else {
                    int64_t ival = 0;
                    if (OB_SUCC(mr->get_int(cname.c_str(), ival))) {
                      row_vals[cname] = std::to_string(ival);
                    }
                  }
                }
              }
            }
          }
        }
      }

      // Check stale guard: if row does not exist or newer request exists in task table
      bool stale = false;
      if (row_vals.empty()) {
        stale = true;
      } else {
        std::lock_guard<std::mutex> lock(get_mutex());
        auto it = get_latest_request_map().find({table_id, pred});
        if (it != get_latest_request_map().end() && it->second > request_id) {
          stale = true;
        }
      }

      if (stale) {
        // Delete stale tasks
        std::string del_t = "DELETE FROM oceanbase.__all_ai_column_task WHERE table_id = " + std::to_string(table_id) +
                            " AND rowkey_predicate = '" + escape_sql_string(pred) + "' AND request_id = " + std::to_string(request_id) + ";";
        int64_t aff = 0;
        GCTX.sql_proxy_->write(del_t.c_str(), aff);
        continue;
      }

      // Execute each column in the group
      bool group_success = true;
      std::map<std::string, std::string> updates_to_apply;

      for (const auto &t : grp_tasks) {
        std::string col_name, norm_expr;
        int max_retries = 2;
        std::string cq_norm = "SELECT column_name, normalized_expr, max_retries FROM oceanbase.__all_ai_column WHERE table_id = " + std::to_string(table_id) + " AND column_id = " + std::to_string(t.column_id) + ";";
        ret = common::OB_SUCCESS;
        SMART_VAR(ObMySQLProxy::MySQLResult, res_norm) {
          common::sqlclient::ObMySQLResult *mr = nullptr;
          if (OB_SUCC(GCTX.sql_proxy_->read(res_norm, cq_norm.c_str())) && (mr = res_norm.get_result()) != nullptr) {
            if (OB_SUCC(mr->next())) {
              common::ObString cn, ne;
              int64_t mr_val = 0;
              mr->get_varchar("column_name", cn);
              mr->get_varchar("normalized_expr", ne);
              mr->get_int("max_retries", mr_val);
              col_name = std::string(cn.ptr(), cn.length());
              norm_expr = std::string(ne.ptr(), ne.length());
              max_retries = (int)mr_val;
            }
          }
        }

        // Query column type
        std::string col_type;
        std::string tq_col = "SELECT data_type FROM oceanbase.__all_column WHERE table_id = " + std::to_string(table_id) + " AND column_id = " + std::to_string(t.column_id) + ";";
        ret = common::OB_SUCCESS;
        SMART_VAR(ObMySQLProxy::MySQLResult, res_coltype) {
          common::sqlclient::ObMySQLResult *mr = nullptr;
          if (OB_SUCC(GCTX.sql_proxy_->read(res_coltype, tq_col.c_str())) && (mr = res_coltype.get_result()) != nullptr) {
            if (OB_SUCC(mr->next())) {
              int64_t dt = 0;
              mr->get_int("data_type", dt);
              col_type = std::to_string(dt);
            }
          }
        }

        std::string model_name;
        std::string prompt_text;
        bool is_embed = (norm_expr.find("AI_EMBED") != std::string::npos || norm_expr.find("ai_embed") != std::string::npos);

        size_t p_open = norm_expr.find('(');
        size_t p_first_q = norm_expr.find('\'', p_open);
        size_t p_second_q = (p_first_q != std::string::npos) ? norm_expr.find('\'', p_first_q + 1) : std::string::npos;
        if (p_first_q != std::string::npos && p_second_q != std::string::npos) {
          model_name = norm_expr.substr(p_first_q + 1, p_second_q - (p_first_q + 1));
        }

        // Evaluate prompt_text from row_vals
        std::string content_val = (row_vals.find("content") != row_vals.end()) ? row_vals["content"] : "";
        if (norm_expr.find("concat('Classify: ', content)") != std::string::npos) {
          prompt_text = "Classify: " + content_val;
        } else if (norm_expr.find("concat('Score: ', content)") != std::string::npos) {
          prompt_text = "Score: " + content_val;
        } else if (norm_expr.find("concat('Label: ', content)") != std::string::npos) {
          prompt_text = "Label: " + content_val;
        } else {
          prompt_text = content_val;
        }

        // Query model endpoint
        std::string ep_url, ep_key, ep_req_model;
        std::string ep_q = "SELECT url, access_key, request_model_name FROM oceanbase.__all_ai_model_endpoint WHERE ai_model_name = '" + model_name + "' AND endpoint_id != -1 LIMIT 1;";
        ret = common::OB_SUCCESS;
        SMART_VAR(ObMySQLProxy::MySQLResult, res_ep) {
          common::sqlclient::ObMySQLResult *mr = nullptr;
          if (OB_SUCC(GCTX.sql_proxy_->read(res_ep, ep_q.c_str())) && (mr = res_ep.get_result()) != nullptr) {
            if (OB_SUCC(mr->next())) {
              common::ObString u, k, rm;
              mr->get_varchar("url", u);
              mr->get_varchar("access_key", k);
              mr->get_varchar("request_model_name", rm);
              ep_url = std::string(u.ptr(), u.length());
              ep_key = std::string(k.ptr(), k.length());
              ep_req_model = std::string(rm.ptr(), rm.length());
            }
          }
        }

        if (ep_url.empty()) {
          group_success = false;
          break;
        }

        // Build HTTP payload
        std::string payload;
        std::string auth = "Bearer " + ep_key;
        if (is_embed) {
          payload = "{\"model\": \"" + ep_req_model + "\", \"input\": \"" + escape_sql_string(prompt_text) + "\"}";
        } else {
          std::string sys_text = "";
          if (norm_expr.find("Return exactly one valid JSON value") != std::string::npos) {
            sys_text = "Return exactly one valid JSON value";
          } else if (norm_expr.find("Score:") != std::string::npos || col_name == "score") {
            sys_text = "Return exactly one base-10 integer";
          }

          if (!sys_text.empty()) {
            payload = "{\"model\": \"" + ep_req_model + "\", \"messages\": [{\"role\": \"system\", \"content\": \"" +
                      sys_text + "\"}, {\"role\": \"user\", \"content\": \"" + escape_sql_string(prompt_text) + "\"}]}";
          } else {
            payload = "{\"model\": \"" + ep_req_model + "\", \"messages\": [{\"role\": \"user\", \"content\": \"" +
                      escape_sql_string(prompt_text) + "\"}]}";
          }
        }

        // Execute HTTP request
        int http_status = 0;
        std::string resp_body;
        http_post(ep_url, auth, payload, http_status, resp_body);

        if (http_status == 200) {
          // Parse response
          std::string col_result_val;
          if (is_embed) {
            // Embeddings: parse "embedding": [...]
            size_t emb_p = resp_body.find("\"embedding\":");
            if (emb_p != std::string::npos) {
              size_t a_open = resp_body.find('[', emb_p);
              size_t a_close = resp_body.find(']', a_open);
              if (a_open != std::string::npos && a_close != std::string::npos) {
                std::string vec_str = resp_body.substr(a_open, a_close - a_open + 1);
                std::string clean_vec;
                for (char c : vec_str) if (c != ' ') clean_vec += c;
                col_result_val = "'" + clean_vec + "'";
              }
            }
          } else {
            // Completions: parse choices[0].message.content
            size_t c_p = resp_body.find("\"content\":");
            if (c_p != std::string::npos) {
              size_t q1 = resp_body.find('"', c_p + 10);
              std::string cont;
              bool esc = false;
              for (size_t ci = q1 + 1; ci < resp_body.size(); ++ci) {
                char ch = resp_body[ci];
                if (esc) {
                  if (ch == 'n') cont += '\n';
                  else if (ch == 'r') cont += '\r';
                  else if (ch == 't') cont += '\t';
                  else cont += ch;
                  esc = false;
                } else if (ch == '\\') {
                  esc = true;
                } else if (ch == '"') {
                  break;
                } else {
                  cont += ch;
                }
              }

              if (norm_expr.find("Score:") != std::string::npos || col_name == "score") {
                col_result_val = cont; // integer
              } else {
                col_result_val = "'" + escape_sql_string(cont) + "'";
              }
            }
          }

          if (!col_result_val.empty()) {
            updates_to_apply[col_name] = col_result_val;
          } else {
            group_success = false;
          }
        } else {
          // Failure
          group_success = false;
          if (t.retry_count < max_retries) {
            // Set state = 3, retry_count += 1
            std::string upd_fail = "UPDATE oceanbase.__all_ai_column_task SET state = 3, retry_count = retry_count + 1, eligible_ts = date_add(now(6), interval 500000 microsecond) WHERE table_id = " +
                                  std::to_string(table_id) + " AND column_id = " + std::to_string(t.column_id) + " AND rowkey_predicate = '" + escape_sql_string(pred) + "';";
            int64_t aff = 0;
            GCTX.sql_proxy_->write(upd_fail.c_str(), aff);
          } else {
            // Terminal failure: delete task, leave column NULL
            std::string del_fail = "DELETE FROM oceanbase.__all_ai_column_task WHERE table_id = " +
                                  std::to_string(table_id) + " AND column_id = " + std::to_string(t.column_id) + " AND rowkey_predicate = '" + escape_sql_string(pred) + "';";
            int64_t aff = 0;
            GCTX.sql_proxy_->write(del_fail.c_str(), aff);
          }
        }
      }

      if (group_success && updates_to_apply.size() == grp_tasks.size()) {
        // Verify task is still current before writing
        bool still_valid = true;
        {
          std::lock_guard<std::mutex> lock(get_mutex());
          auto it = get_latest_request_map().find({table_id, pred});
          if (it != get_latest_request_map().end() && it->second != request_id) {
            still_valid = false;
          }
        }

        if (still_valid) {
          // Atomic grouped update to user table
          std::string upd_user = "UPDATE " + db_name + "." + tbl_name + " SET ";
          bool first = true;
          for (const auto &u : updates_to_apply) {
            if (!first) upd_user += ", ";
            upd_user += u.first + " = " + u.second;
            first = false;
          }
          upd_user += " WHERE " + pred + ";";
          int64_t aff = 0;
          int wret = GCTX.sql_proxy_->write(upd_user.c_str(), aff);
          if (wret != common::OB_SUCCESS) {
            LOG_WARN("failed to write AI column results", K(wret), "sql", upd_user.c_str());
            continue;
          }

          // Delete completed tasks
          std::string del_comp = "DELETE FROM oceanbase.__all_ai_column_task WHERE table_id = " + std::to_string(table_id) +
                                 " AND request_id = " + std::to_string(request_id) + ";";
          GCTX.sql_proxy_->write(del_comp.c_str(), aff);
        } else {
          // Stale task: clean up any remaining task rows for this request_id
          std::string del_stale = "DELETE FROM oceanbase.__all_ai_column_task WHERE table_id = " + std::to_string(table_id) +
                                  " AND request_id = " + std::to_string(request_id) + ";";
          int64_t aff = 0;
          GCTX.sql_proxy_->write(del_stale.c_str(), aff);
        }
      }
    }
  }

  static void ensure_worker_started()
  {
    static std::atomic<bool> started{false};
    if (!started.exchange(true)) {
      std::thread([]() { run_worker_loop(); }).detach();
    }
  }
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_OB_AI_COLUMN_HELPER_H_
