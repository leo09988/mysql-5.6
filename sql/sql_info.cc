/* Copyright (c) 2010, 2019, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "sql/sql_info.h"

#include <algorithm>
#include <atomic>
#include <set>
#include <string>
#include <vector>
#include "include/my_macros.h"
#include "include/my_md5.h"
#include "sql/derror.h"
#include "sql/mysqld.h"
#include "sql/protocol_classic.h"
#include "sql/sql_class.h"
#include "sql/sql_error.h"
#include "sql/sql_lex.h"
#include "sql/sql_parse.h"
#include "sql/sql_thd_internal_api.h"
#include "unordered_map"

/***********************************************************************
 Begin - Functions to support capping the number of duplicate executions
************************************************************************/

/* Global map to track the number of active identical sql statements */
static std::unordered_map<std::string, uint> global_active_sql;

/*
  free_global_active_sql
    Frees global_active_sql
*/
void free_global_active_sql(void) {
  mysql_mutex_lock(&LOCK_global_active_sql);
  global_active_sql.clear();
  mysql_mutex_unlock(&LOCK_global_active_sql);
}

/*
  skip_quote
  Skips over quoted strings
  It handles both single and double quotes by calling
  it twice by passing the quote character as argument
  first time single quote and second as double quote.
  Skipping happens by changing the offset of the next
  comment search (position of the second quote+1).
  It handles both cases when the string includes the
  beginning of a comment or not
*/
static bool skip_quote(std::string &query_str, size_t *offset,
                       size_t c_start_pos, const char *quote) {
  size_t quote_pos = query_str.find(quote, *offset);

  if (quote_pos != std::string::npos && quote_pos < c_start_pos) {
    quote_pos = query_str.find(quote, quote_pos + 1);
    *offset = quote_pos + 1;
    return true;
  }
  return false;
}

/*
  strip_query_comment
    Return the query text without comments

  Examples (where begin comment is /+ and end comment is +/)
   IN: /+C1+/ select '''Q2''', '''/+''', """+/""" /+C2+/ from dual /+C3+/
   OUT:  select '''Q2''', '''/+''', """+/"""  from dual
   IN: /+C1+/ select 'Q2', '/+', "+/" /+C2+/ from dual /+C3+/
   OUT:  select 'Q2', '/+', "+/"  from dual
*/
static void strip_query_comments(std::string &query_str) {
  const uint comment_size = 2;
  size_t offset = 0;
  while (true) {
    size_t c_start_pos = query_str.find("/*", offset);

    if (c_start_pos == std::string::npos) break;

    // so far we found a start of a comment; next we
    // check if it is enclosed in a string i.e either
    // single or double quoted
    if (skip_quote(query_str, &offset, c_start_pos, "'")) continue;

    if (skip_quote(query_str, &offset, c_start_pos, "\"")) continue;

    size_t c_stop_pos = query_str.find("*/", c_start_pos);
    assert(c_stop_pos != std::string::npos);

    query_str.erase(c_start_pos, c_stop_pos + comment_size - c_start_pos);
  }
}

struct query_flags {
  unsigned int client_long_flag : 1;
  unsigned int client_protocol_41 : 1;
  unsigned int protocol_type : 2;
  unsigned int more_results_exists : 1;
  unsigned int in_trans : 1;
  unsigned int autocommit : 1;
  unsigned int pkt_nr;
  uint character_set_client_num;
  uint character_set_results_num;
  uint collation_connection_num;
  ha_rows limit;
  Time_zone *time_zone;
  sql_mode_t sql_mode;
  ulong max_sort_length;
  ulong group_concat_max_len;
  ulong default_week_format;
  ulong div_precision_increment;
  MY_LOCALE *lc_time_names;
};

