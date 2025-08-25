/*
  MongoDB Schema Registry Implementation
  
  Implements dynamic schema inference from MongoDB collections for SQL table mapping.
  This is a critical Phase 2 component that enables MariaDB to understand MongoDB structure.
*/

#include "mongodb_schema.h"

// Include MariaDB-specific headers only when building as plugin
#ifdef MYSQL_SERVER
#include "my_global.h"
#endif

#include "mongodb_uri_parser.h"
#include <algorithm>
#include <sstream>

// Define missing types for compatibility
#ifndef MYSQL_TYPE_JSON
static const enum_field_types MYSQL_TYPE_JSON_REPLACEMENT = MYSQL_TYPE_LONG_BLOB;
#define MYSQL_TYPE_JSON MYSQL_TYPE_JSON_REPLACEMENT
#endif

// Global schema registry storage
std::mutex global_schema_mutex;
std::map<std::string, std::shared_ptr<MongoSchemaRegistry>> global_schema_registries;

/*
  Constructor - Initialize schema registry with MongoDB connection
*/
MongoSchemaRegistry::MongoSchemaRegistry(const std::string &connection_str)
  : connection_string(connection_str),
    cache_ttl(std::chrono::seconds(MONGODB_DEFAULT_SCHEMA_CACHE_TTL_SECONDS)),
    schema_client(nullptr)
{
  // Initialize MongoDB client for schema operations
  mongoc_init();
  mongoc_uri_t *uri = mongoc_uri_new(connection_str.c_str());
  if (uri) {
    schema_client = mongoc_client_new_from_uri(uri);
    mongoc_uri_destroy(uri);
  }
}

/*
  Destructor - Clean up MongoDB client and resources
*/
MongoSchemaRegistry::~MongoSchemaRegistry()
{
  if (schema_client) {
    mongoc_client_destroy(schema_client);
  }
  mongoc_cleanup();
}

/*
  Infer schema from MongoDB collection by sampling documents
  This is the core Phase 2 functionality for schema discovery
*/
bool MongoSchemaRegistry::infer_schema_from_collection(const std::string &database_name,
                                                      const std::string &collection_name)
{
  if (!schema_client) {
    return false;
  }
  
  std::string table_key = database_name + "." + collection_name;
  
  // Check if we have valid cached schema
  {
    std::lock_guard<std::mutex> lock(cache_mutex);
    auto it = schema_cache.find(table_key);
    if (it != schema_cache.end() && is_cache_valid(it->second)) {
      return true; // Valid cache exists
    }
  }
  
  // Get MongoDB collection
  mongoc_database_t *database = mongoc_client_get_database(schema_client, database_name.c_str());
  mongoc_collection_t *collection = mongoc_client_get_collection(schema_client, database_name.c_str(), collection_name.c_str());
  
  if (!collection) {
    mongoc_database_destroy(database);
    return false;
  }
  
  // Sample documents from collection for schema inference
  std::vector<bson_t*> samples;
  bool sampling_success = sample_collection_documents(collection, samples);
  
  if (!sampling_success || samples.empty()) {
    mongoc_collection_destroy(collection);
    mongoc_database_destroy(database);
    return false;
  }
  
  // Analyze document structure and infer field mappings
  std::map<std::string, MongoFieldMapping> inferred_fields;
  
  for (const bson_t* doc : samples) {
    analyze_document_structure(doc, inferred_fields);
  }
  
  // Create schema cache entry
  MongoSchemaCache cache_entry;
  cache_entry.collection_name = collection_name;
  cache_entry.last_updated = std::chrono::steady_clock::now();
  cache_entry.expires_at = cache_entry.last_updated + cache_ttl;
  cache_entry.is_valid = true;
  cache_entry.estimated_documents = samples.size(); // Rough estimate
  
  // Convert field map to vector
  for (const auto& field_pair : inferred_fields) {
    cache_entry.field_mappings.push_back(field_pair.second);
  }
  
  // Store in cache
  {
    std::lock_guard<std::mutex> lock(cache_mutex);
    schema_cache[table_key] = std::move(cache_entry);
  }
  
  // Cleanup
  for (bson_t* doc : samples) {
    bson_destroy(doc);
  }
  mongoc_collection_destroy(collection);
  mongoc_database_destroy(database);
  
  return true;
}

