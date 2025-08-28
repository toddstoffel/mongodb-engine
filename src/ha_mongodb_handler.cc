/* 
   MongoDB Storage Engine for MariaDB - Handler Implementation
   
   This file contains the core handler class implementation for the MongoDB storage engine.
   It provides the minimal required interface to access MongoDB collections as SQL tables.
*/

#define MYSQL_SERVER 1
#include "my_global.h"
#include "field.h"
#include "table.h"
// TODO: Fix complex header dependencies for Item class 
// Forward declaration to avoid complex SQL layer includes
class Item;
typedef Item COND;  // COND is typedef for Item in MariaDB
#include "ha_mongodb.h"
#include "mongodb_connection.h"
#include "mongodb_schema.h"
#include "mongodb_translator.h"

/* 
   Constructor - Initialize a new handler instance
*/
ha_mongodb::ha_mongodb(handlerton *hton, TABLE_SHARE *table_arg)
  : handler(hton, table_arg),
    share(nullptr),
    client(nullptr),
    collection(nullptr),
    cursor(nullptr),
    current_doc(nullptr),
    scan_position(0),
    int_table_flags(HA_CAN_TABLE_CONDITION_PUSHDOWN | HA_PRIMARY_KEY_IN_READ_INDEX | 
                   HA_FILE_BASED | HA_REC_NOT_IN_SEQ | HA_AUTO_PART_KEY | 
                   HA_CAN_INDEX_BLOBS | HA_NULL_IN_KEY | HA_STATS_RECORDS_IS_EXACT),
    pushed_condition(nullptr),
    key_read_mode(false),
    count_mode(false),
    active_index(0),
    mongo_count_result(0),
    mongo_count_returned(0),
    consecutive_rnd_next_calls(0),
    lightweight_count_mode(false),
    // PHASE 3A: Initialize performance tracking variables
    documents_scanned(0),
    optimized_count_operations(0),
    count_performance_tracking(false)
{
  fprintf(stderr, "ha_mongodb::ha_mongodb() CONSTRUCTOR called, int_table_flags=0x%llx\n", int_table_flags);}

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
  
  fprintf(stderr, "OPEN CALLED! name=%s, mode=%d, int_table_flags=0x%llx\n", 
          name ? name : "NULL", mode, int_table_flags);
  
  // Get or create the shared table metadata
  if (!(share = get_share()))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  
  // Parse connection string if not already done
  if (!share->parsed)
  {
    fprintf(stderr, "OPEN: Parsing connection string: %s\n", table->s->connect_string.str);
    
    if (mongodb_parse_connection_string(table->s->connect_string.str, share))
    {
      fprintf(stderr, "OPEN: Connection string parsing failed\n");
      free_share();
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
    }
    share->parsed = true;
    
    fprintf(stderr, "OPEN: Connection string parsed successfully\n");
  }
  
  // Set ref_length for position-based access (match MariaDB expectation = 8 bytes)
  ref_length = 8;
  
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
   Table scan initialization with MongoDB-level ORDER BY support
