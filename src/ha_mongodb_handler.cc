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
#include "mongodb_schema.h"

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
  
  // DEBUG
  fprintf(stderr, "OPEN CALLED! name=%s, mode=%d\n", name ? name : "NULL", mode);
  fflush(stderr);
  
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
   Table scan initialization with schema inference
*/
int ha_mongodb::rnd_init(bool scan)
{
  DBUG_ENTER("ha_mongodb::rnd_init");
  
  // DEBUG
  fprintf(stderr, "RND_INIT CALLED! scan=%d\n", scan);
  fflush(stderr);
  
  // Simple test - just try to connect to MongoDB
  if (!collection)
  {
    if (connect_to_mongodb())
    {
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
    }
  }
  
  // Create a simple cursor for scanning all documents
  bson_t *query = bson_new();  // Empty query = scan all
  cursor = mongoc_collection_find_with_opts(collection, query, nullptr, nullptr);
  bson_destroy(query);
  
  if (!cursor)
  {
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
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
  
  // IMMEDIATE DEBUG: Print to stderr to see if this method is called
  fprintf(stderr, "RND_NEXT CALLED! table=%p, buf=%p\n", table, buf);
  fflush(stderr);
  
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
  if (!current_doc)
  {
    // Document is NULL - this shouldn't happen
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  }
  
  // CRITICAL: Set the record buffer to the table's record format
  memset(buf, 0, table->s->reclength);
  
  // Convert and pack fields into the record buffer
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
  
  // DEBUG
  fprintf(stderr, "RND_POS CALLED! buf=%p, pos=%p\n", buf, pos);
  fflush(stderr);
  
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
  
  // TEMPORARY: Hardcode parsing for testing
  // Expected: mongodb://tom:jerry@holly.local:27017/classicmodels/customers
  
  share->connection_string = my_strdup(PSI_NOT_INSTRUMENTED, connection_string, MYF(0));
  share->database_name = my_strdup(PSI_NOT_INSTRUMENTED, "classicmodels", MYF(0));
  share->collection_name = my_strdup(PSI_NOT_INSTRUMENTED, "customers", MYF(0));
  share->mongo_connection_string = my_strdup(PSI_NOT_INSTRUMENTED, "mongodb://tom:jerry@holly.local:27017/?ssl=false", MYF(0));
  
  DBUG_RETURN(0);
}

int ha_mongodb::connect_to_mongodb()
{
  DBUG_ENTER("ha_mongodb::connect_to_mongodb");
  
  // Check if we have the required fields
  if (!share || !share->mongo_connection_string || !share->database_name || !share->collection_name)
  {
    DBUG_RETURN(1);
  }
  
  // Simple direct connection for testing (bypass connection pool)
  client = mongoc_client_new(share->mongo_connection_string);
  if (!client)
  {
    DBUG_RETURN(1);
  }
  
  // Get the database and collection handles
  mongoc_database_t* database = mongoc_client_get_database(client, share->database_name);
  if (!database)
  {
    mongoc_client_destroy(client);
    client = nullptr;
    DBUG_RETURN(1);
  }
  
  collection = mongoc_database_get_collection(database, share->collection_name);
  mongoc_database_destroy(database);
  
  if (!collection)
  {
    mongoc_client_destroy(client);
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
  
  if (client)
  {
    mongoc_client_destroy(client);
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

/*
  Enhanced document-to-row conversion using virtual column approach
  Strategy: Store _id and full document, use virtual columns for field extraction
*/
int ha_mongodb::convert_document_to_row(const bson_t *doc, uchar *buf)
{
  DBUG_ENTER("ha_mongodb::convert_document_to_row");
  
  if (!doc || !buf)
  {
    DBUG_RETURN(1);
  }
  
  Field **field_ptr;
  
  // Initialize all fields to NULL first
  for (field_ptr = table->field; *field_ptr; field_ptr++)
  {
    (*field_ptr)->set_null();
  }
  
  // Strategy: Store only the document field as JSON
  
  // DEBUG: Print all fields and their indices
  fprintf(stderr, "DEBUG: Table has %u fields:\n", table->s->fields);
  for (uint i = 0; i < table->s->fields; i++) {
    Field *f = table->field[i];
    fprintf(stderr, "  Field[%u]: name='%s', field_index=%u, null_bit=%u\n", 
            i, f->field_name.str, f->field_index, f->null_bit);
  }
  fflush(stderr);
  
  uint field_array_index = 0;
  for (field_ptr = table->field; *field_ptr; field_ptr++, field_array_index++)
  {
    Field *field = *field_ptr;
    const char *field_name = field->field_name.str;
    
    // DEBUG: Print field processing with array index
    fprintf(stderr, "DEBUG: Processing field_name='%s' (length=%lu) at array_index=%u\n", 
            field_name ? field_name : "NULL", field_name ? strlen(field_name) : 0, field_array_index);
    fflush(stderr);
    
    if (strcmp(field_name, "_id") == 0)
    {
      // Handle _id field - extract ObjectId and convert to string
      fprintf(stderr, "DEBUG: Matched _id field at array_index=%u\n", field_array_index);
      fflush(stderr);
      
      field->ptr = buf + field->offset(table->record[0]);
      convert_mongodb_id_field(doc, field);
    }
    else if (strcmp(field_name, "document") == 0)
    {
      // Handle document field - convert full BSON to JSON and pack into buffer
      fprintf(stderr, "DEBUG: Matched document field at array_index=%u - converting to JSON\n", field_array_index);
      fflush(stderr);
      
      // Convert BSON to JSON string using relaxed format (more readable)
      char *json_str = bson_as_relaxed_extended_json(doc, nullptr);
      if (!json_str) {
        fprintf(stderr, "DEBUG: relaxed JSON failed, trying canonical\n");
        fflush(stderr);
        // Fallback to canonical format
        json_str = bson_as_canonical_extended_json(doc, nullptr);
      }
      
      if (json_str) {
        fprintf(stderr, "DEBUG: Successfully converted to JSON: %.200s...\n", json_str);
        fflush(stderr);
        
        // CRITICAL: Set field pointer to point to the row buffer location for this field
        field->ptr = buf + field->offset(table->record[0]);
        
        // Store the JSON string in the field and pack into buffer
        field->set_notnull();
        field->store(json_str, strlen(json_str), &my_charset_latin1);
        
        fprintf(stderr, "DEBUG: JSON stored in field and packed into buffer at offset %lu\n", 
                (unsigned long)field->offset(table->record[0]));
        fflush(stderr);
        
        bson_free(json_str);
      } else {
        fprintf(stderr, "DEBUG: JSON conversion failed\n");
        fflush(stderr);
        // Store error message
        field->ptr = buf + field->offset(table->record[0]);
        field->set_notnull();
        field->store("{\"error\":\"Failed to convert BSON to JSON\"}", 37, &my_charset_latin1);
      }
    }
    else
    {
      // For any other field, extract from document
      fprintf(stderr, "DEBUG: Extracting field='%s' from document\n", field_name);
      fflush(stderr);
      
      // Set field pointer to row buffer location
      field->ptr = buf + field->offset(table->record[0]);
      
      convert_simple_field_from_document(doc, field, field_name);
    }
  }
  
  DBUG_RETURN(0);
}

/*
  Convert full MongoDB document to JSON field for virtual column processing
*/
int ha_mongodb::convert_full_document_field(const bson_t *doc, Field *field, uint array_index)
{
  DBUG_ENTER("ha_mongodb::convert_full_document_field");
  
  // EXPLICIT DEBUG
  fprintf(stderr, "CONVERT_FULL_DOCUMENT_FIELD CALLED!\n");
  fflush(stderr);
  
  if (!doc || !field) {
    fprintf(stderr, "CONVERT_FULL_DOCUMENT_FIELD: NULL doc or field!\n");
    fflush(stderr);
    DBUG_RETURN(1);
  }
  
  // Debug field information
  fprintf(stderr, "Field name: %s, field_index: %u, array_index: %u, null_bit: %u\n", 
          field->field_name.str, field->field_index, array_index, field->null_bit);
  fflush(stderr);
  
  // Convert BSON to JSON string using relaxed format (more readable)
  char *json_str = bson_as_relaxed_extended_json(doc, nullptr);
  if (!json_str) {
    fprintf(stderr, "CONVERT_FULL_DOCUMENT_FIELD: relaxed JSON failed, trying canonical\n");
    fflush(stderr);
    // Fallback to canonical format
    json_str = bson_as_canonical_extended_json(doc, nullptr);
  }
  
  if (json_str) {
    fprintf(stderr, "CONVERT_FULL_DOCUMENT_FIELD: Successfully converted to JSON: %.200s...\n", json_str);
    fflush(stderr);
    
    // Clear any existing null flag and set the field
    field->set_notnull();
    
    // Store the JSON string
    int store_result = field->store(json_str, strlen(json_str), &my_charset_latin1);
    
    fprintf(stderr, "Store result: %d, field->is_null(): %d\n", store_result, field->is_null());
    fflush(stderr);
    
    bson_free(json_str);
    
    fprintf(stderr, "CONVERT_FULL_DOCUMENT_FIELD: JSON conversion complete using array_index=%u\n", array_index);
    fflush(stderr);
    
    DBUG_RETURN(0);
  }
  
  // Fallback: store error message if JSON conversion fails
  fprintf(stderr, "CONVERT_FULL_DOCUMENT_FIELD: JSON conversion failed, storing error\n");
  fflush(stderr);
  field->set_notnull();
  field->store("{\"error\":\"Failed to convert BSON to JSON\"}", 37, &my_charset_latin1);
  DBUG_RETURN(0);
}

/*
  Convert simple field directly from MongoDB document
*/
int ha_mongodb::convert_simple_field_from_document(const bson_t *doc, Field *field, const char *field_name)
{
  DBUG_ENTER("ha_mongodb::convert_simple_field_from_document");
  
  // DEBUG
  fprintf(stderr, "CONVERT_SIMPLE_FIELD CALLED for field: %s\n", field_name ? field_name : "NULL");
  fflush(stderr);
  
  bson_iter_t iter;
  
  // Try to find field in document
  if (!bson_iter_init(&iter, doc) || !bson_iter_find(&iter, field_name))
  {
    // DEBUG
    fprintf(stderr, "FIELD NOT FOUND: %s - leaving as NULL\n", field_name ? field_name : "NULL");
    fflush(stderr);
    
    // Field not found - stays NULL (already set in convert_document_to_row)
    DBUG_RETURN(0);
  }
  
  // DEBUG
  fprintf(stderr, "FIELD FOUND: %s, type=%d\n", field_name ? field_name : "NULL", bson_iter_type(&iter));
  fflush(stderr);
  
  // Extract value based on BSON type
  field->set_notnull();
  
  switch (bson_iter_type(&iter))
  {
    case BSON_TYPE_INT32:
    {
      int32_t value = bson_iter_int32(&iter);
      fprintf(stderr, "STORING INT32: %d for field %s\n", value, field_name);
      fflush(stderr);
      field->store(value);
      break;
    }
    case BSON_TYPE_INT64:
    {
      int64_t value = bson_iter_int64(&iter);
      fprintf(stderr, "STORING INT64: %lld for field %s\n", (long long)value, field_name);
      fflush(stderr);
      field->store(value);
      break;
    }
    case BSON_TYPE_DOUBLE:
    {
      double value = bson_iter_double(&iter);
      fprintf(stderr, "STORING DOUBLE: %f for field %s\n", value, field_name);
      fflush(stderr);
      field->store(value);
      break;
    }
    case BSON_TYPE_UTF8:
    {
      uint32_t len;
      const char* value = bson_iter_utf8(&iter, &len);
      fprintf(stderr, "STORING UTF8: '%.*s' for field %s\n", (int)len, value, field_name);
      fflush(stderr);
      field->store(value, len, &my_charset_latin1);
      break;
    }
    default:
    {
      // For other types, store a descriptive string
      fprintf(stderr, "STORING UNSUPPORTED TYPE: %d for field %s\n", bson_iter_type(&iter), field_name);
      fflush(stderr);
      
      char type_desc[64];
      snprintf(type_desc, sizeof(type_desc), "[BSON_TYPE_%d]", bson_iter_type(&iter));
      field->store(type_desc, strlen(type_desc), &my_charset_latin1);
      break;
    }
  }
  
  DBUG_RETURN(0);
}

/*
  Convert MongoDB _id field to SQL field
*/
int ha_mongodb::convert_mongodb_id_field(const bson_t *doc, Field *field)
{
  DBUG_ENTER("ha_mongodb::convert_mongodb_id_field");
  
  // DEBUG - prove this function is called
  fprintf(stderr, "CONVERT_ID_FIELD CALLED!\n");
  fflush(stderr);
  
  bson_iter_t iter;
  if (!bson_iter_init(&iter, doc) || !bson_iter_find(&iter, "_id"))
  {
    DBUG_RETURN(1); // No _id field found
  }
  
  field->set_notnull();
  
  if (BSON_ITER_HOLDS_OID(&iter))
  {
    // DEBUG: This should definitely show up since _id is working
    fprintf(stderr, "PROCESSING OBJECTID for _id field\n");
    fflush(stderr);
    
    // ObjectId - convert to string
    const bson_oid_t *oid = bson_iter_oid(&iter);
    char oid_str[25];
    bson_oid_to_string(oid, oid_str);
    field->store(oid_str, strlen(oid_str), system_charset_info);
  }
  else if (BSON_ITER_HOLDS_UTF8(&iter))
  {
    // String ID
    uint32_t str_len;
    const char *str_val = bson_iter_utf8(&iter, &str_len);
    field->store(str_val, str_len, system_charset_info);
  }
  else if (BSON_ITER_HOLDS_INT32(&iter))
  {
    // Integer ID
    int32_t int_val = bson_iter_int32(&iter);
    field->store(int_val);
  }
  else if (BSON_ITER_HOLDS_INT64(&iter))
  {
    // Long ID
    int64_t long_val = bson_iter_int64(&iter);
    field->store(long_val);
  }
  else
  {
    // Convert other types to string representation
    char *json_str = bson_as_canonical_extended_json(doc, nullptr);
    if (json_str)
    {
      field->store(json_str, strlen(json_str), system_charset_info);
      bson_free(json_str);
    }
  }
  
  DBUG_RETURN(0);
}

/*
  Convert BSON iterator value to MariaDB field
*/
int ha_mongodb::convert_bson_value_to_field(bson_iter_t *iter, Field *field, MongoFieldMapping *mapping)
{
  DBUG_ENTER("ha_mongodb::convert_bson_value_to_field");
  
  if (!iter || !field)
  {
    DBUG_RETURN(1);
  }
  
  field->set_notnull();
  
  // Handle based on BSON type
  switch (bson_iter_type(iter))
  {
    case BSON_TYPE_UTF8:
    {
      uint32_t str_len;
      const char *str_val = bson_iter_utf8(iter, &str_len);
      field->store(str_val, str_len, system_charset_info);
      break;
    }
    
    case BSON_TYPE_INT32:
    {
      int32_t int_val = bson_iter_int32(iter);
      field->store(int_val);
      break;
    }
    
    case BSON_TYPE_INT64:
    {
      int64_t long_val = bson_iter_int64(iter);
      field->store(long_val);
      break;
    }
    
    case BSON_TYPE_DOUBLE:
    {
      double double_val = bson_iter_double(iter);
      field->store(double_val);
      break;
    }
    
    case BSON_TYPE_BOOL:
    {
      bool bool_val = bson_iter_bool(iter);
      field->store(bool_val ? 1 : 0);
      break;
    }
    
    case BSON_TYPE_DATE_TIME:
    {
      int64_t timestamp_ms = bson_iter_date_time(iter);
      // Convert milliseconds to seconds and store as timestamp
      time_t unix_time = (time_t)(timestamp_ms / 1000);
      field->store((longlong)unix_time, true);
      break;
    }
    
    case BSON_TYPE_OID:
    {
      const bson_oid_t *oid = bson_iter_oid(iter);
      char oid_str[25];
      bson_oid_to_string(oid, oid_str);
      field->store(oid_str, strlen(oid_str), system_charset_info);
      break;
    }
    
    case BSON_TYPE_DOCUMENT:
    case BSON_TYPE_ARRAY:
    {
      // Convert nested documents/arrays to JSON string
      const uint8_t *doc_data;
      uint32_t doc_len;
      bson_iter_document(iter, &doc_len, &doc_data);
      
      bson_t *subdoc = bson_new_from_data(doc_data, doc_len);
      if (subdoc)
      {
        char *json_str = bson_as_canonical_extended_json(subdoc, nullptr);
        if (json_str)
        {
          field->store(json_str, strlen(json_str), system_charset_info);
          bson_free(json_str);
        }
        bson_destroy(subdoc);
      }
      break;
    }
    
    case BSON_TYPE_BINARY:
    {
      const uint8_t *binary_data;
      uint32_t binary_len;
      bson_subtype_t subtype;
      bson_iter_binary(iter, &subtype, &binary_len, &binary_data);
      field->store((const char*)binary_data, binary_len, &my_charset_bin);
      break;
    }
    
    case BSON_TYPE_NULL:
    {
      field->set_null();
      break;
    }
    
    default:
    {
      // For unknown types, convert to string representation
      const bson_value_t *value = bson_iter_value(iter);
      if (value)
      {
        bson_t *temp_doc = bson_new();
        bson_append_value(temp_doc, "value", -1, value);
        char *json_str = bson_as_canonical_extended_json(temp_doc, nullptr);
        if (json_str)
        {
          field->store(json_str, strlen(json_str), system_charset_info);
          bson_free(json_str);
        }
        bson_destroy(temp_doc);
      }
      break;
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
