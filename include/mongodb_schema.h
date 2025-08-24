#ifndef MONGODB_SCHEMA_H
#define MONGODB_SCHEMA_H

/*
  MongoDB Schema Registry and Management
  
  Provides dynamic schema inference, field mapping, and document-to-row conversion
  for MongoDB's flexible document structure in SQL context.
*/

#include "my_global.h"
#include "field.h"
#include <mongoc/mongoc.h>
#include <bson/bson.h>
#include <map>
#include <vector>
#include <string>
#include <mutex>
#include <chrono>

/*
  Schema configuration
*/
#define MONGODB_DEFAULT_SCHEMA_CACHE_TTL_SECONDS 300
#define MONGODB_MAX_FIELD_MAPPINGS 1000
#define MONGODB_SCHEMA_SAMPLE_SIZE 100

/*
  Field mapping information for MongoDB document fields
*/
struct MongoFieldMapping {
  std::string sql_name;              // SQL column name
  std::string mongo_path;            // MongoDB field path (e.g., "address.city")
  enum_field_types sql_type;         // MariaDB field type
  bool is_virtual;                   // Virtual computed field
  bool is_indexed;                   // Has MongoDB index
  bool is_nullable;                  // Can be NULL
  std::string default_value;         // Default value if missing
  uint32_t max_length;               // Maximum field length
  uint32_t decimals;                 // Decimal places for numeric types
  
  MongoFieldMapping()
    : sql_type(MYSQL_TYPE_STRING), is_virtual(false), is_indexed(false),
      is_nullable(true), max_length(255), decimals(0) {}
};

/*
  Schema cache entry for a MongoDB collection
*/
struct MongoSchemaCache {
  std::string collection_name;
  std::vector<MongoFieldMapping> field_mappings;
  std::chrono::steady_clock::time_point last_updated;
  std::chrono::steady_clock::time_point expires_at;
  ha_rows estimated_documents;
  size_t average_document_size;
  bool is_valid;
  
  MongoSchemaCache() : estimated_documents(0), average_document_size(0), is_valid(false) {}
};

/*
  MongoDB Schema Registry - manages dynamic schema inference and caching
*/
class MongoSchemaRegistry {
private:
  std::map<std::string, MongoSchemaCache> schema_cache;
  std::mutex cache_mutex;
  std::chrono::seconds cache_ttl;
  
  // MongoDB connection for schema operations
  mongoc_client_t *schema_client;
  std::string connection_string;
  
  // Schema inference methods
  bool sample_collection_documents(mongoc_collection_t *collection, 
                                  std::vector<bson_t*> &samples);
  enum_field_types infer_field_type(const bson_value_t *value);
  bool analyze_document_structure(const bson_t *doc, 
                                 std::map<std::string, MongoFieldMapping> &fields);
  bool merge_field_mappings(const std::map<std::string, MongoFieldMapping> &new_fields,
                           std::vector<MongoFieldMapping> &existing_fields);
  
  // Cache management
  bool is_cache_valid(const MongoSchemaCache &cache) const;
  void cleanup_expired_cache();

public:
  MongoSchemaRegistry(const std::string &connection_str);
  ~MongoSchemaRegistry();
  
  // Schema inference and management
  bool infer_schema_from_collection(const std::string &database_name,
                                   const std::string &collection_name);
  bool register_field_mapping(const std::string &table_name, 
                             const MongoFieldMapping &mapping);
  bool get_field_mappings(const std::string &table_name,
                         std::vector<MongoFieldMapping> &mappings);
  
  // Document conversion
  bool document_to_row(const bson_t *doc, uchar *buf, TABLE *table);
  bool row_to_document(const uchar *buf, TABLE *table, bson_t **doc);
  
  // Field access and validation
  bool get_field_value(const bson_t *doc, const std::string &path, bson_value_t *value);
  bool set_field_value(bson_t *doc, const std::string &path, const bson_value_t *value);
  bool validate_field_mapping(const MongoFieldMapping &mapping);
  
  // Schema evolution and maintenance
  void refresh_schema(const std::string &table_name);
  bool validate_schema_compatibility(const std::string &table_name, TABLE *table);
  void invalidate_cache(const std::string &table_name);
  void clear_all_cache();
  
  // Statistics and monitoring
  size_t get_cache_size() const;
  double get_cache_hit_ratio() const;
  std::vector<std::string> get_cached_tables() const;
  
  // Configuration
  void set_cache_ttl(std::chrono::seconds ttl);
  std::chrono::seconds get_cache_ttl() const { return cache_ttl; }
  
  // Connection management
  bool reconnect();
  bool is_connected() const;
};

/*
  Global schema registry management
*/
extern std::mutex global_schema_mutex;
extern std::map<std::string, std::shared_ptr<MongoSchemaRegistry>> global_schema_registries;

/*
  Helper functions for schema operations
*/
MongoSchemaRegistry* get_or_create_schema_registry(const std::string &connection_string);
void cleanup_all_schema_registries();

/*
  BSON type to MariaDB type conversion utilities
*/
enum_field_types bson_type_to_mysql_type(bson_type_t bson_type);
const char* mysql_type_to_string(enum_field_types type);
bool is_numeric_type(enum_field_types type);
bool is_string_type(enum_field_types type);
bool is_date_type(enum_field_types type);

/*
  MongoDB field path utilities
*/
bool parse_field_path(const std::string &path, std::vector<std::string> &components);
std::string normalize_field_name(const std::string &name);
bool is_valid_sql_identifier(const std::string &name);

#endif /* MONGODB_SCHEMA_H */