*/
int ha_mongodb::rnd_init(bool scan)
{
  DBUG_ENTER("ha_mongodb::rnd_init");
  
  // Reset row counter for new scan
  static ha_rows *row_counter_ptr = nullptr;
  if (!row_counter_ptr) {
    static ha_rows global_row_counter = 0;
    row_counter_ptr = &global_row_counter;
  }
  *row_counter_ptr = 0;
  
  fprintf(stderr, "RND_INIT CALLED! scan=%d, table=%p, reset row counter\n", scan, table);
  
  // CRITICAL: Reset optimization state for each new scan to prevent persistence
  lightweight_count_mode = false;
  consecutive_rnd_next_calls = 0;
  fprintf(stderr, "RND_INIT: Reset lightweight optimization state\n");
  
  // CRITICAL: Reset count_mode at start of each scan
  // Operation 46 is called for many non-COUNT queries, so we can't rely on it
  if (scan) {  // Reset for table scans
    fprintf(stderr, "RND_INIT: Resetting count_mode for new table scan\n");
    count_mode = false;
    mongo_count_result = 0;
    mongo_count_returned = 0;
    
    // CRITICAL: Also reset any persistent pushed_condition that might be stuck
    // This prevents previous WHERE clauses from affecting new queries
    if (pushed_condition) {
      fprintf(stderr, "RND_INIT: WARNING - Found persistent pushed_condition, cleaning up\n");
      char* old_filter = bson_as_canonical_extended_json(pushed_condition, nullptr);
      fprintf(stderr, "RND_INIT: Removing stuck filter: %s\n", old_filter);
      bson_free(old_filter);
      bson_destroy(pushed_condition);
      pushed_condition = nullptr;
    }
  }
  
  fprintf(stderr, "RND_INIT: count_mode=%d, key_read_mode=%d\n", count_mode, key_read_mode);
  fprintf(stderr, "RND_INIT: pushed_condition=%p\n", (void*)pushed_condition);
  fflush(stderr);  // Force immediate output
  
  // Simple test - just try to connect to MongoDB
  if (!collection)
  {
    int connect_result = connect_to_mongodb();
    fprintf(stderr, "RND_INIT: connect_to_mongodb() returned: %d\n", connect_result);
    if (connect_result)
    {
      fprintf(stderr, "RND_INIT: Connection failed\n");
      // Error already reported by connect_to_mongodb() or stash_remote_error()
      DBUG_RETURN(connect_result);
    }
  }
  
  // Enhanced COUNT detection: Detect COUNT operations beyond simple count_mode flag
  // This catches COUNT with WHERE clauses that use key_read_mode
  bool enhanced_count_detection = false;
  
  // PHASE 3A: Enhanced COUNT Detection - Look for patterns that suggest COUNT operations
  // Since COUNT with WHERE uses table scanning, optimize the scanning process
  if (scan) {
    // Enable performance tracking for all scans - might be COUNT operations
    count_performance_tracking = true;
    count_start_time = std::chrono::steady_clock::now();
    documents_scanned = 0;
    
    fprintf(stderr, "RND_INIT: Enabling scan optimization for potential COUNT operation\n");
  }
  
  // COUNT MODE: Use MongoDB native count instead of fetching documents
  if (count_mode) {  // Remove key_read_mode requirement - count_mode alone is enough
    fprintf(stderr, "RND_INIT: COUNT MODE DETECTED - using MongoDB native count optimization\n");
    fprintf(stderr, "RND_INIT: count_mode=%d, key_read_mode=%d\n", count_mode, key_read_mode);
    fprintf(stderr, "RND_INIT: collection=%p, database=%s, collection_name=%s\n", 
            collection, share ? share->database_name : "NULL", share ? share->collection_name : "NULL");
    
    bson_t *query = pushed_condition ? bson_copy(pushed_condition) : bson_new();
    
    // Debug: show the exact query being sent to MongoDB
    char *query_str = bson_as_canonical_extended_json(query, nullptr);
    fprintf(stderr, "RND_INIT: MongoDB count query: %s\n", query_str);
    bson_free(query_str);
    
    bson_error_t error;
    
    int64_t count = mongoc_collection_count_documents(collection, query, nullptr, nullptr, nullptr, &error);
    bson_destroy(query);
    
    if (count < 0) {
      fprintf(stderr, "RND_INIT: MongoDB count error: %s\n", error.message);
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
    }
    
    // Store count for rnd_next() optimization
    mongo_count_result = (ha_rows)count;
    mongo_count_returned = 0;
    fprintf(stderr, "RND_INIT: MongoDB native count returned: %lld documents (stored as mongo_count_result=%llu)\n", 
            (long long)count, (unsigned long long)mongo_count_result);
    
    // Don't create cursor for count operations
    DBUG_RETURN(0);
  }
  
  // Reset the static row counter for new scan
  static ha_rows current_scan_counter = 0;
  current_scan_counter = 0;  // Reset for each new scan
  
  // Reset scan position for position-based access
  scan_position = 0;
  
  // SOLUTION: MongoDB ORDER BY Pushdown
  // Instead of letting MariaDB handle ORDER BY through external sorting,
  // implement MongoDB-level sorting which is more efficient
  
  // For now, detect ORDER BY by checking if we're likely in a sort context
  // This is a heuristic approach - in production we'd use proper condition pushdown
  bool use_mongodb_sort = false;
  
  // Simple heuristic: if we've had external sorting issues recently,
  // try MongoDB-level sorting instead
  // TODO: Implement proper ORDER BY detection via condition pushdown
  use_mongodb_sort = false; // Temporarily disable MongoDB sorting to test
  
  if (use_mongodb_sort) {
    fprintf(stderr, "RND_INIT: Using MongoDB-level sorting for better ORDER BY performance\n");
    
    // TODO: Implement proper ORDER BY detection and field mapping via condition pushdown
    // This will require parsing the SQL ORDER BY clause and mapping SQL field names
    // to MongoDB field names dynamically, without any hardcoded values
    
    fprintf(stderr, "RND_INIT: MongoDB sorting not yet implemented - falling back to MariaDB sorting\n");
    
    // For now, fall back to simple cursor
    bson_t *query = pushed_condition ? bson_copy(pushed_condition) : bson_new();
    cursor = mongoc_collection_find_with_opts(collection, query, nullptr, nullptr);
    bson_destroy(query);
    fprintf(stderr, "RND_INIT: Created cursor with condition filter\n");
  } else {
    // PHASE 3A: MongoDB Cursor Optimization for COUNT Operations
    // CRITICAL COUNT OPTIMIZATION: If we have a pushed condition, this could be COUNT with WHERE
    // Since MariaDB chooses table scanning for COUNT with WHERE, intercept and optimize
    
    bson_t *query = pushed_condition ? bson_copy(pushed_condition) : bson_new();
    
    // INTELLIGENT COUNT DETECTION: For scans with WHERE conditions, try COUNT first
    if (pushed_condition && scan) {
      fprintf(stderr, "RND_INIT: SCAN + WHERE condition detected - attempting COUNT optimization\n");
      
      // Try MongoDB native count with the condition
      bson_error_t count_error;
      int64_t condition_count = mongoc_collection_count_documents(
          collection, pushed_condition, nullptr, nullptr, nullptr, &count_error);
      
      if (condition_count >= 0) {
        fprintf(stderr, "RND_INIT: SUCCESS! MongoDB COUNT with WHERE: %lld documents\n", (long long)condition_count);
        
        // Store count result for potential use
        mongo_count_result = (ha_rows)condition_count;
        mongo_count_returned = 0;
        
        // STRATEGY: Instead of full cursor, create minimal cursor for count verification
        // This allows MariaDB's scanning loop to work but with minimal data transfer
        
        // Use projection to fetch only _id field for minimal data transfer
        bson_t *opts = bson_new();
        bson_t *projection = bson_new();
        bson_append_int32(projection, "_id", 3, 1);  // Only fetch _id field
        bson_append_document(opts, "projection", 10, projection);
        bson_append_int32(opts, "batchSize", 9, 100);  // Small batches for counting
        
        cursor = mongoc_collection_find_with_opts(collection, query, opts, nullptr);
        
        bson_destroy(projection);
        bson_destroy(opts);
        
        fprintf(stderr, "RND_INIT: Created COUNT-optimized cursor with minimal projection\n");
      } else {
        fprintf(stderr, "RND_INIT: MongoDB count failed: %s, using normal cursor\n", count_error.message);
        
        // Fall back to normal cursor
        bson_t *opts = bson_new();
        bson_append_int32(opts, "batchSize", 9, 1000);
        bson_append_bool(opts, "noCursorTimeout", 15, true);
        
        cursor = mongoc_collection_find_with_opts(collection, query, opts, nullptr);
        bson_destroy(opts);
      }
    } else {
      // Normal scan without WHERE condition
      bson_t *opts = bson_new();
      bson_append_int32(opts, "batchSize", 9, 1000);
      bson_append_bool(opts, "noCursorTimeout", 15, true);
      
      cursor = mongoc_collection_find_with_opts(collection, query, opts, nullptr);
      bson_destroy(opts);
      fprintf(stderr, "RND_INIT: Created normal cursor\n");
    }
    
    bson_destroy(query);  // Clean up query in all cases
  }
  
  if (!cursor)
  {
    fprintf(stderr, "RND_INIT: Failed to create cursor\n");
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  }
  
  current_doc = nullptr;
  fprintf(stderr, "RND_INIT: Successfully created sorted cursor\n");
  DBUG_RETURN(0);
}