/*
  Sample documents from MongoDB collection for schema analysis
*/
bool MongoSchemaRegistry::sample_collection_documents(mongoc_collection_t *collection, 
                                                     std::vector<bson_t*> &samples)
{
  if (!collection) {
    return false;
  }
  
  // Create aggregation pipeline for sampling
  bson_t *pipeline = bson_new();
  bson_t sample_stage;
  
  // Use $sample to get random documents for better schema coverage
  BSON_APPEND_DOCUMENT_BEGIN(pipeline, "0", &sample_stage);
  BSON_APPEND_UTF8(&sample_stage, "$sample", "");
  bson_t sample_opts;
  BSON_APPEND_DOCUMENT_BEGIN(&sample_stage, "$sample", &sample_opts);
  BSON_APPEND_INT32(&sample_opts, "size", MONGODB_SCHEMA_SAMPLE_SIZE);
  bson_append_document_end(&sample_stage, &sample_opts);
  bson_append_document_end(pipeline, &sample_stage);
  
  // Execute aggregation
  mongoc_cursor_t *cursor = mongoc_collection_aggregate(
    collection, MONGOC_QUERY_NONE, pipeline, nullptr, nullptr);
  
  if (!cursor) {
    bson_destroy(pipeline);
    return false;
  }
  
  // Collect sample documents
  const bson_t *doc;
  int count = 0;
  
  while (mongoc_cursor_next(cursor, &doc) && count < MONGODB_SCHEMA_SAMPLE_SIZE) {
    bson_t *doc_copy = bson_copy(doc);
    samples.push_back(doc_copy);
    count++;
  }
  
  // Check for cursor errors
  bson_error_t error;
  bool has_error = mongoc_cursor_error(cursor, &error);
  
  mongoc_cursor_destroy(cursor);
  bson_destroy(pipeline);
  
  return !has_error && !samples.empty();
}

/*
  Analyze BSON document structure and extract field mappings
*/
bool MongoSchemaRegistry::analyze_document_structure(const bson_t *doc, 
                                                    std::map<std::string, MongoFieldMapping> &fields)
{
  if (!doc) {
    return false;
  }
  
  bson_iter_t iter;
  if (!bson_iter_init(&iter, doc)) {
    return false;
  }
  
  // Iterate through document fields
  while (bson_iter_next(&iter)) {
    const char *key = bson_iter_key(&iter);
    const bson_value_t *value = bson_iter_value(&iter);
    
    std::string field_name = normalize_field_name(key);
    
    // Skip invalid SQL identifiers
    if (!is_valid_sql_identifier(field_name)) {
      continue;
    }
    
    // Create or update field mapping
    auto it = fields.find(field_name);
    if (it == fields.end()) {
      // New field - create mapping
      MongoFieldMapping mapping;
      mapping.sql_name = field_name;
      mapping.mongo_path = key;
      mapping.sql_type = infer_field_type(value);
      mapping.is_nullable = true; // MongoDB fields can be missing
      
      // Set appropriate max_length based on type
      switch (mapping.sql_type) {
        case MYSQL_TYPE_STRING:
        case MYSQL_TYPE_VAR_STRING:
          mapping.max_length = 255;
          break;
        case MYSQL_TYPE_BLOB:
        case MYSQL_TYPE_LONG_BLOB:
          mapping.max_length = 65535;
          break;
        case MYSQL_TYPE_MEDIUM_BLOB:
          mapping.max_length = 1048576; // 1MB for JSON-like data
          break;
        default:
          mapping.max_length = 0;
      }
      
      fields[field_name] = mapping;
    } else {
      // Existing field - refine type if needed
      enum_field_types current_type = it->second.sql_type;
      enum_field_types inferred_type = infer_field_type(value);
      
      // Use most general type if types differ
      if (current_type != inferred_type) {
        if (current_type == MYSQL_TYPE_LONG && inferred_type == MYSQL_TYPE_DOUBLE) {
          it->second.sql_type = MYSQL_TYPE_DOUBLE;
        } else if (current_type != MYSQL_TYPE_STRING && inferred_type == MYSQL_TYPE_STRING) {
          it->second.sql_type = MYSQL_TYPE_STRING;
          it->second.max_length = 255;
        }
      }
    }
  }
  
  return true;
}

