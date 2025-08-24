/* 
   MongoDB Storage Engine for MariaDB - Handler Implementation
   
   This file contains the core handler class implementation for the MongoDB storage engine.
   It provides the minimal interface required by MariaDB to load and operate the plugin.
*/

#define MYSQL_SERVER 1
#include "my_global.h"
#include "field.h"
#include "table.h"
#include "ha_mongodb.h"
#include "mongodb_connection.h"

/* 
   Constructor - Initialize a new handler instance
*/
ha_mongodb::ha_mongodb(handlerton *hton, TABLE_SHARE *table_arg)
  : handler(hton, table_arg), share(nullptr), cursor(nullptr)
{
  // Initialize basic state
}

/*
   Destructor - Clean up handler instance
*/
ha_mongodb::~ha_mongodb()
{
  // Cleanup resources - don't call close() to avoid recursion
}

/*
   Open - Initialize handler for table access
*/
int ha_mongodb::open(const char *name, int mode, uint test_if_locked)
{
  DBUG_ENTER("ha_mongodb::open");
  
  // Get or create the shared table metadata
  if (!(share = get_share()))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  
  // Parse connection string if not already done
  if (!share->parsed)
  {
    if (parse_connection_string(table->s->connect_string.str))
    {
      free_share();
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
    }
    share->parsed = true;
  }
  
  // Don't connect to MongoDB here - wait until first query
  // This allows table creation even with invalid connections
  
  DBUG_RETURN(0);
  
  DBUG_RETURN(0);
}

/*
   Close - Clean up handler resources
*/
int ha_mongodb::close(void)
{
  DBUG_ENTER("ha_mongodb::close");
  
  // Clean up MongoDB resources
  disconnect_from_mongodb();
  
  // Free the shared table metadata
  if (share)
  {
    free_share();
    share = nullptr;
  }
  
  DBUG_RETURN(0);
}

/*
   Table scan initialization
*/
int ha_mongodb::rnd_init(bool scan)
{
  DBUG_ENTER("ha_mongodb::rnd_init");
  
  // Ensure we have a valid MongoDB connection
  if (!collection)
  {
    if (connect_to_mongodb())
    {
      // Return the specific error that was stored in connect_to_mongodb()
      DBUG_RETURN(remote_error_number ? remote_error_number : HA_ERR_INTERNAL_ERROR);
    }
  }
  
  // Create a cursor for scanning all documents
  cursor = mongoc_collection_find_with_opts(collection, 
                                           bson_new(), // Empty filter = scan all
                                           nullptr,    // No options
                                           nullptr);   // No read prefs
  
  if (!cursor)
  {
    stash_remote_error();
    DBUG_RETURN(HA_ERR_GENERIC);
  }
  
  current_doc = nullptr;
  DBUG_RETURN(0);
}

/*
   Fetch next row in table scan
*/
int ha_mongodb::rnd_next(uchar *buf)
{
  DBUG_ENTER("ha_mongodb::rnd_next");
  
  if (!cursor)
  {
    DBUG_RETURN(HA_ERR_END_OF_FILE);
  }
  
  // Fetch next document from MongoDB
  if (!mongoc_cursor_next(cursor, &current_doc))
  {
    // Check for errors
    bson_error_t error;
    if (mongoc_cursor_error(cursor, &error))
    {
      stash_remote_error();
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
    }
    
    // No more documents
    DBUG_RETURN(HA_ERR_END_OF_FILE);
  }
  
  // Convert MongoDB document to MariaDB row
  if (convert_document_to_row(current_doc, buf))
  {
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  }
  
  DBUG_RETURN(0);
}

/*
   End table scan
*/
int ha_mongodb::rnd_end()
{
  DBUG_ENTER("ha_mongodb::rnd_end");
  
  // Clean up cursor
  if (cursor)
  {
    mongoc_cursor_destroy(cursor);
    cursor = nullptr;
  }
  current_doc = nullptr;
  DBUG_RETURN(0);
}

/*
   Get table information
*/
int ha_mongodb::info(uint flag)
{
  DBUG_ENTER("ha_mongodb::info");
  
  // Set basic stats
  stats.records = 0;
  stats.mean_rec_length = 0;
  stats.data_file_length = 0;
  stats.index_file_length = 0;
  stats.max_data_file_length = 0;
  stats.delete_length = 0;
  stats.auto_increment_value = 0;
  
  DBUG_RETURN(0);
}