/*
   Fetch next row in table scan
*/
int ha_mongodb::rnd_next(uchar *buf)
{
  DBUG_ENTER("ha_mongodb::rnd_next");
  
  fprintf(stderr, "RND_NEXT CALLED! count_mode=%d, key_read_mode=%d\n", count_mode, key_read_mode);
  fflush(stderr);
  
  // COUNT MODE: Return count result without fetching documents
  if (count_mode) {  // Remove key_read_mode requirement
    fprintf(stderr, "RND_NEXT: COUNT MODE - storage engine COUNT optimization\n");
    fprintf(stderr, "RND_NEXT: count_mode=%d, key_read_mode=%d, mongo_count_result=%lld\n", 
            count_mode, key_read_mode, (long long)mongo_count_result);
    
    // For COUNT operations, immediately signal end of file
    // The actual count was already provided through other means
    fprintf(stderr, "RND_NEXT: COUNT MODE - immediately returning HA_ERR_END_OF_FILE\n");
    DBUG_RETURN(HA_ERR_END_OF_FILE);
  }
  
  // PHASE 3A: Enhanced COUNT Performance - Intelligent COUNT Detection
  // Track consecutive rnd_next() calls to identify potential COUNT operations
  
  // Increment call counter and document tracking
  consecutive_rnd_next_calls++;
  if (count_performance_tracking) {
    documents_scanned++;
  }
  
  // CONSERVATIVE COUNT DETECTION: Enable lightweight mode for high-frequency scanning patterns
  // TEMPORARILY DISABLED: This optimization was interfering with normal SELECT queries
  // The threshold-based detection is too aggressive and incorrectly identifies SELECT as COUNT
  if (false && consecutive_rnd_next_calls > 8 && !lightweight_count_mode) {
    // Additional validation: check if we're processing many rows (typical of COUNT)
    // and haven't seen any data access patterns typical of SELECT queries
    
    fprintf(stderr, "RND_NEXT: HIGH-FREQUENCY PATTERN DETECTED (%d calls) - likely COUNT operation\n", 
            consecutive_rnd_next_calls);
    fprintf(stderr, "RND_NEXT: Enabling lightweight document processing optimization\n");
    lightweight_count_mode = true;
    optimized_count_operations++;
    
    // From this point forward, minimize document processing overhead
  }
  
  // Normal document fetching mode
  if (!cursor)
  {
    DBUG_RETURN(HA_ERR_END_OF_FILE);
  }
  
  if (!mongoc_cursor_next(cursor, &current_doc))
  {
    // Check for errors
    bson_error_t error;
    if (mongoc_cursor_error(cursor, &error))
    {
      fprintf(stderr, "RND_NEXT: Cursor error: %s\n", error.message);
      
      // Report MongoDB connection errors via fprintf and return proper error codes
      // Note: Using fprintf instead of my_error() to avoid complex service dependencies
      if (strstr(error.message, "connection refused") || 
          strstr(error.message, "No suitable servers found")) {
        fprintf(stderr, "MONGODB ERROR: Connection failed - %s\n", error.message);
        DBUG_RETURN(HA_ERR_NO_CONNECTION);
      } else if (strstr(error.message, "Authentication failed") ||
                 strstr(error.message, "not authorized")) {
        fprintf(stderr, "MONGODB ERROR: Authentication failed - %s\n", error.message);
        DBUG_RETURN(HA_ERR_NO_CONNECTION);
      } else if (strstr(error.message, "Collection") && strstr(error.message, "not found")) {
        fprintf(stderr, "MONGODB ERROR: Collection not found - %s\n", error.message);
        DBUG_RETURN(HA_ERR_NO_SUCH_TABLE);
      } else {
        // Generic MongoDB error
        fprintf(stderr, "MONGODB ERROR: %s (code: %d)\n", error.message, error.code);
        DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
      }
    }
    
    // No more documents
    DBUG_RETURN(HA_ERR_END_OF_FILE);
  }
  
  // Convert MongoDB document to MariaDB row
  if (!current_doc)
  {
    // Document is NULL - this shouldn't happen
    fprintf(stderr, "RND_NEXT: Current document is NULL!\n");
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  }
  
  // CRITICAL: Set the record buffer to the table's record format
  memset(buf, 0, table->s->reclength);
  
  // PHASE 3A: LIGHTWEIGHT COUNT OPTIMIZATION - Minimal processing for COUNT operations
  if (lightweight_count_mode) {
    fprintf(stderr, "RND_NEXT: LIGHTWEIGHT MODE - minimal document processing (scan_position=%llu)\n", 
            (unsigned long long)scan_position);
    
    // For COUNT operations, we just need to indicate we have a row
    // Skip expensive document-to-row conversion and just return success
    // This allows MariaDB to count the rows without full data processing
    
    // PHASE 3A: The main optimization is skipping document-to-row conversion
    // This provides significant performance benefits for COUNT operations
    
    // Minimal row setup - just ensure the buffer is valid for MariaDB
    memset(buf, 0, table->s->reclength);
    
    // Advance position tracking
    scan_position++;
    
    // Reset consecutive calls counter since we're processing successfully
    consecutive_rnd_next_calls = 0;
    
    fprintf(stderr, "RND_NEXT: LIGHTWEIGHT - skipping document conversion, returning success\n");
    DBUG_RETURN(0);
  }
  
  // Convert and pack fields into the record buffer
  fprintf(stderr, "RND_NEXT: Converting document to row...\n");
  
  if (convert_document_to_row(current_doc, buf))
  {
    fprintf(stderr, "RND_NEXT: Document conversion failed!\n");
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  }
  
  // Increment scan position for position tracking
  scan_position++;

  DBUG_RETURN(0);
}

/*
   End table scan
*/
int ha_mongodb::rnd_end()
{
  DBUG_ENTER("ha_mongodb::rnd_end");
  
  fprintf(stderr, "RND_END CALLED! Cleaning up count_mode=%d\n", count_mode);
  
  // PHASE 3A: Performance Reporting
  if (count_performance_tracking) {
    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - count_start_time);
    
    fprintf(stderr, "RND_END: PERFORMANCE REPORT - Operation completed in %lld ms\n", 
            (long long)duration.count());
    fprintf(stderr, "RND_END: Documents scanned: %llu, Consecutive calls: %d\n", 
            (unsigned long long)documents_scanned, consecutive_rnd_next_calls);
    fprintf(stderr, "RND_END: Lightweight mode: %s, Optimized operations: %llu\n", 
            lightweight_count_mode ? "ENABLED" : "DISABLED",
            (unsigned long long)optimized_count_operations);
    
    if (lightweight_count_mode) {
      fprintf(stderr, "RND_END: COUNT OPTIMIZATION ACHIEVED - reduced document processing overhead\n");
    }
    
    count_performance_tracking = false;
  }
  
  // Reset lightweight count optimization state
  if (lightweight_count_mode || consecutive_rnd_next_calls > 0) {
    fprintf(stderr, "RND_END: Resetting lightweight count state (calls=%d, mode=%s)\n", 
            consecutive_rnd_next_calls, lightweight_count_mode ? "true" : "false");
  }
  consecutive_rnd_next_calls = 0;
  lightweight_count_mode = false;
  
  // Clean up cursor
  if (cursor)
  {
    mongoc_cursor_destroy(cursor);
    cursor = nullptr;
  }
  current_doc = nullptr;
  
  // Reset count mode state
  count_mode = false;
  mongo_count_result = 0;
  mongo_count_returned = 0;
  
  DBUG_RETURN(0);
}

