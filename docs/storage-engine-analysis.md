# MariaDB Storage Engine Analysis

## Overview
This document contains our analysis of existing MariaDB storage engines to understand implementation patterns for building the MongoDB Storage Engine. We examined the **FederatedX** and **example** storage engines as primary references.

*Generated: August 23, 2025*

## Key Findings from Storage Engine Analysis

### 🏗️ Architecture Patterns

#### 1. Share Structure Pattern (Metadata Management)
All storage engines use a shared structure for metadata that's shared across multiple handler instances:

```cpp
// Pattern from FederatedX
typedef struct st_federatedx_share {
  MEM_ROOT mem_root;              // Memory management
  const char *share_key;          // Unique identifier (db/table)
  char *connection_string;        // Full connection info
  
  // Parsed connection components
  char *hostname, *username, *password, *database;
  char *table_name;
  ushort port;
  
  uint use_count;                 // Reference counting
  THR_LOCK lock;                  // Thread safety
  FEDERATEDX_SERVER *s;           // Connection server reference
} FEDERATEDX_SHARE;
```

**Key Insights:**
- Uses `MEM_ROOT` for efficient memory management
- Reference counting (`use_count`) for resource sharing
- Thread-safe with `THR_LOCK`
- Stores both raw and parsed connection information

#### 2. Handler Class Structure
```cpp
class ha_federatedx final : public handler {
  THR_LOCK_DATA lock;                   // MariaDB lock integration
  FEDERATEDX_SHARE *share;              // Shared metadata
  federatedx_io *io;                    // Abstract I/O interface
  FEDERATEDX_IO_RESULT *stored_result;  // Query results
  FEDERATEDX_IO_ROWS *current;          // Current row position
  
public:
  ha_federatedx(handlerton *hton, TABLE_SHARE *table_arg);
  
  // Required methods (MUST implement)
  int open(const char *name, int mode, uint test_if_locked) override;
  int close(void) override;
  int rnd_init(bool scan) override;
  int rnd_next(uchar *buf) override;
  int rnd_end() override;
  int rnd_pos(uchar *buf, uchar *pos) override;
  void position(const uchar *record) override;
  int info(uint) override;
  int create(const char *name, TABLE *form, HA_CREATE_INFO *create_info) override;
  int external_lock(THD *thd, int lock_type) override;
  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to, enum thr_lock_type lock_type) override;
  
  // Optional methods (implement as needed)
  int index_init(uint keynr, bool sorted) override;
  int index_read_map(uchar *buf, const uchar *key, key_part_map keypart_map, enum ha_rkey_function find_flag) override;
  int index_next(uchar *buf) override;
  int index_end() override;
  int write_row(const uchar *buf) override;
  int update_row(const uchar *old_data, const uchar *new_data) override;
  int delete_row(const uchar *buf) override;
  
  // Table capability flags
  ulonglong table_flags() const override;
  ulong index_flags(uint inx, uint part, bool all_parts) const override;
};
```

#### 3. Abstract I/O Interface Pattern
FederatedX uses an abstract I/O layer that can support multiple database backends:

```cpp
class federatedx_io {
  FEDERATEDX_SERVER * const server;
  bool active, busy, readonly;
  
protected:
  virtual int query(const char *buffer, size_t length) = 0;
  virtual FEDERATEDX_IO_RESULT *store_result() = 0;
  virtual FEDERATEDX_IO_ROW *fetch_row(FEDERATEDX_IO_RESULT *result) = 0;
  virtual const char *get_column_data(FEDERATEDX_IO_ROW *row, unsigned int column) = 0;
  virtual bool is_column_null(const FEDERATEDX_IO_ROW *row, unsigned int column) const = 0;
  
public:
  static federatedx_io *construct(MEM_ROOT *server_root, FEDERATEDX_SERVER *server);
  virtual int commit() = 0;
  virtual int rollback() = 0;
};
```

### 🔌 Plugin Registration Pattern

#### 1. Handlerton (Storage Engine Descriptor)
```cpp
// Handler factory function
static handler *federatedx_create_handler(handlerton *hton,
                                         TABLE_SHARE *table,
                                         MEM_ROOT *mem_root) {
  return new (mem_root) ha_federatedx(hton, table);
}

// Handlerton global instance
handlerton* federatedx_hton;
```

#### 2. Initialization Function
```cpp
int federatedx_db_init(void *p) {
  init_federated_psi_keys();                    // Performance monitoring
  federatedx_hton = (handlerton *)p;
  federatedx_hton->db_type = DB_TYPE_FEDERATED_DB;
  federatedx_hton->create = federatedx_create_handler;
  federatedx_hton->flags = HTON_ALTER_NOT_SUPPORTED;
  
  // Transaction support
  federatedx_hton->close_connection = ha_federatedx::disconnect;
  federatedx_hton->savepoint_set = ha_federatedx::savepoint_set;
  federatedx_hton->savepoint_rollback = ha_federatedx::savepoint_rollback;
  federatedx_hton->savepoint_release = ha_federatedx::savepoint_release;
  federatedx_hton->commit = ha_federatedx::commit;
  federatedx_hton->rollback = ha_federatedx::rollback;
  
  // Initialize global resources
  if (mysql_mutex_init(&federatedx_mutex, MY_MUTEX_INIT_FAST))
    goto error;
  
  // Hash tables for shared resources
  if (!my_hash_init(&federatedx_open_tables, &my_charset_bin, 32, 0, 0, 
                    federatedx_share_get_key, 0, 0))
    return FALSE;
error:
  return TRUE;
}
```

