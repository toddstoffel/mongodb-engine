/*
  MongoDB Storage Engine for MariaDB
  
  This storage engine enables querying MongoDB collections as virtual SQL tables,
  supporting cross-engine joins and SQL-to-MongoDB query translation.
  
  Copyright (c) 2025 MongoDB Storage Engine Contributors
  
  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.
*/

// Critical: MariaDB headers - use local sources properly
#include "my_global.h"
#include "mysql/plugin.h"
#include <mysql.h>
// Include key MariaDB handler definitions
#include "handler.h"
#include "ha_mongodb.h"
// Add missing header for sql_print_* functions
#include "log.h"

// Project headers
#include "mongodb_connection.h"
#include "mongodb_schema.h"

// MongoDB C driver (after MariaDB headers)
#include <mongoc/mongoc.h>
#include <bson/bson.h>

/*
  Global storage engine state - handlerton must be non-static for proper registration
*/
handlerton *mongodb_hton;
static mysql_mutex_t mongodb_mutex;
static HASH mongodb_open_tables;
static HASH mongodb_open_servers;

/*
  System variables for MongoDB storage engine configuration
*/
static int mongodb_connection_timeout = 30;  // seconds (int for MYSQL_SYSVAR_INT)
static int mongodb_max_connections = 10;     // per server (int for MYSQL_SYSVAR_INT)
static my_bool mongodb_enable_aggregation_pushdown = TRUE;
static my_bool mongodb_enable_schema_cache = TRUE;
static int mongodb_schema_cache_ttl = 300;   // seconds (int for MYSQL_SYSVAR_INT)

/*
  Status variables for monitoring
*/
static long long mongodb_queries_translated = 0;
static long long mongodb_connections_active = 0;
static long long mongodb_schema_cache_hits = 0;
static long long mongodb_schema_cache_misses = 0;
static long long mongodb_documents_scanned = 0;
static long long mongodb_rows_returned = 0;

/*
  Forward declarations
*/
static handler *mongodb_create_handler(handlerton *hton, TABLE_SHARE *table,
                                      MEM_ROOT *mem_root);
static int mongodb_init_func(void *p);
static int mongodb_done_func(void *p);

/*
  File extensions for MongoDB storage engine (like EXAMPLE engine)
*/
static const char *ha_mongodb_exts[] = {
  NullS
};

/*
  Plugin registration structure - must be global for plugin loading
*/
struct st_mysql_storage_engine mongodb_storage_engine = {
  MYSQL_HANDLERTON_INTERFACE_VERSION
};

/*
  System variables declarations
*/
static MYSQL_SYSVAR_INT(connection_timeout, mongodb_connection_timeout,
  PLUGIN_VAR_RQCMDARG,
  "MongoDB connection timeout in seconds",
  nullptr, nullptr, 30, 1, 300, 0);

static MYSQL_SYSVAR_INT(max_connections, mongodb_max_connections,
  PLUGIN_VAR_RQCMDARG,
  "Maximum number of MongoDB connections per server",
  nullptr, nullptr, 10, 1, 100, 0);

static MYSQL_SYSVAR_BOOL(enable_aggregation_pushdown, mongodb_enable_aggregation_pushdown,
  PLUGIN_VAR_RQCMDARG,
  "Enable pushing down aggregation operations to MongoDB",
  nullptr, nullptr, TRUE);

static MYSQL_SYSVAR_BOOL(enable_schema_cache, mongodb_enable_schema_cache,
  PLUGIN_VAR_RQCMDARG,
  "Enable caching of MongoDB collection schemas",
  nullptr, nullptr, TRUE);

static MYSQL_SYSVAR_INT(schema_cache_ttl, mongodb_schema_cache_ttl,
  PLUGIN_VAR_RQCMDARG,
  "MongoDB schema cache TTL in seconds",
  nullptr, nullptr, 300, 60, 3600, 0);

