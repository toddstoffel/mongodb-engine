#ifndef HA_MONGODB_INCLUDED
#define HA_MONGODB_INCLUDED

/*
  MongoDB Storage Engine for MariaDB
  
  This storage engine enables querying MongoDB collections as virtual SQL tables,
  supporting cross-engine joins and SQL-to-MongoDB query translation.
  
  Copyright (c) 2025 MongoDB Storage Engine Contributors
  
  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.
*/

// MariaDB includes (order is critical!)
#define MYSQL_SERVER 1
#include "my_global.h"
#include "handler.h"
#include "thr_lock.h"
#include "my_base.h"

// MongoDB C driver includes
#include <mongoc/mongoc.h>
#include <bson/bson.h>

// Include forward declarations for MongoDB components
#include "mongodb_schema.h"

// Forward declarations
class MongoConnectionPool;
class MongoQueryTranslator;
class MongoCursorManager;

/*
  MongoDB server connection information - shared among all handlers
  for the same MongoDB server to enable connection pooling
*/
typedef struct st_mongodb_server {
  MEM_ROOT mem_root;
  uint use_count, io_count;

  uchar *key;
  uint key_length;

  const char *scheme;           // mongodb:// or mongodb+srv://
  const char *hostname;
  const char *username;
  const char *password;
  const char *database;
  const char *auth_source;
  const char *replica_set;
  ushort port;
  bool ssl_enabled;

  mysql_mutex_t mutex;
  MongoConnectionPool *connection_pool;
} MONGODB_SERVER;

/*
  MONGODB_SHARE is shared among all open handlers for the same table.
  It contains parsed connection information and cached schema data.
*/
typedef struct st_mongodb_share {
  MEM_ROOT mem_root;

  bool parsed;
  const char *share_key;        // Unique identifier: database/collection
  char *connection_string;      // Original CONNECTION string
  char *mongo_connection_string; // MongoDB C driver compatible string (without collection)

  // Parsed MongoDB connection components
  char *hostname;
  char *username;
  char *password;
  char *database_name;
  char *collection_name;
  char *auth_source;
  char *replica_set;
  ushort port;
  bool ssl_enabled;

  // Schema management
  MongoSchemaRegistry *schema_registry;
  bool schema_inferred;
  std::vector<MongoFieldMapping> field_mappings;
  time_t schema_last_updated;
  
  // Statistics
  ha_rows records;
  ulong mean_rec_length;
  time_t create_time;
  time_t update_time;

  // Sharing and locking
  int share_key_length;
  uint use_count;
  THR_LOCK lock;
  mysql_mutex_t mutex;
  
  MONGODB_SERVER *server;
} MONGODB_SHARE;

/*
  Error codes specific to MongoDB storage engine
*/
#define HA_MONGODB_ERROR_WITH_REMOTE_SYSTEM 10000
#define HA_MONGODB_ERROR_CONNECTION_FAILED  10001
#define HA_MONGODB_ERROR_AUTH_FAILED        10002
#define HA_MONGODB_ERROR_COLLECTION_NOT_FOUND 10003
#define HA_MONGODB_ERROR_QUERY_TRANSLATION_FAILED 10004
#define HA_MONGODB_ERROR_SCHEMA_INFERENCE_FAILED 10005
#define HA_MONGODB_ERROR_DOCUMENT_CONVERSION_FAILED 10006

/*
  Buffer sizes and limits
*/
#define MONGODB_QUERY_BUFFER_SIZE STRING_BUFFER_USUAL_SIZE * 5
#define MONGODB_RECORDS_IN_RANGE 2
#define MONGODB_MAX_KEY_LENGTH 3500

/*
  Class definition for the MongoDB storage engine handler
*/
class ha_mongodb final : public handler
{
  friend int mongodb_db_init(void *p);

  THR_LOCK_DATA lock;           // MariaDB lock integration
  MONGODB_SHARE *share;         // Shared table metadata
  
  // MongoDB-specific components
  mongoc_client_t *client;      // MongoDB client connection
  mongoc_collection_t *collection; // MongoDB collection handle
  mongoc_cursor_t *cursor;      // Current query cursor
  const bson_t *current_doc;    // Current document being processed
  
  // Query and schema management
  MongoQueryTranslator *translator;
  MongoCursorManager *cursor_manager;
  
  // Query state
  bson_t *pushed_condition;     // Condition pushed down to MongoDB
  bson_t *sort_spec;           // ORDER BY specification for MongoDB
  bool position_called;         // Track if position() was called
  ha_rows scan_position;        // Current position in table scan (for rnd_pos support)
  
  // Error handling
  int remote_error_number;
  char remote_error_buf[MONGODB_QUERY_BUFFER_SIZE];

private:
  /*
    Internal helper methods
  */
  MONGODB_SHARE *get_share();
  int free_share();
  int parse_connection_string(const char *connection_string);
  int connect_to_mongodb();
  void disconnect_from_mongodb();
  int stash_remote_error();
  