#### 3. Plugin Declaration
```cpp
// System variables
static MYSQL_SYSVAR_BOOL(pushdown, use_pushdown, 0,
  "Use query fragments pushdown capabilities", NULL, NULL, FALSE);
static struct st_mysql_sys_var* sysvars[] = { MYSQL_SYSVAR(pushdown), NULL };

// Plugin registration
maria_declare_plugin(federatedx)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,        // Plugin type
  &federatedx_storage_engine,         // Storage engine structure
  "FEDERATED",                        // Engine name (SQL)
  "Patrick Galbraith",                // Author
  "Allows one to access tables on other MariaDB servers, supports transactions and more",
  PLUGIN_LICENSE_GPL,                 // License
  federatedx_db_init,                 // Init function
  federatedx_done,                    // Cleanup function
  0x0201,                            // Version (2.1)
  NULL,                              // Status variables
  sysvars,                           // System variables
  "2.1",                             // String version
  MariaDB_PLUGIN_MATURITY_STABLE     // Maturity level
}
maria_declare_plugin_end;
```

### 📊 Data Flow Patterns

#### 1. Table Lifecycle
```
CREATE TABLE → parse CONNECTION string → store in share
OPEN table → acquire/create share → establish connection → prepare cursor
SCAN table → rnd_init() → execute query → rnd_next() loop → rnd_end()
CLOSE table → release connection → decrement share use_count
DROP share → cleanup resources when use_count = 0
```

#### 2. Row Data Conversion
```
Remote format → Fetch via I/O interface → Convert to MariaDB format (uchar *buf)
```

FederatedX example:
```cpp
uint ha_federatedx::convert_row_to_internal_format(uchar *buf, 
                                                   FEDERATEDX_IO_ROW *row,
                                                   FEDERATEDX_IO_RESULT *result) {
  Field **field = table->field;
  uchar *old_ptr = buf;
  
  for (uint i = 0; i < table->s->fields; i++, field++) {
    if (io->is_column_null(row, i)) {
      (*field)->set_null();
    } else {
      const char *data = io->get_column_data(row, i);
      (*field)->set_notnull();
      (*field)->store(data, strlen(data), &my_charset_bin);
    }
    (*field)->move_field_offset(old_ptr - buf);
  }
  return 0;
}
```

### 🎯 MongoDB Storage Engine Application

#### 1. MongoDB Share Structure
```cpp
typedef struct st_mongodb_share {
  MEM_ROOT mem_root;
  char *share_key;
  char *connection_string;
  
  // Parsed MongoDB URI components
  char *hostname;
  char *username; 
  char *password;
  char *database_name;
  char *collection_name;
  int port;
  char *replica_set;
  char *auth_source;
  bool ssl_enabled;
  
  // MongoDB-specific components
  MongoSchemaRegistry *schema_registry;     // Dynamic schema management
  MongoConnectionPool *connection_pool;     // Connection pooling
  time_t schema_last_updated;              // Schema cache invalidation
  
  // Standard components
  uint use_count;
  THR_LOCK lock;
  mysql_mutex_t mutex;
} MONGODB_SHARE;
```

#### 2. MongoDB I/O Interface
```cpp
class mongodb_io {
  mongoc_client_t *client;
  mongoc_collection_t *collection;
  mongoc_cursor_t *cursor;
  const bson_t *current_doc;
  MongoSchemaRegistry *schema_registry;
  
public:
  // Core operations
  int execute_aggregation(const bson_t *pipeline);
  int execute_find(const bson_t *filter, const bson_t *projection);
  bson_t* fetch_document();
  bool has_more_documents();
  
  // Data conversion
  int convert_document_to_row(const bson_t *doc, uchar *buf, TABLE *table);
  int convert_row_to_document(const uchar *buf, TABLE *table, bson_t *doc);
  
  // Query translation
  bson_t* translate_where_condition(const Item *cond);
  bson_t* build_aggregation_pipeline(const Item *where_cond, 
                                    const ORDER *order_by, 
                                    ha_rows limit_count);
  
  // Connection management
  void reset_cursor();
  int connect_to_mongodb();
  void disconnect();
  
  // Error handling
  bool has_error() const;
  std::string get_last_error() const;
};
```