/*
  Infer MariaDB field type from BSON value
*/
enum_field_types MongoSchemaRegistry::infer_field_type(const bson_value_t *value)
{
  if (!value) {
    return MYSQL_TYPE_STRING;
  }
  
  switch (value->value_type) {
    case BSON_TYPE_DOUBLE:
      return MYSQL_TYPE_DOUBLE;
    
    case BSON_TYPE_UTF8:
      return MYSQL_TYPE_STRING;
    
    case BSON_TYPE_DOCUMENT:
    case BSON_TYPE_ARRAY:
      return MYSQL_TYPE_MEDIUM_BLOB; // Use MEDIUMBLOB for JSON-like data
    
    case BSON_TYPE_BINARY:
      return MYSQL_TYPE_BLOB;
    
    case BSON_TYPE_BOOL:
      return MYSQL_TYPE_TINY;
    
    case BSON_TYPE_DATE_TIME:
      return MYSQL_TYPE_DATETIME;
    
    case BSON_TYPE_NULL:
      return MYSQL_TYPE_STRING; // Default for NULL
    
    case BSON_TYPE_INT32:
      return MYSQL_TYPE_LONG;
    
    case BSON_TYPE_TIMESTAMP:
      return MYSQL_TYPE_TIMESTAMP;
    
    case BSON_TYPE_INT64:
      return MYSQL_TYPE_LONGLONG;
    
    case BSON_TYPE_DECIMAL128:
      return MYSQL_TYPE_NEWDECIMAL;
    
    case BSON_TYPE_OID:
      return MYSQL_TYPE_STRING; // ObjectId as string
    
    default:
      return MYSQL_TYPE_STRING; // Default fallback
  }
}

bool MongoSchemaRegistry::document_to_row(const bson_t *doc, uchar *buf, TABLE *table)
{
  // TODO: Implement document-to-row conversion
  // This is a critical method that will be implemented in Phase 2.6
  return false;
}

bool MongoSchemaRegistry::row_to_document(const uchar *buf, TABLE *table, bson_t **doc)
{
  // TODO: Implement row-to-document conversion for write operations
  // This will be implemented in Phase 3
  return false;
}

bool MongoSchemaRegistry::get_field_mappings(const std::string &table_name,
                                            std::vector<MongoFieldMapping> &mappings)
{
  std::lock_guard<std::mutex> lock(cache_mutex);
  
  auto it = schema_cache.find(table_name);
  if (it != schema_cache.end() && is_cache_valid(it->second)) {
    mappings = it->second.field_mappings;
    return true;
  }
  
  return false;
}

bool MongoSchemaRegistry::is_cache_valid(const MongoSchemaCache &cache) const
{
  auto now = std::chrono::steady_clock::now();
  return cache.is_valid && (now < cache.expires_at);
}

void MongoSchemaRegistry::invalidate_cache(const std::string &table_name)
{
  std::lock_guard<std::mutex> lock(cache_mutex);
  
  auto it = schema_cache.find(table_name);
  if (it != schema_cache.end()) {
    it->second.is_valid = false;
  }
}

void MongoSchemaRegistry::clear_all_cache()
{
  std::lock_guard<std::mutex> lock(cache_mutex);
  schema_cache.clear();
}

/*
  Utility functions for field name normalization and validation
*/
std::string normalize_field_name(const std::string &name)
{
  std::string normalized = name;
  
  // Replace invalid characters with underscores
  for (char &c : normalized) {
    if (!std::isalnum(c) && c != '_') {
      c = '_';
    }
  }
  
  // Ensure it starts with letter or underscore
  if (!normalized.empty() && std::isdigit(normalized[0])) {
    normalized = "_" + normalized;
  }
  
  return normalized;
}