/*
  get_query_flags
   Get the query cache flags used to build a key when looking
   for identical queries
*/
static query_flags get_query_flags(THD *thd) {
  query_flags flags;
  Protocol *prot = thd->get_protocol();

  // fill all gaps between fields with 0 to get repeatable key
  memset(&flags, 0, sizeof(query_flags));
  flags.client_long_flag =
      (prot->get_client_capabilities() & CLIENT_LONG_FLAG) ? 1 : 0;
  flags.client_protocol_41 =
      (prot->get_client_capabilities() & CLIENT_PROTOCOL_41) ? 1 : 0;
  /*
    Protocol influences result format, so statement results in the binary
    protocol (COM_EXECUTE) cannot be served to statements asking for results
    in the text protocol (COM_QUERY) and vice-versa.
  */
  flags.protocol_type = (unsigned int)prot->type();
  /* PROTOCOL_LOCAL results are not cached. */
  assert(flags.protocol_type != (unsigned int)Protocol::PROTOCOL_LOCAL);
  flags.more_results_exists =
      (thd->server_status & SERVER_MORE_RESULTS_EXISTS) ? 1 : 0;
  flags.in_trans = thd->in_active_multi_stmt_transaction();
  flags.autocommit = (thd->server_status & SERVER_STATUS_AUTOCOMMIT) ? 1 : 0;

  flags.character_set_client_num = thd->variables.character_set_client->number;
  flags.character_set_results_num =
      (thd->variables.character_set_results
           ? thd->variables.character_set_results->number
           : UINT_MAX);
  flags.collation_connection_num = thd->variables.collation_connection->number;
  flags.limit = thd->variables.select_limit;
  flags.time_zone = thd->variables.time_zone;
  flags.sql_mode = thd->variables.sql_mode;
  flags.max_sort_length = thd->variables.max_sort_length;
  flags.lc_time_names = thd->variables.lc_time_names;
  flags.group_concat_max_len = thd->variables.group_concat_max_len;
  flags.div_precision_increment = thd->variables.div_precincrement;
  flags.default_week_format = thd->variables.default_week_format;

  return flags;
}

/*
  register_active_sql
    Register a new active SQL, called right after SQL parsing

  Returns TRUE if the query is rejected
*/
bool register_active_sql(THD *thd, const char *query_text,
                         size_t query_length) {
  /* Get the maximum number of duplicate executions from query attribute,
     connection attribute, or server variable
  */
  ulong max_dup_exe = thd->get_query_or_connect_attr_value(
      "@@sql_max_dup_exe", sql_maximum_duplicate_executions, ULONG_MAX);
  ulong control = sql_duplicate_executions_control;
  /* Exit now if any of the conditions that prevent the feature is true
     - feature is off
     - command type is not SELECT
     - EXPLAIN
     - in a transaction
  */
  if (max_dup_exe == 0 || control == CONTROL_LEVEL_OFF ||
      thd->lex->sql_command != SQLCOM_SELECT || thd->lex->is_explain() ||
      thd->in_active_multi_stmt_transaction())
    return false;

  size_t flags_size = sizeof(query_flags);
  /* prepare a string with a size large enough to store the sql text,
     database, and some flags (that affect query similarity).
  */
  std::string query_str;
  query_str.reserve(query_length + thd->db().length + flags_size + 1);

  /* load the sql text */
  query_str.append(query_text, query_length);

  /* strip the query comments (everywhere in the text) */
  strip_query_comments(query_str);

  /* load the database name */
  query_str.append((const char *)thd->db().str, thd->db().length);

  /* load the flags */
  query_flags flags = get_query_flags(thd);
  query_str.append((const char *)&flags, flags_size);

  /* compute MD5 from the key value (query, db, flags) */
  std::array<unsigned char, MD5_HASH_SIZE> sql_hash;
  compute_md5_hash(reinterpret_cast<char *>(sql_hash.data()), query_str.c_str(),
                   query_str.length());
  std::string sql_hash_str((const char *)sql_hash.data(), MD5_HASH_SIZE);

  // so far did not exceed the max number of dups
  bool rejected = false;

  mysql_mutex_lock(&LOCK_global_active_sql);
  auto iter = global_active_sql.find(sql_hash_str);
  if (iter == global_active_sql.end()) {
    global_active_sql.emplace(sql_hash_str, 1);  // its first occurrence
  } else {
    if (iter->second + 1 > max_dup_exe)  // one too many
    {
      if (control == CONTROL_LEVEL_ERROR)  // reject it
      {
        rejected = true;
        my_error(ER_DUPLICATE_STATEMENT_EXECUTION, MYF(0));
      } else {  // control is NOTE or WARN => report note / warning
        push_warning(thd,
                     (control == CONTROL_LEVEL_NOTE)
                         ? Sql_condition::SL_NOTE
                         : Sql_condition::SL_WARNING,
                     ER_DUPLICATE_STATEMENT_EXECUTION,
                     ER_THD(thd, ER_DUPLICATE_STATEMENT_EXECUTION));
      }
    }
    if (!rejected && iter != global_active_sql.end())
      iter->second++;  // increment the number of duplicates
  }

  if (!rejected) {
    // remember the sql_hash
    thd->mt_key_set(THD::SQL_HASH, sql_hash.data(), MD5_HASH_SIZE);
    assert(thd->mt_key_is_set(THD::SQL_HASH));
  }

  mysql_mutex_unlock(&LOCK_global_active_sql);
  return rejected;
}

