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

/*
  Query translation interface
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