/*
   Create table - stub implementation  
*/
int ha_mongodb::create(const char *name, TABLE *form, HA_CREATE_INFO *create_info)
{
  DBUG_ENTER("ha_mongodb::create");
  DBUG_RETURN(0);
}

/*
   Delete table - stub implementation
*/
int ha_mongodb::delete_table(const char *name)
{
  DBUG_ENTER("ha_mongodb::delete_table");
  DBUG_RETURN(0);
}

/*
   Position cursor - stub implementation
*/
void ha_mongodb::position(const uchar *record)
{
  DBUG_ENTER("ha_mongodb::position");
  DBUG_VOID_RETURN;
}

/*
   Random position read - stub implementation
*/
int ha_mongodb::rnd_pos(uchar *buf, uchar *pos)
{
  DBUG_ENTER("ha_mongodb::rnd_pos");
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}

/*
   Index operations - stub implementations
*/
int ha_mongodb::index_init(uint keynr, bool sorted)
{
  DBUG_ENTER("ha_mongodb::index_init");
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}

int ha_mongodb::index_read_map(uchar *buf, const uchar *key, key_part_map keypart_map, enum ha_rkey_function find_flag)
{
  DBUG_ENTER("ha_mongodb::index_read_map");
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}

int ha_mongodb::index_next(uchar *buf)
{
  DBUG_ENTER("ha_mongodb::index_next");
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}

int ha_mongodb::index_end()
{
  DBUG_ENTER("ha_mongodb::index_end");
  DBUG_RETURN(0);
}

/*
   Data modification operations - stub implementations
*/
int ha_mongodb::write_row(const uchar *buf)
{
  DBUG_ENTER("ha_mongodb::write_row");
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}

int ha_mongodb::update_row(const uchar *old_data, const uchar *new_data)
{
  DBUG_ENTER("ha_mongodb::update_row");
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}

int ha_mongodb::delete_row(const uchar *buf)
{
  DBUG_ENTER("ha_mongodb::delete_row");
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}

/*
   Statistics and metadata - stub implementations
*/
ha_rows ha_mongodb::estimate_rows_upper_bound()
{
  DBUG_ENTER("ha_mongodb::estimate_rows_upper_bound");
  DBUG_RETURN(HA_POS_ERROR);
}

IO_AND_CPU_COST ha_mongodb::scan_time()
{
  DBUG_ENTER("ha_mongodb::scan_time");
  IO_AND_CPU_COST cost;
  cost.io = 0.0;
  cost.cpu = 0.0;
  DBUG_RETURN(cost);
}

IO_AND_CPU_COST ha_mongodb::keyread_time(uint index, ulong ranges, ha_rows rows, ulonglong blocks)
{
  DBUG_ENTER("ha_mongodb::keyread_time");
  IO_AND_CPU_COST cost;
  cost.io = 0.0;
  cost.cpu = 0.0;
  DBUG_RETURN(cost);
}

IO_AND_CPU_COST ha_mongodb::rnd_pos_time(ha_rows rows)
{
  DBUG_ENTER("ha_mongodb::rnd_pos_time");
  IO_AND_CPU_COST cost;
  cost.io = 0.0;
  cost.cpu = 0.0;
  DBUG_RETURN(cost);
}

ha_rows ha_mongodb::records_in_range(uint inx, const key_range *min_key, const key_range *max_key, page_range *pages)
{
  DBUG_ENTER("ha_mongodb::records_in_range");
  DBUG_RETURN(10);
}

/*
   Condition pushdown - stub implementations
*/
const Item *ha_mongodb::cond_push(const Item *cond)
{
  DBUG_ENTER("ha_mongodb::cond_push");
  DBUG_RETURN(cond);
}

void ha_mongodb::cond_pop()
{
  DBUG_ENTER("ha_mongodb::cond_pop");
  DBUG_VOID_RETURN;
}

/*
   Locking operations - stub implementations
*/
int ha_mongodb::external_lock(THD *thd, int lock_type)
{
  DBUG_ENTER("ha_mongodb::external_lock");
  DBUG_RETURN(0);
}

THR_LOCK_DATA **ha_mongodb::store_lock(THD *thd, THR_LOCK_DATA **to, enum thr_lock_type lock_type)
{
  DBUG_ENTER("ha_mongodb::store_lock");
  DBUG_RETURN(to);
}