/*
  remove_active_sql
    Remove an active SQL, called at end of the execution
*/
void remove_active_sql(THD *thd) {
  if (!thd->mt_key_is_set(THD::SQL_HASH)) return;

  std::string sql_hash_str(
      (const char *)thd->mt_key_value(THD::SQL_HASH).data(), MD5_HASH_SIZE);
  mysql_mutex_lock(&LOCK_global_active_sql);

  auto iter = global_active_sql.find(sql_hash_str);
  if (iter != global_active_sql.end()) {
    if (iter->second == 1)
      global_active_sql.erase(iter);
    else
      iter->second--;
  }

  mysql_mutex_unlock(&LOCK_global_active_sql);
}

/*********************************************************************
 End - Functions to support capping the number of duplicate executions
**********************************************************************/

/***********************************************************************
              Begin - Functions to support SQL findings
************************************************************************/

/*
  SQL_FINDINGS

  Associates a SQL ID with its findings (aka SQL conditions).
*/
const uint sf_max_message_size = 512;  // max message text size
/* standard limit for field, table, DB name length */
const uint max_db_name_length = NAME_CHAR_LEN;

/*
  SQL statements are identified by their digest and database. Including database
  name allows avoiding collision between similar SQL statements.
*/
typedef std::pair<digest_key, std::string> SQL_FINDING_KEY;

struct hash_pair {
  template <class T1, class T2>
  size_t operator()(const std::pair<T1, T2> &p) const {
    auto hash1 = std::hash<T1>{}(p.first);
    auto hash2 = std::hash<T2>{}(p.second);
    return hash1 ^ hash2;
  }
};

/* Global SQL findings map to track findings for all SQL statements */
static std::unordered_map<SQL_FINDING_KEY, SQL_FINDING_VEC, hash_pair>
    global_sql_findings_map;

/* System variable to control the maximum size of the sql findings map */
ulonglong max_sql_findings_size;
/* Stores the current maximum size
   Used to determine whether we need to flush the findings map, i.e when
   the new maximum size is lower then the current maximum size.
*/
ulonglong current_max_sql_findings_size;
/* Stores the memory size currently in use */
ulonglong sql_findings_size = 0;

/*
  is_sql_stats_collection_above_limit
    Checks whether we reached the counter and size limits for the all the
    SQL metrics we collect (sql findings)
*/
bool is_sql_stats_collection_above_limit() {
  return sql_findings_size >= current_max_sql_findings_size;
}

/*
  free_global_sql_findings
    Frees global_sql_findings
*/
void free_global_sql_findings(bool limits_updated) {
  mysql_mutex_lock(&LOCK_global_sql_findings);

  if (limits_updated) {
    if (sql_findings_size <= max_sql_findings_size) {
      current_max_sql_findings_size = max_sql_findings_size;
      mysql_mutex_unlock(&LOCK_global_sql_findings);
      return;
    }
  }

  for (auto &finding_iter : global_sql_findings_map)
    finding_iter.second.clear();

  global_sql_findings_map.clear();
  sql_findings_size = 0;

  current_max_sql_findings_size = max_sql_findings_size;

  mysql_mutex_unlock(&LOCK_global_sql_findings);
}