static struct st_mysql_sys_var* mongodb_system_variables[] = {
  MYSQL_SYSVAR(connection_timeout),
  MYSQL_SYSVAR(max_connections),
  MYSQL_SYSVAR(enable_aggregation_pushdown),
  MYSQL_SYSVAR(enable_schema_cache),
  MYSQL_SYSVAR(schema_cache_ttl),
  nullptr
};

/*
  Status variables declarations
*/
static struct st_mysql_show_var mongodb_status_variables[] = {
  {"mongodb_queries_translated", (char*)&mongodb_queries_translated, SHOW_LONGLONG},
  {"mongodb_connections_active", (char*)&mongodb_connections_active, SHOW_LONGLONG},
  {"mongodb_schema_cache_hits", (char*)&mongodb_schema_cache_hits, SHOW_LONGLONG},
  {"mongodb_schema_cache_misses", (char*)&mongodb_schema_cache_misses, SHOW_LONGLONG},
  {"mongodb_documents_scanned", (char*)&mongodb_documents_scanned, SHOW_LONGLONG},
  {"mongodb_rows_returned", (char*)&mongodb_rows_returned, SHOW_LONGLONG},
  {nullptr, nullptr, SHOW_UNDEF}
};

/*
  Hash key extraction functions for share management
*/
static const uchar *mongodb_share_get_key(const void *share_, size_t *length, my_bool)
{
  const MONGODB_SHARE *share = static_cast<const MONGODB_SHARE*>(share_);
  *length = share->share_key_length;
  return reinterpret_cast<const uchar*>(share->share_key);
}

static const uchar *mongodb_server_get_key(const void *server_, size_t *length, my_bool)
{
  const MONGODB_SERVER *server = static_cast<const MONGODB_SERVER*>(server_);
  *length = server->key_length;
  return reinterpret_cast<const uchar*>(server->key);
}

/*
  Handler factory function
*/
static handler *mongodb_create_handler(handlerton *hton, TABLE_SHARE *table,
                                      MEM_ROOT *mem_root)
{
  DBUG_ENTER("mongodb_create_handler");
  DBUG_RETURN((handler*) new (mem_root) ha_mongodb(hton, table));
}

/*
  Storage engine initialization - simplified like CONNECT engine
*/
static int mongodb_init_func(void *p)
{
  DBUG_ENTER("mongodb_init_func");
  
  // Initialize MongoDB C driver
  mongoc_init();
  
  // Set up handlerton with minimal configuration like CONNECT and EXAMPLE
  mongodb_hton = static_cast<handlerton*>(p);
  mongodb_hton->db_type = DB_TYPE_FIRST_DYNAMIC;  // Use dynamic type, not DEFAULT
  mongodb_hton->create = mongodb_create_handler;
  mongodb_hton->flags = HTON_CAN_RECREATE;
  mongodb_hton->tablefile_extensions = ha_mongodb_exts;
  
  sql_print_information("MongoDB storage engine initialized successfully");
  DBUG_RETURN(0);
}

/*
  Storage engine cleanup - simplified 
*/
static int mongodb_done_func(void *p)
{
  DBUG_ENTER("mongodb_done_func");
  
  // Cleanup MongoDB C driver
  mongoc_cleanup();
  
  sql_print_information("MongoDB storage engine shut down successfully");
  DBUG_RETURN(0);
}

/*
  Plugin declaration - this is what MariaDB loads
*/
maria_declare_plugin(mongodb)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &mongodb_storage_engine,
  "MONGODB",
  "MongoDB Storage Engine Contributors",
  "MongoDB Storage Engine for MariaDB - Cross-engine SQL/NoSQL integration",
  PLUGIN_LICENSE_GPL,
  mongodb_init_func,                /* Plugin Init */
  mongodb_done_func,                /* Plugin Deinit */
  0x0100,                          /* Version: 1.0 (simple) */
  NULL,                            /* Status variables */
  NULL,                            /* System variables */
  "1.0",                           /* Version string */
  MariaDB_PLUGIN_MATURITY_STABLE   /* Maturity level */
}
maria_declare_plugin_end;