/*
   Error handling - stub implementation
*/
bool ha_mongodb::get_error_message(int error, String *buf)
{
  DBUG_ENTER("ha_mongodb::get_error_message");
  DBUG_RETURN(false);
}

/*
   Additional operations - stub implementations
*/
int ha_mongodb::extra(enum ha_extra_function operation)
{
  DBUG_ENTER("ha_mongodb::extra");
  DBUG_RETURN(0);
}

int ha_mongodb::reset()
{
  DBUG_ENTER("ha_mongodb::reset");
  DBUG_RETURN(0);
}

/*
  Helper method implementations
*/

MONGODB_SHARE *ha_mongodb::get_share()
{
  DBUG_ENTER("ha_mongodb::get_share");
  
  // For Phase 1, just allocate a simple share
  if (!share)
  {
    share = (MONGODB_SHARE*)my_malloc(PSI_NOT_INSTRUMENTED, sizeof(MONGODB_SHARE), MYF(MY_WME | MY_ZEROFILL));
    if (share)
    {
      share->use_count = 1;
      share->parsed = false;
    }
  }
  else
  {
    share->use_count++;
  }
  
  DBUG_RETURN(share);
}

int ha_mongodb::free_share()
{
  DBUG_ENTER("ha_mongodb::free_share");
  
  if (share && --share->use_count == 0)
  {
    my_free(share);
    share = nullptr;
  }
  
  DBUG_RETURN(0);
}

int ha_mongodb::parse_connection_string(const char *connection_string)
{
  DBUG_ENTER("ha_mongodb::parse_connection_string");
  
  if (!connection_string || !*connection_string)
  {
    DBUG_RETURN(1);
  }
  
  // Use our new URI parser for comprehensive validation
  std::string error_message;
  if (!validate_mongodb_connection_string(connection_string, error_message))
  {
    // TODO: Add proper error logging when available
    // sql_print_error("MongoDB storage engine: Invalid connection string: %s", error_message.c_str());
    DBUG_RETURN(1);
  }
  
  // Parse the URI components
  MongoURI parsed = parse_mongodb_connection_string(connection_string);
  if (!parsed.is_valid)
  {
    // TODO: Add proper error logging when available  
    // sql_print_error("MongoDB storage engine: URI parsing failed: %s", parsed.error_message.c_str());
    DBUG_RETURN(1);
  }
  
  // Store the parsed connection information in the share
  share->connection_string = my_strdup(PSI_NOT_INSTRUMENTED, connection_string, MYF(0));
  share->database_name = my_strdup(PSI_NOT_INSTRUMENTED, parsed.database.c_str(), MYF(0));
  share->collection_name = my_strdup(PSI_NOT_INSTRUMENTED, parsed.collection.c_str(), MYF(0));
  
  // Store the MongoDB-compatible connection string (without collection)
  std::string mongo_conn = parsed.to_connection_string();
  share->mongo_connection_string = my_strdup(PSI_NOT_INSTRUMENTED, mongo_conn.c_str(), MYF(0));
  
  DBUG_RETURN(0);
}

int ha_mongodb::connect_to_mongodb()
{
  DBUG_ENTER("ha_mongodb::connect_to_mongodb");
  
  if (!share || !share->mongo_connection_string || !share->database_name || !share->collection_name)
  {
    DBUG_RETURN(1);
  }
  
  // Get or create a connection from the pool
  MongoConnectionPool* pool = get_or_create_connection_pool(share->mongo_connection_string);
  if (!pool || !pool->is_connection_valid())
  {
    // TODO: Add proper error logging
    // sql_print_error("MongoDB storage engine: Failed to create connection pool: %s", 
    //                 pool ? pool->get_connection_error().c_str() : "Unknown error");
    DBUG_RETURN(1);
  }
  
  // Acquire a connection from the pool
  client = pool->acquire_connection();
  if (!client)
  {
    // TODO: Add proper error logging
    // sql_print_error("MongoDB storage engine: Failed to acquire connection from pool");
    DBUG_RETURN(1);
  }
  
  // Get the database and collection handles
  mongoc_database_t* database = mongoc_client_get_database(client, share->database_name);
  if (!database)
  {
    pool->release_connection(client);
    client = nullptr;
    DBUG_RETURN(1);
  }
  
  collection = mongoc_database_get_collection(database, share->collection_name);
  mongoc_database_destroy(database);
  
  if (!collection)
  {
    pool->release_connection(client);
    client = nullptr;
    DBUG_RETURN(1);
  }
  
  DBUG_RETURN(0);
}