/*
  populate_sql_findings
    Populate the findings for the SQL statement that just ended into
    the specificed finding map

  Input:
    thd         in:  - THD
    query_text  in:  - text of the SQL statement
    finding_vec out: - vector that stores the findings of the statement
                       (key is the warning code)
*/
static void populate_sql_findings(THD *thd, const std::string &query_text,
                                  SQL_FINDING_VEC &finding_vec) {
  Diagnostics_area::Sql_condition_iterator it =
      thd->get_stmt_da()->sql_conditions();

  const Sql_condition *err;
  const char *db_name = thd->get_db_name();
  while ((err = it++)) {
    ulonglong now = my_getsystime() / 10000000;
    const uint err_no = err->mysql_errno();

    // Lookup the finding map of this statement for current condition
    std::vector<SQL_FINDING>::iterator iter = std::find_if(
        finding_vec.begin(), finding_vec.end(),
        [&](const SQL_FINDING &finding) {
          return finding.code == err_no &&
                 err->message_octet_length() >= finding.message.size() &&
                 strncmp(finding.message.c_str(), err->message_text(),
                         finding.message.size()) == 0;
        });
    if (iter == finding_vec.cend()) {
      /* If we reached the SQL stats limits then skip adding new findings
         i.e, keep updating the count and date of existing findings
       */
      if (is_sql_stats_collection_above_limit()) continue;

      // First time this finding is reported for this statement
      SQL_FINDING sql_find;
      sql_find.code = err_no;
      sql_find.level = err->severity();
      sql_find.message.append(
          err->message_text(),
          std::min((uint)err->message_octet_length(), sf_max_message_size));
      sql_find.query_text.append(
          query_text, 0,
          std::min(query_text.size(), performance_schema_max_sql_text_length));
      sql_find.count = 1;
      sql_find.last_recorded = now;
      sql_find.db_name.append(
          db_name, std::min((uint)strlen(db_name), max_db_name_length));
      finding_vec.push_back(sql_find);

      sql_findings_size += sizeof(SQL_FINDING);
      sql_findings_size += sql_find.message.size();
      sql_findings_size += sql_find.query_text.size();
      sql_findings_size += sql_find.db_name.size();
    } else {
      // Increment the count and update the time
      iter->count++;
      iter->last_recorded = now;
    }
  }
}

/*
  store_sql_findings
    Store the findings for the SQL statement that just ended into
    the corresponding findings map that is looked up in the global
    map using the SQL ID of the statement. The bulk of the work is
    done in populate_sql_findings()

  Input:
    thd         in:  - THD
    query_text  in:  - text of the SQL statement
*/
void store_sql_findings(THD *thd, const std::string &query_text) {
  if (sql_findings_control == SQL_INFO_CONTROL_ON &&
      thd->mt_key_is_set(THD::SQL_ID) && thd->get_stmt_da()->cond_count() > 0) {
    mysql_mutex_lock(&LOCK_global_sql_findings);

    auto db_name = thd->get_db_name();
    auto sql_key = std::make_pair(thd->mt_key_value(THD::SQL_ID), db_name);
    // Lookup finding map for this statement
    auto sql_find_it = global_sql_findings_map.find(sql_key);
    if (sql_find_it == global_sql_findings_map.end()) {
      /* Check whether we reached the SQL stats limits  */
      if (!is_sql_stats_collection_above_limit()) {
        // First time a finding is reported for this statement
        SQL_FINDING_VEC finding_vec;
        populate_sql_findings(thd, query_text, finding_vec);

        global_sql_findings_map.insert(std::make_pair(sql_key, finding_vec));

        sql_findings_size += sizeof(SQL_FINDING_KEY);
        sql_findings_size += DIGEST_HASH_SIZE;  // for SQL_ID
        sql_findings_size += strlen(db_name);   // for DB name
      }
    } else {
      populate_sql_findings(thd, query_text, sql_find_it->second);
    }

    mysql_mutex_unlock(&LOCK_global_sql_findings);
  }
}

std::vector<sql_findings_row> get_all_sql_findings() {
  std::vector<sql_findings_row> sql_findings;
  mysql_mutex_lock(&LOCK_global_sql_findings);

  for (auto sql_iter = global_sql_findings_map.cbegin();
       sql_iter != global_sql_findings_map.cend(); ++sql_iter) {
    /* Generate the DIGEST string from the digest */
    char sql_id_string[DIGEST_HASH_TO_STRING_LENGTH + 1];
    DIGEST_HASH_TO_STRING(sql_iter->first.first.data(), sql_id_string);
    sql_id_string[DIGEST_HASH_TO_STRING_LENGTH] = '\0';

    for (auto f_iter = sql_iter->second.cbegin();
         f_iter != sql_iter->second.cend(); ++f_iter) {
      sql_findings.emplace_back(
          sql_id_string,                           // SQL_ID
          f_iter->code,                            // CODE
          warning_level_names[f_iter->level].str,  // LEVEL
          f_iter->message.c_str(),                 // MESSAGE
          f_iter->query_text.c_str(),              // QUERY_TEXT
          f_iter->count,                           // COUNT
          f_iter->last_recorded,                   // LAST_RECORDED
          f_iter->db_name);                        // DB_NAME
    }
  }

  mysql_mutex_unlock(&LOCK_global_sql_findings);
  return sql_findings;
}

