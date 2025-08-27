#ifndef MONGODB_TRANSLATOR_INCLUDED
#define MONGODB_TRANSLATOR_INCLUDED

/*
  MongoDB Query Translator for MariaDB
  
  Translates SQL queries to MongoDB aggregation pipelines.
  
  Copyright (c) 2025 MongoDB Storage Engine Contributors
  
  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.
*/

#include <string>
#include <vector>
#include <memory>

// MongoDB C driver includes
#include <mongoc/mongoc.h>
#include <bson/bson.h>

// Forward declarations for MariaDB types (avoid complex header dependencies)
class Item;
class Item_func;
class Item_cond;
class String;

/*
  Condition translation functions for cond_push implementation
*/
namespace mongodb_translator {

// Main translation entry point
bool translate_condition_to_bson(const Item* item, bson_t* match_doc);

// Function and condition translators  
bool translate_function_item(const Item_func* func, bson_t* match_doc);
bool translate_condition_item(const Item_cond* cond, bson_t* match_doc);

// Specific condition types
bool translate_equality(const Item_func* func, bson_t* match_doc);
bool translate_comparison(const Item_func* func, bson_t* match_doc, const char* mongodb_op);
bool translate_in_condition(const Item_func* func, bson_t* match_doc);
bool translate_and_condition(const Item_cond* cond, bson_t* match_doc);
bool translate_or_condition(const Item_cond* cond, bson_t* match_doc);

// Helper functions
bool convert_sql_field_to_mongodb(const char* sql_field, String* mongodb_field);
bool add_value_to_bson(bson_t* doc, const char* key, Item* value_item);

} // namespace mongodb_translator

/*
  Legacy query translation interface (for future aggregation pipeline work)
*/
class MongoQueryTranslator {
public:
    // Constructor
    MongoQueryTranslator() = default;
    ~MongoQueryTranslator() = default;

    // Main translation method
    static bson_t* translate_sql_to_aggregation(const std::string& sql_query);
    
    // Helper methods for specific SQL constructs
    static bson_t* translate_select_to_match(const std::string& where_clause);
    static bson_t* translate_joins(const std::string& join_clause);
    static bson_t* translate_order_by(const std::string& order_clause);
    static bson_t* translate_group_by(const std::string& group_clause);
    
    // Cleanup
    static void cleanup_bson(bson_t* doc);
};

#endif // MONGODB_TRANSLATOR_INCLUDED