void ha_mongodb::disconnect_from_mongodb()
{
  DBUG_ENTER("ha_mongodb::disconnect_from_mongodb");
  
  if (cursor)
  {
    mongoc_cursor_destroy(cursor);
    cursor = nullptr;
  }
  
  if (collection)
  {
    mongoc_collection_destroy(collection);
    collection = nullptr;
  }
  
  if (client && share && share->mongo_connection_string)
  {
    // Release the connection back to the pool instead of destroying it
    MongoConnectionPool* pool = get_or_create_connection_pool(share->mongo_connection_string);
    if (pool)
    {
      pool->release_connection(client);
    }
    client = nullptr;
  }
  
  current_doc = nullptr;
  DBUG_VOID_RETURN;
}

int ha_mongodb::stash_remote_error()
{
  DBUG_ENTER("ha_mongodb::stash_remote_error");
  
  // For Phase 1, just set a generic error
  remote_error_number = HA_ERR_INTERNAL_ERROR;
  strcpy(remote_error_buf, "MongoDB operation failed");
  
  DBUG_RETURN(remote_error_number);
}

int ha_mongodb::convert_document_to_row(const bson_t *doc, uchar *buf)
{
  DBUG_ENTER("ha_mongodb::convert_document_to_row");
  
  if (!doc || !buf)
  {
    DBUG_RETURN(1);
  }
  
  // Phase 1: Basic document to row conversion
  // For now, we'll just handle basic fields that match the table schema
  
  Field **field_ptr;
  bson_iter_t iter;
  
  // Initialize all fields to NULL first
  for (field_ptr = table->field; *field_ptr; field_ptr++)
  {
    (*field_ptr)->set_null();
  }
  
  // Iterate through table fields and try to map them from the document
  for (field_ptr = table->field; *field_ptr; field_ptr++)
  {
    Field *field = *field_ptr;
    const char *field_name = field->field_name.str;
    
    // Special handling for _id field
    if (strcmp(field_name, "_id") == 0 || strcmp(field_name, "id") == 0)
    {
      bson_iter_init(&iter, doc);
      if (bson_iter_find(&iter, "_id"))
      {
        if (BSON_ITER_HOLDS_OID(&iter))
        {
          const bson_oid_t *oid = bson_iter_oid(&iter);
          char oid_str[25];
          bson_oid_to_string(oid, oid_str);
          field->set_notnull();
          field->store(oid_str, strlen(oid_str), system_charset_info);
        }
        continue;
      }
    }
    
    // Try to find field in document
    bson_iter_init(&iter, doc);
    if (bson_iter_find(&iter, field_name))
    {
      field->set_notnull();
      
      // Convert based on BSON type
      if (BSON_ITER_HOLDS_UTF8(&iter))
      {
        uint32_t str_len;
        const char *str_val = bson_iter_utf8(&iter, &str_len);
        field->store(str_val, str_len, system_charset_info);
      }
      else if (BSON_ITER_HOLDS_INT32(&iter))
      {
        int32_t int_val = bson_iter_int32(&iter);
        field->store(int_val);
      }
      else if (BSON_ITER_HOLDS_INT64(&iter))
      {
        int64_t long_val = bson_iter_int64(&iter);
        field->store(long_val);
      }
      else if (BSON_ITER_HOLDS_DOUBLE(&iter))
      {
        double double_val = bson_iter_double(&iter);
        field->store(double_val);
      }
      else if (BSON_ITER_HOLDS_BOOL(&iter))
      {
        bool bool_val = bson_iter_bool(&iter);
        field->store(bool_val ? 1 : 0);
      }
      // For other types, leave as NULL for now
    }
  }
  
  DBUG_RETURN(0);
}

/*
  Transaction support methods (MongoDB DOES support transactions)
  These are static methods required by the handlerton registration
*/
int ha_mongodb::commit(THD *thd, bool all)
{
  DBUG_ENTER("ha_mongodb::commit");
  // TODO: Implement MongoDB transaction commit for Phase 4
  // For now, just return success for Phase 1 compatibility
  DBUG_RETURN(0);
}

int ha_mongodb::rollback(THD *thd, bool all)
{
  DBUG_ENTER("ha_mongodb::rollback");
  // TODO: Implement MongoDB transaction rollback for Phase 4
  // For now, just return success for Phase 1 compatibility
  DBUG_RETURN(0);
}