/***********************************************************************
                End - Functions to support SQL findings
************************************************************************/

/***********************************************************************
              Begin - Functions to support full SQL
************************************************************************/

static std::atomic_bool empty(true);
static std::set<std::string> full_sql_ids_set;
static std::unordered_map<std::string, std::string> global_full_sql_text;

bool update_full_sql_ids(const std::string &sql_id_csv) {
  if (!sql_id_csv.empty()) {
    std::istringstream csv_stream(sql_id_csv);
    std::string sql_id;
    std::set<std::string> sql_ids;
    while (std::getline(csv_stream, sql_id, ',')) {
      if (sql_id.size() != 2 * DIGEST_HASH_SIZE) {
        return true; /* error */
      }
      sql_ids.insert(sql_id);
    }
    mysql_mutex_lock(&LOCK_full_sql_text);
    full_sql_ids_set.clear();
    full_sql_ids_set.insert(sql_ids.begin(), sql_ids.end());
    mysql_mutex_unlock(&LOCK_full_sql_text);
    empty = false;
  } else {
    free_full_sql_text();
  }
  return false; /* success */
}

void free_full_sql_text() {
  mysql_mutex_lock(&LOCK_full_sql_text);
  full_sql_ids_set.clear();
  global_full_sql_text.clear();
  mysql_mutex_unlock(&LOCK_full_sql_text);
  empty = true;
}

void store_full_sql_text(THD *thd, const std::string &query_text) {
  if (!empty && thd->mt_key_is_set(THD::SQL_ID)) {
    std::string sql_id_str(thd_get_sql_id(thd));

    auto fsis_it = full_sql_ids_set.find(sql_id_str);
    if (fsis_it == full_sql_ids_set.end()) {
      return;
    }

    mysql_mutex_lock(&LOCK_full_sql_text);
    auto gfst_it = global_full_sql_text.find(sql_id_str);
    if (gfst_it == global_full_sql_text.end()) {
      global_full_sql_text.emplace(sql_id_str, query_text);
    }
    mysql_mutex_unlock(&LOCK_full_sql_text);
  }
}

std::vector<full_sql_row> get_all_full_sql() {
  std::vector<full_sql_row> full_sql;
  mysql_mutex_lock(&LOCK_full_sql_text);

  for (auto sql_iter = global_full_sql_text.cbegin();
       sql_iter != global_full_sql_text.cend(); ++sql_iter) {
    full_sql.emplace_back(sql_iter->first,    // SQL_ID
                          sql_iter->second);  // SQL_TEXT
  }

  mysql_mutex_unlock(&LOCK_full_sql_text);
  return full_sql;
}

/***********************************************************************
                End - Functions to support full SQL
************************************************************************/

/*
  Stores the client attribute names
*/
void store_client_attribute_names(char *new_value) {
  std::vector<std::string> new_attr_names = split_into_vector(new_value, ',');

  mysql_mutex_lock(&LOCK_client_attribute_names);
  client_attribute_names = new_attr_names;
  mysql_mutex_unlock(&LOCK_client_attribute_names);
}

/*
  Initializes sql info related variables/structures at instance start
*/
void init_sql_info() {
  // set the current sql findings size limit
  current_max_sql_findings_size = max_sql_findings_size;
}

/***********************************************************************
          Begin - Functions to support full SQL Plan Capture
************************************************************************/

/* The main flag to turn ON/OFF/HARD_OFF SQL Plans feature */
ulong sql_plans_control{SQL_INFO_CONTROL_OFF_HARD};
/* The maximum space in bytes available for storing SQL Plans */
ulonglong sql_plans_max_buffer;
/* The sampling rate to be used for capturing SQL Plans */
uint sql_plans_sampling_rate;

/* This control allows SQL Plans to be saved with pruned expr trees */
bool sql_plans_norm_prune_expr_trees;
/* This control allows SQL Plans to be saved with pruned IN lists */
bool sql_plans_norm_prune_in_lists;
/* This control allows SQL Plans to be saved with argument counts
 * when possible, used in conjunction with above two flags */