  /*
    Data conversion methods
  */
  // Document-to-row conversion methods (virtual column approach)
  int convert_document_to_row(const bson_t *doc, uchar *buf);
  int convert_full_document_field(const bson_t *doc, Field *field, uint array_index);
  int convert_simple_field_from_document(const bson_t *doc, Field *field, const char *field_name);
  int convert_mongodb_id_field(const bson_t *doc, Field *field);
  int convert_bson_value_to_field(bson_iter_t *iter, Field *field, MongoFieldMapping *mapping);
  int convert_row_to_document(const uchar *buf, bson_t **doc);
  
  /*
    Query building helpers
  */
  bson_t *build_find_filter(const Item *cond);
  bson_t *build_aggregation_pipeline(const Item *where_cond, 
                                    const ORDER *order_by, 
                                    ha_rows limit_count);

public:
  ha_mongodb(handlerton *hton, TABLE_SHARE *table_arg);
  ~ha_mongodb();

  /*
    Storage engine capability flags - Remove problematic flags
  */
  ulonglong table_flags() const override
  {
    return (HA_FILE_BASED | HA_REC_NOT_IN_SEQ | HA_AUTO_PART_KEY |
            HA_CAN_INDEX_BLOBS | HA_BINLOG_ROW_CAPABLE | 
            HA_BINLOG_STMT_CAPABLE | HA_PARTIAL_COLUMN_READ |
            HA_NULL_IN_KEY | HA_STATS_RECORDS_IS_EXACT);
  }

  ulong index_flags(uint inx, uint part, bool all_parts) const override
  {
    return (HA_READ_NEXT | HA_READ_RANGE | HA_KEYREAD_ONLY);
  }

  /*
    Storage engine limits
  */
  uint max_supported_record_length() const override { return HA_MAX_REC_LENGTH; }
  uint max_supported_keys() const override { return MAX_KEY; }
  uint max_supported_key_parts() const override { return MAX_REF_PARTS; }
  uint max_supported_key_length() const override { return MONGODB_MAX_KEY_LENGTH; }
  uint max_supported_key_part_length() const override { return MONGODB_MAX_KEY_LENGTH; }

  /*
    Cost estimation for query optimization
  */
  IO_AND_CPU_COST scan_time() override;
  IO_AND_CPU_COST keyread_time(uint index, ulong ranges, ha_rows rows, ulonglong blocks) override;
  IO_AND_CPU_COST rnd_pos_time(ha_rows rows) override;

  /*
    Required MariaDB storage engine interface methods
  */
  int open(const char *name, int mode, uint test_if_locked) override;
  int close(void) override;
  
  // Table scanning operations
  int rnd_init(bool scan) override;
  int rnd_next(uchar *buf) override;
  int rnd_end() override;
  int rnd_pos(uchar *buf, uchar *pos) override;
  void position(const uchar *record) override;
  
  // Index operations (MongoDB index utilization)
  int index_init(uint keynr, bool sorted) override;
  int index_read_map(uchar *buf, const uchar *key, key_part_map keypart_map, enum ha_rkey_function find_flag) override;
  int index_next(uchar *buf) override;
  int index_end() override;
  
  // Data modification operations (future implementation)
  int write_row(const uchar *buf) override;
  int update_row(const uchar *old_data, const uchar *new_data) override;
  int delete_row(const uchar *buf) override;
  
  // Table management
  int create(const char *name, TABLE *form, HA_CREATE_INFO *create_info) override;
  int delete_table(const char *name) override;
  
  // Metadata and statistics
  int info(uint) override;
  ha_rows estimate_rows_upper_bound() override;
  
  // Condition pushdown for query optimization
  const Item *cond_push(const Item *cond) override;
  void cond_pop() override;
  
  // Locking integration
  int external_lock(THD *thd, int lock_type) override;
  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to, enum thr_lock_type lock_type) override;
  
  // Error handling
  bool get_error_message(int error, String *buf) override;
  
  // Additional operations
  int extra(enum ha_extra_function operation) override;
  int reset(void) override;
  
  // Statistics for optimizer
  ha_rows records_in_range(uint inx, const key_range *start_key,
                          const key_range *end_key, page_range *pages) override;

  /*
    Transaction support (MongoDB DOES support transactions)
    Static methods required by handlerton registration
  */
  static int commit(THD *thd, bool all);
  static int rollback(THD *thd, bool all);

  /*
    MongoDB-specific public interface
  */
  const MONGODB_SHARE *get_mongodb_share() const { return share; }
};

/*
  Global functions for MongoDB storage engine
*/
extern const char mongodb_ident_quote_char;    // Character for quoting identifiers
extern const char mongodb_value_quote_char;    // Character for quoting literals

/*
  Connection and schema management functions
*/
extern MongoConnectionPool *get_connection_pool(MONGODB_SERVER *server);
extern int mongodb_parse_connection_string(const char *connection_string, MONGODB_SHARE *share);

#endif /* HA_MONGODB_INCLUDED */