bool is_valid_sql_identifier(const std::string &name)
{
  if (name.empty() || name.length() > 64) {
    return false;
  }
  
  // Must start with letter or underscore
  if (!std::isalpha(name[0]) && name[0] != '_') {
    return false;
  }
  
  // Rest must be alphanumeric or underscore
  for (size_t i = 1; i < name.length(); i++) {
    if (!std::isalnum(name[i]) && name[i] != '_') {
      return false;
    }
  }
  
  return true;
}

/*
  BSON type to MariaDB type conversion utilities
*/
enum_field_types bson_type_to_mysql_type(bson_type_t bson_type)
{
  switch (bson_type) {
    case BSON_TYPE_DOUBLE:
      return MYSQL_TYPE_DOUBLE;
    case BSON_TYPE_UTF8:
      return MYSQL_TYPE_STRING;
    case BSON_TYPE_DOCUMENT:
    case BSON_TYPE_ARRAY:
      return MYSQL_TYPE_JSON;
    case BSON_TYPE_BINARY:
      return MYSQL_TYPE_BLOB;
    case BSON_TYPE_BOOL:
      return MYSQL_TYPE_TINY;
    case BSON_TYPE_DATE_TIME:
      return MYSQL_TYPE_DATETIME;
    case BSON_TYPE_INT32:
      return MYSQL_TYPE_LONG;
    case BSON_TYPE_TIMESTAMP:
      return MYSQL_TYPE_TIMESTAMP;
    case BSON_TYPE_INT64:
      return MYSQL_TYPE_LONGLONG;
    case BSON_TYPE_DECIMAL128:
      return MYSQL_TYPE_NEWDECIMAL;
    case BSON_TYPE_OID:
      return MYSQL_TYPE_STRING;
    default:
      return MYSQL_TYPE_STRING;
  }
}

const char* mysql_type_to_string(enum_field_types type)
{
  switch (type) {
    case MYSQL_TYPE_TINY: return "TINYINT";
    case MYSQL_TYPE_SHORT: return "SMALLINT";
    case MYSQL_TYPE_LONG: return "INT";
    case MYSQL_TYPE_LONGLONG: return "BIGINT";
    case MYSQL_TYPE_FLOAT: return "FLOAT";
    case MYSQL_TYPE_DOUBLE: return "DOUBLE";
    case MYSQL_TYPE_NEWDECIMAL: return "DECIMAL";
    case MYSQL_TYPE_STRING: return "VARCHAR";
    case MYSQL_TYPE_VAR_STRING: return "VARCHAR";
    case MYSQL_TYPE_BLOB: return "BLOB";
    case MYSQL_TYPE_LONG_BLOB: return "LONGBLOB";
    case MYSQL_TYPE_MEDIUM_BLOB: return "MEDIUMBLOB";
    case MYSQL_TYPE_DATETIME: return "DATETIME";
    case MYSQL_TYPE_TIMESTAMP: return "TIMESTAMP";
    case MYSQL_TYPE_DATE: return "DATE";
    case MYSQL_TYPE_TIME: return "TIME";
    default: return "VARCHAR";
  }
}

/*
  Global schema registry management
*/
MongoSchemaRegistry* get_or_create_schema_registry(const std::string &connection_string)
{
  std::lock_guard<std::mutex> lock(global_schema_mutex);
  
  auto it = global_schema_registries.find(connection_string);
  if (it != global_schema_registries.end()) {
    return it->second.get();
  }
  
  // Create new registry
  auto registry = std::make_shared<MongoSchemaRegistry>(connection_string);
  global_schema_registries[connection_string] = registry;
  
  return registry.get();
}

void cleanup_all_schema_registries()
{
  std::lock_guard<std::mutex> lock(global_schema_mutex);
  global_schema_registries.clear();
}