#### 3. MongoDB Handler Class
```cpp
class ha_mongodb final : public handler {
  THR_LOCK_DATA lock;
  MONGODB_SHARE *share;
  mongodb_io *io;
  mongoc_cursor_t *current_cursor;
  bool position_called;
  
  // MongoDB-specific members
  MongoSchemaRegistry *schema_registry;
  bson_t *current_filter;
  bson_t *pushed_condition;
  
private:
  // Internal helpers
  int parse_connection_string(const char *connection_string);
  MONGODB_SHARE *get_share();
  int free_share();
  int connect_to_mongodb();
  
public:
  ha_mongodb(handlerton *hton, TABLE_SHARE *table_arg);
  ~ha_mongodb();
  
  // Required MariaDB interface methods
  int open(const char *name, int mode, uint test_if_locked) override;
  int close(void) override;
  int rnd_init(bool scan) override;
  int rnd_next(uchar *buf) override;
  int rnd_end() override;
  int rnd_pos(uchar *buf, uchar *pos) override;
  void position(const uchar *record) override;
  int info(uint) override;
  int create(const char *name, TABLE *form, HA_CREATE_INFO *create_info) override;
  int external_lock(THD *thd, int lock_type) override;
  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to, enum thr_lock_type lock_type) override;
  
  // MongoDB-optimized methods
  int index_init(uint keynr, bool sorted) override;
  int index_read_map(uchar *buf, const uchar *key, key_part_map keypart_map, enum ha_rkey_function find_flag) override;
  int index_next(uchar *buf) override;
  int index_end() override;
  
  // Condition pushdown support
  const Item *cond_push(const Item *cond) override;
  void cond_pop() override;
  
  // Table capabilities
  ulonglong table_flags() const override {
    return (HA_FILE_BASED | HA_REC_NOT_IN_SEQ | 
            HA_CAN_INDEX_BLOBS | HA_BINLOG_ROW_CAPABLE | 
            HA_BINLOG_STMT_CAPABLE | HA_PARTIAL_COLUMN_READ |
            HA_NULL_IN_KEY | HA_NON_COMPARABLE_ROWID);
  }
  
  ulong index_flags(uint inx, uint part, bool all_parts) const override {
    return (HA_READ_NEXT | HA_READ_RANGE | HA_KEYREAD_ONLY);
  }
};
```

## Implementation Priorities

### Phase 1: Foundation (Weeks 1-3)
**Goals**: Basic plugin infrastructure working
- [x] Analyze existing storage engines (COMPLETED)
- [ ] Create basic project structure
- [ ] Implement minimal plugin registration
- [ ] Basic CONNECTION string parsing
- [ ] Simple table scanning without MongoDB connectivity

### Phase 2: Core Functionality (Weeks 4-8)
**Goals**: MongoDB connectivity and basic operations
- [ ] MongoDB connection management using libmongoc
- [ ] Schema inference from MongoDB collections
- [ ] Document-to-row conversion
- [ ] Basic query translation (find operations)

### Phase 3: Advanced Features (Weeks 9-12)
**Goals**: Query optimization and advanced features
- [ ] Aggregation pipeline translation
- [ ] Index utilization
- [ ] Condition pushdown implementation
- [ ] Cross-engine join support

### Phase 4: Production Features (Weeks 13-16)
**Goals**: Performance and production readiness
- [ ] Connection pooling optimization
- [ ] Performance monitoring
- [ ] Comprehensive error handling
- [ ] Transaction support (where applicable)

## Key Technical Insights

### 1. Critical Include Order
```cpp
#define MYSQL_SERVER 1
#include "my_global.h"
#include "handler.h"
#include "thr_lock.h"
// ... other MariaDB headers
// MongoDB headers last
#include <mongoc/mongoc.h>
#include <bson/bson.h>
```

### 2. Memory Management Pattern
- Use `MEM_ROOT` for efficient allocation/deallocation
- All share-related memory allocated from share's `mem_root`
- Automatic cleanup when share is destroyed

### 3. Thread Safety Requirements
- All shared structures must be protected with mutexes
- Use `THR_LOCK` for MariaDB integration
- Connection pooling requires careful synchronization

### 4. Error Handling Pattern
```cpp
// Store remote errors in handler instance
int ha_mongodb::stash_remote_error() {
  const bson_error_t *error = mongodb_io_get_last_error(io);
  remote_error_number = error->code;
  strmake(remote_error_buf, error->message, sizeof(remote_error_buf)-1);
  return error->code;
}

// Integrate with MariaDB error reporting
bool ha_mongodb::get_error_message(int error, String *buf) {
  if (error == HA_MONGODB_ERROR_WITH_REMOTE_SYSTEM) {
    buf->append(remote_error_buf);
    return true;
  }
  return false;
}
```

## Next Steps

1. **Create Basic Project Structure**: Set up CMake build system with cross-platform discovery
2. **Implement Minimal Plugin**: Get "INSTALL SONAME 'mongodb'" working
3. **Add MongoDB Connectivity**: Basic libmongoc integration
4. **Implement Table Scanning**: Simple find() operations
5. **Add Schema Inference**: Dynamic field mapping from documents

## References

- **FederatedX Source**: `sources/server/storage/federatedx/`
- **Example Engine**: `sources/server/storage/example/`
- **Handler Base Class**: `sources/server/sql/handler.h`
- **MariaDB Plugin API**: `sources/server/include/mysql/plugin.h`
- **MongoDB C Driver**: `sources/mongo-c-driver/`

---

*This analysis provides the foundation for implementing a production-ready MongoDB storage engine that follows MariaDB's established patterns and best practices.*