bool sql_plans_norm_use_arg_counts;
/* This control prevents capturing SQL Plans for system tables
 * Currently this includes table in performance_schema and
 * information_schema databases. */
bool sql_plans_skip_builtin_db;

bool plan_capture_check_footprint();
bool plan_capture_check_sql_command(THD *thd);
bool plan_capture_check_sampling_rate();
std::string capture_query_tree_plan(THD *thd);

// the main plan hash table, with normalized plan hash serving as key
std::unordered_map<digest_key, Plan_val> plan_ht;

// Total footprint in bytes of the plan data
ulonglong current_sql_plans_buffer;

// Total stmts that passed all checks and were ready to be sampled
std::atomic<ulonglong> sql_plans_total_stmts_seen;
// Total stmts that got sampled (due to sampling_rate)
std::atomic<ulonglong> sql_plans_total_stmts_sampled;
// Total slow queries for which sql query plan was captured
std::atomic<ulonglong> sql_plans_total_slow_queries;

void capture_query_plan(THD *thd, bool slow_query_plan_capture) {
  DBUG_TRACE;
  bool regular_plan_capture{(sql_plans_control == SQL_INFO_CONTROL_ON)};

  /*
     Plan capture should get triggered if:

     1. Plan capture feature
        a) plan_capture feature is turned ON (already checked)
        b) plan_capture memory and disk footprints are under the limit
        c) current plan should be sampled

     2. Slow query plan capture (already checked in mysql_execute_command)
        a) slow query logging turned on
        b) slow query plan capture is turned on
        c) current query was a slow query

     3  For both the above cases, there is some additional criteria:
        a) current SQL command is valid for plan capture (SELECT statement),
        b) current statement involves a table (to avoid statements that have
           uninteresting plans, like 'SELECT @@<session var>
        c) If `sql_plans_skip_builtin_db` is turned on, then avoid capturing
           plans for system tables (tables in  P_S, I_S, sys, mysql DBs)
   */

  assert((sql_plans_control == SQL_INFO_CONTROL_ON) ||
         (slow_query_plan_capture));  // 1a.

  if (plan_capture_check_sql_command(thd))  // 3a, 3b, 3c
    return;

  /*
    if regular sql plan capture is ON, we need to check
     the sampling and buffer capacity before proceeding
   */
  if ((sql_plans_control == SQL_INFO_CONTROL_ON) &&
      (plan_capture_check_footprint() ||     // 1b
       plan_capture_check_sampling_rate()))  // 1c
    regular_plan_capture = false;

  if ((regular_plan_capture == false) && (slow_query_plan_capture == false))
    return;

  // If we're here, then we can trigger the plan capture

  ulong plan_format_modifiers{0};

  if (sql_plans_norm_prune_expr_trees)
    plan_format_modifiers |= THD::Plan_format::PRUNE_EXPR_TREES;

  if (sql_plans_norm_use_arg_counts)
    plan_format_modifiers |= THD::Plan_format::USE_ARG_COUNTS;

  if (sql_plans_norm_prune_in_lists)
    plan_format_modifiers |= THD::Plan_format::PRUNE_IN_LISTS;

  thd->set_plan_capture(true, plan_format_modifiers);

  // Next, capture the query plan for hashing
  // Note that this output should already be a "normalized" plan
  thd->captured_sql_plan = capture_query_tree_plan(thd);

  thd->set_plan_capture(false);

  /* exit now if plan was captured purely for slow queries */
  if (regular_plan_capture == false) {
    if (!thd->captured_sql_plan.empty()) sql_plans_total_slow_queries++;

    return;
  }

  if (thd->captured_sql_plan.empty()) {
    // nothing captured
    // decr counter to make sure sampling logic works;
    sql_plans_total_stmts_seen--;
    return;
  } else if (slow_query_plan_capture) {
    /*
       increment slow query counter when both slow_query_plan_capture and
       regular_plan_capture are true
     */
    sql_plans_total_slow_queries++;
  }

  sql_plans_total_stmts_sampled++;

  // at this point we should have the normalized plan captured

  digest_key plan_id;
  compute_md5_hash((char *)plan_id.data(),
                   (const char *)thd->captured_sql_plan.data(),
                   thd->captured_sql_plan.length());
  thd->mt_key_set(THD::PLAN_ID, plan_id.data(), MD5_HASH_SIZE);

  // Check if this can be avoided by picking from THD or elsewhere
  ulonglong now = my_getsystime() / 10000000;

  mysql_mutex_lock(&LOCK_plan_ht);

  // Lookup plan hash in plan_ht
  const auto sql_plan_it = plan_ht.find(thd->mt_key_value(THD::PLAN_ID));
  if (sql_plan_it == plan_ht.end()) {
    plan_ht[plan_id] = {1, now, thd->captured_sql_plan};

    current_sql_plans_buffer +=
        MD5_HASH_SIZE + sizeof(Plan_val) + thd->captured_sql_plan.length();

  } else {
    // hash key already present in plan_ht
    // Just increment the counter and update the last_recorded
    sql_plan_it->second.count_occur++;
    sql_plan_it->second.last_recorded = now;
  }

  mysql_mutex_unlock(&LOCK_plan_ht);
}