/*
   Get table information - Enhanced implementation for Phase 2
*/
int ha_mongodb::info(uint flag)
{
  DBUG_ENTER("ha_mongodb::info");
  
  fprintf(stderr, "INFO() CALLED with flag: %u - this might be used for COUNT optimization!\n", flag);
  fprintf(stderr, "INFO: count_mode=%d, key_read_mode=%d\n", count_mode, key_read_mode);
  fflush(stderr);
  
  // Initialize stats to safe defaults
  stats.records = 0;
  stats.mean_rec_length = 512; // Reasonable default for document size
  stats.data_file_length = 0;
  stats.index_file_length = 0;
  stats.max_data_file_length = 0;
  stats.delete_length = 0;
  stats.auto_increment_value = 0;
  
  // Only try to get real statistics if we have valid connection and collection
  // During ALTER operations, these might be null, so we need to be defensive
  if (client && collection && share && share->connection_string)
  {
    fprintf(stderr, "INFO: Getting MongoDB document count for statistics (pushed_condition=%p)\n", (void*)pushed_condition);
    
    // Get document count from MongoDB - use pushed condition if available for COUNT with WHERE
    bson_error_t error;
    bson_t *filter;
    
    if (pushed_condition) {
      // Use the pushed condition for COUNT with WHERE optimization
      filter = bson_copy(pushed_condition);
      fprintf(stderr, "INFO: Using pushed condition for COUNT with WHERE optimization\n");
    } else {
      // Empty filter for simple COUNT(*)
      filter = bson_new(); 
      fprintf(stderr, "INFO: Using empty filter for simple COUNT(*)\n");
    }
    
    int64_t doc_count = mongoc_collection_count_documents(
      collection,
      filter,     // Empty filter
      NULL,       // No options
      NULL,       // No read prefs
      NULL,       // No reply
      &error
    );
    
    bson_destroy(filter);
    
    if (doc_count >= 0)
    {
      stats.records = (ha_rows)doc_count;
      stats.data_file_length = stats.records * stats.mean_rec_length;
      fprintf(stderr, "INFO: Successfully got MongoDB count: %lld documents with %s\n", 
              (long long)doc_count, pushed_condition ? "WHERE condition" : "no condition");
      fprintf(stderr, "INFO: Set stats.records = %llu for COUNT optimization\n", (unsigned long long)stats.records);
      
      // CRITICAL: For COUNT(*) operations, MariaDB may use stats.records directly
      // This enables COUNT pushdown for both simple COUNT(*) and COUNT with WHERE
      if (count_mode || pushed_condition) {
        fprintf(stderr, "INFO: COUNT MODE or WHERE condition - MariaDB should use this count directly!\n");
      }
    }
    else
    {
      fprintf(stderr, "INFO: Failed to get MongoDB count: %s\n", error.message);
      // Failed to get document count, keep defaults
    }
  }
  else
  {
    // No valid connection available during this operation (e.g., ALTER TABLE)
    // This is normal - just return safe defaults
  }
  
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
   Position cursor - store current document's _id for later retrieval
*/
void ha_mongodb::position(const uchar *record)
{
  DBUG_ENTER("ha_mongodb::position");
  
  fprintf(stderr, "POSITION CALLED! record=%p, ref_length=%u\n", record, ref_length);
  
  // Use CONNECT engine approach: store simple record position/offset
  // Store the CURRENT position (scan_position is already incremented by rnd_next)
  my_off_t current_position = (my_off_t)(scan_position - 1); // Position of the record we just read
  
  // Store position using MariaDB's standard method (like CONNECT engine)
  my_store_ptr(ref, ref_length, current_position);
  
  fprintf(stderr, "POSITION: Stored record position %llu using my_store_ptr (ref_length=%u)\n", 
          (unsigned long long)current_position, ref_length);
  
  DBUG_VOID_RETURN;
}

/*
   Random position read - read document by _id stored in position
*/
int ha_mongodb::rnd_pos(uchar *buf, uchar *pos)
{
  DBUG_ENTER("ha_mongodb::rnd_pos");
  
  fprintf(stderr, "RND_POS CALLED! buf=%p, pos=%p, ref_length=%u\n", buf, pos, ref_length);
  
  if (!pos || !collection) {
    fprintf(stderr, "RND_POS: No position or collection\n");
    DBUG_RETURN(HA_ERR_WRONG_COMMAND);
  }
  
  // Extract position using MariaDB standard my_get_ptr (CONNECT engine pattern)
  my_off_t target_position = my_get_ptr(pos, ref_length);
  
  fprintf(stderr, "RND_POS: Seeking to position %llu\n", (unsigned long long)target_position);
  
  // Reset cursor to beginning and seek to target position
  if (!cursor) {
    fprintf(stderr, "RND_POS: No active cursor - reinitializing\n");
    
    // Reinitialize scan if no cursor exists
    int rc = rnd_init(true);
    if (rc != 0) {
      fprintf(stderr, "RND_POS: Failed to reinitialize scan\n");
      DBUG_RETURN(rc);
    }
  }
  
  // If we need to seek to a different position, restart from beginning with sorting
  if (scan_position > target_position) {
    fprintf(stderr, "RND_POS: Rewinding cursor (current=%llu, target=%llu)\n", 
            (unsigned long long)scan_position, (unsigned long long)target_position);
    
    // Clean up current cursor
    if (cursor) {
      mongoc_cursor_destroy(cursor);
      cursor = nullptr;
    }
    
    // Create new cursor WITHOUT any sorting (let MariaDB handle ORDER BY)
    bson_t *query = bson_new();  // Empty query = scan all
    cursor = mongoc_collection_find_with_opts(collection, query, nullptr, nullptr);
    bson_destroy(query);
    
    if (!cursor) {
      fprintf(stderr, "RND_POS: Failed to create cursor for rewind\n");
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
    }
    
    scan_position = 0;  // Reset position counter
    fprintf(stderr, "RND_POS: Created new cursor for position access\n");
  }
  
  // Skip to the target position
  while (scan_position < target_position) {
    const bson_t *doc = nullptr;
    if (!mongoc_cursor_next(cursor, &doc)) {
      fprintf(stderr, "RND_POS: Could not seek to position %llu (stopped at %llu)\n", 
              (unsigned long long)target_position, (unsigned long long)scan_position);
      DBUG_RETURN(HA_ERR_KEY_NOT_FOUND);
    }
    scan_position++;
  }
  
  // Now call rnd_next to get the current document
  int rc = rnd_next(buf);
  if (rc == 0) {
    fprintf(stderr, "RND_POS: Successfully retrieved document at position %llu\n", 
            (unsigned long long)target_position);
  } else {
    fprintf(stderr, "RND_POS: Failed to retrieve document at position %llu (error=%d)\n", 
            (unsigned long long)target_position, rc);
  }
  
  DBUG_RETURN(rc);
}

/*
   Index operations - stub implementations
*/
int ha_mongodb::index_init(uint keynr, bool sorted)
{
  DBUG_ENTER("ha_mongodb::index_init");
  
  fprintf(stderr, "INDEX_INIT CALLED! keynr=%u, sorted=%d (FederatedX pattern)\n", keynr, sorted);
  
  // Follow FederatedX pattern: just set active index and return success
  // The actual cursor initialization will happen in index_read_map
  active_index = keynr;
  
  fprintf(stderr, "INDEX_INIT: Set active_index=%u, returning success\n", active_index);
  DBUG_RETURN(0);
}

int ha_mongodb::index_read_map(uchar *buf, const uchar *key, key_part_map keypart_map, enum ha_rkey_function find_flag)
{
  DBUG_ENTER("ha_mongodb::index_read_map");
  
  fprintf(stderr, "INDEX_READ_MAP ENTRY: buf=%p, key=%p, keypart_map=%u, find_flag=%d\n", 
          (void*)buf, (void*)key, (uint)keypart_map, (int)find_flag);
  
  // Initialize connection if needed (following FederatedX pattern)
  if (!collection)
  {
    if (connect_to_mongodb())
    {
      fprintf(stderr, "INDEX_READ_MAP: Connection failed\n");
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
    }
  }
  
  // Initialize cursor if needed (following FederatedX pattern)
  if (!cursor)
  {
    fprintf(stderr, "INDEX_READ_MAP: Initializing cursor for index operations\n");
    bson_t *query = pushed_condition ? bson_copy(pushed_condition) : bson_new();
    cursor = mongoc_collection_find_with_opts(collection, query, nullptr, nullptr);
    bson_destroy(query);
    
    if (!cursor)
    {
      fprintf(stderr, "INDEX_READ_MAP: Failed to create cursor\n");
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
    }
    current_doc = nullptr;
    scan_position = 0;
  }
  
  fprintf(stderr, "INDEX_READ_MAP: Proceeding with read operation\n");
  
  // Use the same logic as rnd_next to get the first document
  if (!mongoc_cursor_next(cursor, &current_doc))
  {
    bson_error_t error;
    if (mongoc_cursor_error(cursor, &error))
    {
      fprintf(stderr, "INDEX_READ_MAP: Cursor error: %s\n", error.message);
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
    }
    
    fprintf(stderr, "INDEX_READ_MAP: No documents found\n");
    DBUG_RETURN(HA_ERR_END_OF_FILE);
  }
  
  // Check if we're in key-only mode (for COUNT operations)
  if (key_read_mode)
  {
    fprintf(stderr, "INDEX_READ_MAP: Key-only mode - COUNT optimization\n");
    // For key-only reads (COUNT), we just need to indicate we have a row
    // MariaDB will count the successful returns without needing full data
    memset(buf, 0, table->s->reclength);
    // For PRIMARY KEY (_id), just set the key field if it exists
    // This is sufficient for COUNT operations
  }
  else
  {
    fprintf(stderr, "INDEX_READ_MAP: Full row mode - converting document\n");
    // Convert document to row for full reads
    memset(buf, 0, table->s->reclength);
    if (convert_document_to_row(current_doc, buf))
    {
      fprintf(stderr, "INDEX_READ_MAP: Document conversion failed\n");
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
    }
  }
  
  fprintf(stderr, "INDEX_READ_MAP: Successfully returned first row\n");
  DBUG_RETURN(0);
}

int ha_mongodb::index_read(uchar *buf, const uchar *key, uint key_len, enum ha_rkey_function find_flag)
{
  DBUG_ENTER("ha_mongodb::index_read");
  
  fprintf(stderr, "INDEX_READ CALLED! key_len=%u, find_flag=%d (FederatedX compatibility)\n", key_len, (int)find_flag);
  
  // Convert key_len to key_part_map for index_read_map compatibility
  key_part_map keypart_map = (1UL << key_len) - 1;  // Simple conversion
  
  // Call our main index_read_map implementation
  int result = index_read_map(buf, key, keypart_map, find_flag);
  
  fprintf(stderr, "INDEX_READ: Delegated to index_read_map, result=%d\n", result);
  DBUG_RETURN(result);
}

int ha_mongodb::index_next(uchar *buf)
{
  DBUG_ENTER("ha_mongodb::index_next");
  
  fprintf(stderr, "INDEX_NEXT CALLED (key_read_mode=%d)\n", key_read_mode);
  
  if (!cursor)
  {
    fprintf(stderr, "INDEX_NEXT: No cursor - returning END_OF_FILE\n");
    DBUG_RETURN(HA_ERR_END_OF_FILE);
  }
  
  // Continue iterating through the sorted cursor
  if (!mongoc_cursor_next(cursor, &current_doc))
  {
    bson_error_t error;
    if (mongoc_cursor_error(cursor, &error))
    {
      fprintf(stderr, "INDEX_NEXT: Cursor error: %s\n", error.message);
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
    }
    
    fprintf(stderr, "INDEX_NEXT: End of cursor reached\n");
    DBUG_RETURN(HA_ERR_END_OF_FILE);
  }
  
  // Check if we're in key-only mode (for COUNT operations)
  if (key_read_mode)
  {
    fprintf(stderr, "INDEX_NEXT: Key-only mode - COUNT optimization\n");
    // For key-only reads (COUNT), we just need to indicate we have a row
    memset(buf, 0, table->s->reclength);
  }
  else
  {
    fprintf(stderr, "INDEX_NEXT: Full row mode - converting document\n");
    // Convert document to row for full reads
    memset(buf, 0, table->s->reclength);
    if (convert_document_to_row(current_doc, buf))
    {
      fprintf(stderr, "INDEX_NEXT: Document conversion failed\n");
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
    }
  }
  
  DBUG_RETURN(0);
}

int ha_mongodb::index_end()
{
  DBUG_ENTER("ha_mongodb::index_end");
  
  fprintf(stderr, "INDEX_END CALLED - cleaning up cursor\n");
  
  // Clean up cursor (same as rnd_end)
  if (cursor)
  {
    mongoc_cursor_destroy(cursor);
    cursor = nullptr;
  }
  current_doc = nullptr;
  
  fprintf(stderr, "INDEX_END: Cursor cleaned up\n");
  DBUG_RETURN(0);
}

/*
   Data modification operations - stub implementations
*/
// Range operations - required for COUNT(*) with PRIMARY KEY
int ha_mongodb::read_range_first(const key_range *start_key, const key_range *end_key,
                                bool eq_range, bool sorted)
{
  fprintf(stderr, "READ_RANGE_FIRST CALLED! eq_range=%d, sorted=%d\n", eq_range, sorted);
  
  // For MongoDB, we don't have actual ranges like SQL databases
  // We'll just initialize a cursor for the entire collection
  // and let index_next() handle the iteration
  
  if (cursor) {
    mongoc_cursor_destroy(cursor);
    cursor = nullptr;
  }
  
  // Initialize cursor for entire collection (MongoDB doesn't have traditional ranges)
  bson_t *query = bson_new();
  
  if (key_read_mode) {
    // For COUNT(*) operations, we only need to count documents
    fprintf(stderr, "READ_RANGE_FIRST: key_read_mode enabled, optimizing for COUNT\n");
  }
  
  cursor = mongoc_collection_find_with_opts(collection, query, nullptr, nullptr);
  
  bson_destroy(query);
  
  if (!cursor) {
    fprintf(stderr, "READ_RANGE_FIRST: Failed to create cursor\n");
    return HA_ERR_INTERNAL_ERROR;
  }
  
  fprintf(stderr, "READ_RANGE_FIRST: Success, cursor initialized\n");
  return 0;
}

int ha_mongodb::read_range_next()
{
  fprintf(stderr, "READ_RANGE_NEXT CALLED!\n");
  
  if (!cursor) {
    fprintf(stderr, "READ_RANGE_NEXT: No cursor available\n");
    return HA_ERR_END_OF_FILE;
  }
  
  // Get next document from cursor
  if (!mongoc_cursor_next(cursor, &current_doc)) {
    bson_error_t error;
    if (mongoc_cursor_error(cursor, &error)) {
      fprintf(stderr, "READ_RANGE_NEXT: Cursor error: %s\n", error.message);
      return HA_ERR_INTERNAL_ERROR;
    }
    fprintf(stderr, "READ_RANGE_NEXT: End of results\n");
    return HA_ERR_END_OF_FILE;
  }
  
  // For key_read_mode (COUNT operations), we don't need to fill the buffer
  if (key_read_mode) {
    fprintf(stderr, "READ_RANGE_NEXT: key_read_mode - counting document\n");
    return 0;
  }
  
  fprintf(stderr, "READ_RANGE_NEXT: Got document, would convert to row\n");
  // Note: We don't have the buf parameter here, so we'll handle this in the actual read methods
  return 0;
}

// Record counting - MongoDB native count pushdown
ha_rows ha_mongodb::records()
{
  fprintf(stderr, "*** RECORDS() CALLED - implementing MongoDB native count pushdown ***\n");
  fflush(stderr);  // Force immediate output
  
  if (!collection) {
    fprintf(stderr, "RECORDS: No collection available\n");
    return 0;
  }
  
  bson_error_t error;
  bson_t *query = nullptr;
  
  // Use pushed condition if available (for COUNT with WHERE clause)
  if (pushed_condition) {
    query = bson_copy(pushed_condition);  // Use already converted BSON filter
    fprintf(stderr, "RECORDS: Using pushed condition for COUNT\n");
  } else {
    query = bson_new();  // Empty query for COUNT(*)
    fprintf(stderr, "RECORDS: Using empty query for COUNT(*)\n");
  }
  
  // Use MongoDB's native count operation
  int64_t count = mongoc_collection_count_documents(collection, query, nullptr, nullptr, nullptr, &error);
  
  bson_destroy(query);
  
  if (count < 0) {
    fprintf(stderr, "RECORDS: MongoDB count error: %s\n", error.message);
    return 0;
  }
  
  fprintf(stderr, "RECORDS: MongoDB native count returned: %lld documents\n", (long long)count);
  return (ha_rows)count;
}
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
   Statistics and metadata - implement reasonable defaults for optimizer
*/
ha_rows ha_mongodb::estimate_rows_upper_bound()
{
  DBUG_ENTER("ha_mongodb::estimate_rows_upper_bound");
  // Return a reasonable estimate instead of error
  // Use the stats.records if available, otherwise default to 1000
  if (stats.records > 0) {
    DBUG_RETURN(stats.records);
  }
  DBUG_RETURN(1000); // Reasonable default for MongoDB collections
}

IO_AND_CPU_COST ha_mongodb::scan_time()
{
  DBUG_ENTER("ha_mongodb::scan_time");
  IO_AND_CPU_COST cost;
  
  // Provide reasonable cost estimates for MongoDB table scans
  // Base cost on estimated record count
  ha_rows records = (stats.records > 0) ? stats.records : 1000;
  
  cost.io = (double)records * 0.1;   // Assume 0.1 IO cost per record
  cost.cpu = (double)records * 0.05; // Assume 0.05 CPU cost per record
  
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
  
  // Cost for random position access (by _id)
  cost.io = (double)rows * 0.2;   // Higher cost for random access
  cost.cpu = (double)rows * 0.1;  // CPU cost for position lookups
  
  DBUG_RETURN(cost);
}

ha_rows ha_mongodb::records_in_range(uint inx, const key_range *min_key, const key_range *max_key, page_range *pages)
{
  DBUG_ENTER("ha_mongodb::records_in_range");
  
  fprintf(stderr, "RECORDS_IN_RANGE CALLED for index %u (FederatedX pattern)\n", inx);
  
  // Follow FederatedX pattern: return a small constant to encourage index usage
  // FederatedX comment: "We really want indexes to be used as often as possible, 
  // therefore we just need to hard-code the return value to a very low number to force the issue"
  ha_rows result = MONGODB_RECORDS_IN_RANGE;
  
  fprintf(stderr, "RECORDS_IN_RANGE: Returning %llu (encourages index usage)\n", (unsigned long long)result);
  DBUG_RETURN(result);
}

/*
   Condition pushdown - stub implementations
*/
const COND *ha_mongodb::cond_push(const COND *cond)
{
  DBUG_ENTER("ha_mongodb::cond_push");
  
  if (!cond) {
    fprintf(stderr, "COND_PUSH: No condition received\n");
    DBUG_RETURN(nullptr);
  }

  fprintf(stderr, "COND_PUSH: Received condition (pointer: %p)\n", (void*)cond);

  // Create BSON document for MongoDB filter
  bson_t *match_filter = bson_new();
  if (!match_filter) {
    fprintf(stderr, "COND_PUSH: Failed to create BSON document\n");
    DBUG_RETURN(cond);
  }

  // Translate the condition to MongoDB BSON filter
  if (mongodb_translator::translate_condition_to_bson(cond, match_filter)) {
    // Translation successful - store the filter for use in rnd_init/index_init
    if (pushed_condition) {
      bson_destroy(pushed_condition);
    }
    pushed_condition = match_filter;
    
    if (pushed_condition) {
      char *filter_str = bson_as_canonical_extended_json(pushed_condition, nullptr);
      if (filter_str) {
        fprintf(stderr, "COND_PUSH: Successfully translated condition to MongoDB filter: %s\n", filter_str);
        bson_free(filter_str);
      }
    }
    
    // Return nullptr to indicate we can handle this condition
    DBUG_RETURN(nullptr);
  } else {
    // Translation failed - cleanup and let MariaDB handle filtering
    bson_destroy(match_filter);
    fprintf(stderr, "COND_PUSH: Translation failed - returning condition for MariaDB filtering\n");
    DBUG_RETURN(cond);
  }
}

void ha_mongodb::cond_pop()
{
  DBUG_ENTER("ha_mongodb::cond_pop");
  
  // Clean up any pushed condition
  if (pushed_condition) {
    fprintf(stderr, "COND_POP: Cleaning up pushed condition\n");
    bson_destroy(pushed_condition);
    pushed_condition = nullptr;
  }
  
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
  
  fprintf(stderr, "EXTRA CALLED with operation: %d\n", (int)operation);
  
  switch (operation) {
    case HA_EXTRA_RESET_STATE:
      fprintf(stderr, "EXTRA: HA_EXTRA_RESET_STATE\n");
      key_read_mode = false;  // Reset key-only mode
      count_mode = false;     // Reset count mode
      break;
    case HA_EXTRA_KEYREAD:
      fprintf(stderr, "EXTRA: HA_EXTRA_KEYREAD - enabling key-only mode\n");
      key_read_mode = true;   // Enable key-only mode for COUNT optimization
      break;
    case HA_EXTRA_NO_KEYREAD:
      fprintf(stderr, "EXTRA: HA_EXTRA_NO_KEYREAD - disabling key-only mode\n");
      key_read_mode = false;  // Disable key-only mode
      break;
    case HA_EXTRA_IGNORE_DUP_KEY:
      fprintf(stderr, "EXTRA: HA_EXTRA_IGNORE_DUP_KEY\n");
      break;
    case HA_EXTRA_NO_IGNORE_DUP_KEY:
      fprintf(stderr, "EXTRA: HA_EXTRA_NO_IGNORE_DUP_KEY\n");
      break;
    case 4:  // Likely HA_EXTRA_RETRIEVE_ALL_COLS or similar
      fprintf(stderr, "EXTRA: Operation 4 (retrieve columns)\n");
      break;
    case 5:  // Required for basic operations
      fprintf(stderr, "EXTRA: Operation 5 (basic operation)\n");
      break;
    case 43: // Required for basic operations
      fprintf(stderr, "EXTRA: Operation 43 (basic operation)\n");
      break;
    case 46: // HA_EXTRA_DETACH_CHILDREN - NOT related to COUNT
      fprintf(stderr, "EXTRA: Operation 46 (HA_EXTRA_DETACH_CHILDREN) - table management operation\n");
      // This is unrelated to COUNT operations - do not set count_mode
      break;
    default:
      fprintf(stderr, "EXTRA: Unknown operation %d - returning success\n", (int)operation);
      break;
  }
  
  // Return success for all operations - be permissive rather than restrictive
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
      // Initialize the memory root for string allocations
      init_alloc_root(PSI_NOT_INSTRUMENTED, &share->mem_root, 512, 0, MYF(0));
      share->use_count = 1;
      share->parsed = false;
      
      fprintf(stderr, "GET_SHARE: Created new share with initialized mem_root\n");
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
    // Clean up the memory root
    free_root(&share->mem_root, MYF(0));
    my_free(share);
    share = nullptr;
    
    fprintf(stderr, "FREE_SHARE: Cleaned up share and mem_root\n");
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
  
  // Use the enhanced global parsing function
  if (mongodb_parse_connection_string(connection_string, share) != 0)
  {
    // Connection string parsing FAILED - this is a fatal error
    fprintf(stderr, "ERROR: Failed to parse connection string: %s\n", connection_string);
    DBUG_RETURN(1); // Return failure, no fallbacks
  }
  
  // Success - create mongo_connection_string from parsed connection_string
  share->mongo_connection_string = my_strdup(PSI_NOT_INSTRUMENTED, share->connection_string, MYF(0));
  
  fprintf(stderr, "SUCCESS: Using parsed connection values - db='%s', collection='%s'\n", 
          share->database_name ? share->database_name : "NULL",
          share->collection_name ? share->collection_name : "NULL");
  
  DBUG_RETURN(0);
}

int ha_mongodb::connect_to_mongodb()
{
  DBUG_ENTER("ha_mongodb::connect_to_mongodb");
  
  // Check if we have the required fields
  fprintf(stderr, "CONNECT: Checking share fields - share=%p\n", (void*)share);
  if (share) {
    fprintf(stderr, "CONNECT: mongo_connection_string=%p, database_name=%p, collection_name=%p\n", 
            (void*)share->mongo_connection_string, (void*)share->database_name, (void*)share->collection_name);
    if (share->mongo_connection_string) {
      fprintf(stderr, "CONNECT: connection_string='%s'\n", share->mongo_connection_string);
    }
    if (share->database_name) {
      fprintf(stderr, "CONNECT: database_name='%s'\n", share->database_name);
    }
    if (share->collection_name) {
      fprintf(stderr, "CONNECT: collection_name='%s'\n", share->collection_name);
    }
  }
  
  if (!share || !share->mongo_connection_string || !share->database_name || !share->collection_name)
  {
    fprintf(stderr, "CONNECT: Missing required fields, returning 1\n");
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
  
  // Report a generic MongoDB connection error via fprintf
  // Note: Using fprintf instead of my_error() to avoid service dependencies
  fprintf(stderr, "MONGODB ERROR: Connection failed - check connection string and server availability\n");
  
  remote_error_number = HA_ERR_NO_CONNECTION;
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
  
  fprintf(stderr, "DEBUG: Table has %u fields:\n", table->s->fields);
  for (uint i = 0; i < table->s->fields; i++) {
    Field *f = table->field[i];
    fprintf(stderr, "  Field[%u]: name='%s', field_index=%u, null_bit=%u\n", 
            i, f->field_name.str, f->field_index, f->null_bit);
  }
  
  uint field_array_index = 0;
  for (field_ptr = table->field; *field_ptr; field_ptr++, field_array_index++)
  {
    Field *field = *field_ptr;
    const char *field_name = field->field_name.str;
    
    fprintf(stderr, "DEBUG: Processing field_name='%s' (length=%lu) at array_index=%u\n", 
            field_name ? field_name : "NULL", field_name ? strlen(field_name) : 0, field_array_index);
    
    if (strcmp(field_name, "_id") == 0)
    {
      // Handle _id field - extract ObjectId and convert to string
      fprintf(stderr, "DEBUG: Matched _id field at array_index=%u\n", field_array_index);
      
      field->ptr = buf + field->offset(table->record[0]);
      convert_mongodb_id_field(doc, field);
    }
    else if (strcmp(field_name, "document") == 0)
    {
      // Handle document field - convert full BSON to JSON and pack into buffer
      fprintf(stderr, "DEBUG: Matched document field at array_index=%u - converting to JSON\n", field_array_index);
      
      // Convert BSON to JSON string using relaxed format (more readable)
      char *json_str = bson_as_relaxed_extended_json(doc, nullptr);
      if (!json_str) {
        fprintf(stderr, "DEBUG: relaxed JSON failed, trying canonical\n");
        // Fallback to canonical format
        json_str = bson_as_canonical_extended_json(doc, nullptr);
      }
      
      if (json_str) {
        fprintf(stderr, "DEBUG: Successfully converted to JSON: %.200s...\n", json_str);
        
        // CRITICAL: Set field pointer to point to the row buffer location for this field
        field->ptr = buf + field->offset(table->record[0]);
        
        // Store the JSON string in the field and pack into buffer
        field->set_notnull();
        CHARSET_INFO *field_charset = field->charset();
        field->store(json_str, strlen(json_str), field_charset);
        
        fprintf(stderr, "DEBUG: JSON stored in field and packed into buffer at offset %lu\n", 
                (unsigned long)field->offset(table->record[0]));
        
        bson_free(json_str);
      } else {
        fprintf(stderr, "DEBUG: JSON conversion failed\n");
        // Store error message
        field->ptr = buf + field->offset(table->record[0]);
        field->set_notnull();
        CHARSET_INFO *field_charset = field->charset();
        field->store("{\"error\":\"Failed to convert BSON to JSON\"}", 37, field_charset);
      }
    }
    else
    {
      // For any other field, extract from document
      fprintf(stderr, "DEBUG: Extracting field='%s' from document\n", field_name);
      
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
  
  if (!doc || !field) {
    fprintf(stderr, "CONVERT_FULL_DOCUMENT_FIELD: NULL doc or field!\n");
    DBUG_RETURN(1);
  }
  
  // Debug field information
  fprintf(stderr, "Field name: %s, field_index: %u, array_index: %u, null_bit: %u\n", 
          field->field_name.str, field->field_index, array_index, field->null_bit);
  
  // Convert BSON to JSON string using relaxed format (more readable)
  char *json_str = bson_as_relaxed_extended_json(doc, nullptr);
  if (!json_str) {
    fprintf(stderr, "CONVERT_FULL_DOCUMENT_FIELD: relaxed JSON failed, trying canonical\n");
    // Fallback to canonical format
    json_str = bson_as_canonical_extended_json(doc, nullptr);
  }
  
  if (json_str) {
    fprintf(stderr, "CONVERT_FULL_DOCUMENT_FIELD: Successfully converted to JSON: %.200s...\n", json_str);
    
    // Clear any existing null flag and set the field
    field->set_notnull();
    
    // Store the JSON string using field's charset
    CHARSET_INFO *field_charset = field->charset();
    int store_result = field->store(json_str, strlen(json_str), field_charset);
    
    fprintf(stderr, "Store result: %d, field->is_null(): %d\n", store_result, field->is_null());
    
    bson_free(json_str);
    
    fprintf(stderr, "CONVERT_FULL_DOCUMENT_FIELD: JSON conversion complete using array_index=%u\n", array_index);
    
    DBUG_RETURN(0);
  }
  
  // Fallback: store error message if JSON conversion fails
  fprintf(stderr, "CONVERT_FULL_DOCUMENT_FIELD: JSON conversion failed, storing error\n");
  field->set_notnull();
  CHARSET_INFO *field_charset = field->charset();
  field->store("{\"error\":\"Failed to convert BSON to JSON\"}", 37, field_charset);
  DBUG_RETURN(0);
}

/*
  Convert simple field directly from MongoDB document
*/
int ha_mongodb::convert_simple_field_from_document(const bson_t *doc, Field *field, const char *field_name)
{
  DBUG_ENTER("ha_mongodb::convert_simple_field_from_document");
  
  fprintf(stderr, "CONVERT_SIMPLE_FIELD CALLED for field: %s\n", field_name ? field_name : "NULL");
  
  bson_iter_t iter;
  
  // Try to find field in document
  if (!bson_iter_init(&iter, doc) || !bson_iter_find(&iter, field_name))
  {
    fprintf(stderr, "FIELD NOT FOUND: %s - leaving as NULL\n", field_name ? field_name : "NULL");
    
    // Field not found - stays NULL (already set in convert_document_to_row)
    DBUG_RETURN(0);
  }
  
  fprintf(stderr, "FIELD FOUND: %s, type=%d\n", field_name ? field_name : "NULL", bson_iter_type(&iter));
  
  // Extract value based on BSON type
  field->set_notnull();
  
  switch (bson_iter_type(&iter))
  {
    case BSON_TYPE_INT32:
    {
      int32_t value = bson_iter_int32(&iter);
      fprintf(stderr, "STORING INT32: %d for field %s\n", value, field_name);
      field->store(value);
      break;
    }
    case BSON_TYPE_INT64:
    {
      int64_t value = bson_iter_int64(&iter);
      fprintf(stderr, "STORING INT64: %lld for field %s\n", (long long)value, field_name);
      field->store(value);
      break;
    }
    case BSON_TYPE_DOUBLE:
    {
      double value = bson_iter_double(&iter);
      fprintf(stderr, "STORING DOUBLE: %f for field %s\n", value, field_name);
      field->store(value);
      break;
    }
    case BSON_TYPE_UTF8:
    {
      uint32_t len;
      const char* value = bson_iter_utf8(&iter, &len);
      fprintf(stderr, "STORING UTF8: '%.*s' for field %s\n", (int)len, value, field_name);
      
      // Use the field's charset instead of hardcoding
      CHARSET_INFO *field_charset = field->charset();
      field->store(value, len, field_charset);
      break;
    }
    default:
    {
      // For other types, store a descriptive string
      fprintf(stderr, "STORING UNSUPPORTED TYPE: %d for field %s\n", bson_iter_type(&iter), field_name);
      
      char type_desc[64];
      snprintf(type_desc, sizeof(type_desc), "[BSON_TYPE_%d]", bson_iter_type(&iter));
      
      // Use the field's charset instead of hardcoding
      CHARSET_INFO *field_charset = field->charset();
      field->store(type_desc, strlen(type_desc), field_charset);
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
  
  fprintf(stderr, "CONVERT_ID_FIELD CALLED!\n");
  
  bson_iter_t iter;
  if (!bson_iter_init(&iter, doc) || !bson_iter_find(&iter, "_id"))
  {
    DBUG_RETURN(1); // No _id field found
  }
  
  field->set_notnull();
  
  if (BSON_ITER_HOLDS_OID(&iter))
  {
    fprintf(stderr, "PROCESSING OBJECTID for _id field\n");
    
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

/*
  Index flags - specify what operations our indexes support
*/
/*
  Helper method implementations
*/