bool plan_capture_check_footprint() {
  DBUG_TRACE;
  // This way, we will end up storing one extra plan above the configured
  // limit, which should be okay
  return (current_sql_plans_buffer >= sql_plans_max_buffer);
}

bool plan_capture_check_sql_command(THD *thd) {
  DBUG_TRACE;
  //
  THD::Query_plan *qp = &thd->query_plan;

  /* At the outset, we don't want to capture the plan for a:
     1. prepared query
     2. a query that cannot be explained (already checked outside)
     3. an explain query (probably not needed as this flag may not be set for
     EXPLAINS: sql_command_flags[command] & CF_CAN_BE_EXPLAINED
     4. a stored routine query
   */
  assert(is_explainable_query(qp->get_command())); // (2)

  if (qp->is_ps_query() ||                         // (1)
      qp->get_lex()->is_explain() ||               // (3)
      qp->get_lex()->sphead != nullptr)            // (4)
  {
    return true;
  }

  // For now, only capture plans for SELECT commands:
  if (qp->get_command() != SQLCOM_SELECT) return true;

  // Skip commands that likely won't have interesting plans
  if (!qp->get_lex()->query_tables) {
    // the command doesn't have an associated table
    return true;
  }

  // Additionally, filter out plans involving P_S, I_S and other builtin DBs
  // when sql_plans_skip_builtin_db is set

  if (sql_plans_skip_builtin_db &&
      is_mysql_builtin_database(qp->get_lex()->query_tables->db)) {
    // matches a system DB
    return true;
  }

  return false;
}

bool plan_capture_check_sampling_rate() {
  DBUG_TRACE;
  // check the global plan_capture state
  // increment counter for valid SQL CMDs that have reached this point
  // see if current plan should be sampled

  // NOTE: if plan capture fails, we need to decrement the counter,
  // so that the next capture (or increment the counter only after
  // plan capture completed)

  sql_plans_total_stmts_seen++;
  if (sql_plans_total_stmts_seen % sql_plans_sampling_rate) return true;

  return false;
}

std::vector<sql_plan_row> get_all_sql_plans() {
  std::vector<sql_plan_row> sql_plans;
  mysql_mutex_lock(&LOCK_plan_ht);

  for (auto sql_plans_iter = plan_ht.cbegin(); sql_plans_iter != plan_ht.cend();
       ++sql_plans_iter) {
    char plan_id_string[DIGEST_HASH_TO_STRING_LENGTH + 1];

    DIGEST_HASH_TO_STRING(sql_plans_iter->first.data(), plan_id_string);
    plan_id_string[DIGEST_HASH_TO_STRING_LENGTH] = '\0';

    sql_plans.emplace_back(plan_id_string, sql_plans_iter->second.count_occur,
                           sql_plans_iter->second.last_recorded,
                           sql_plans_iter->second.plan);
  }

  mysql_mutex_unlock(&LOCK_plan_ht);
  return sql_plans;
}

void reset_sql_plans() {
  mysql_mutex_lock(&LOCK_plan_ht);

  plan_ht.clear();

  current_sql_plans_buffer = 0;
  sql_plans_total_stmts_seen = 0;
  sql_plans_total_stmts_sampled = 0;

  mysql_mutex_unlock(&LOCK_plan_ht);
}

int get_captured_plan_count() {
  // return the total number of entries in the plan map so far
  // This is used by the optimizer when queries the P_S.SQL_PLANS table

  return plan_ht.size();
}

/***********************************************************************
               End - Functions to support SQL Plan Capture
************************************************************************/
