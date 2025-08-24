#ifndef MONGODB_URI_PARSER_H
#define MONGODB_URI_PARSER_H

/*
  MongoDB URI Parser
  
  Parses and validates MongoDB connection strings with support for:
  - Standard mongodb:// URIs  
  - MongoDB Atlas mongodb+srv:// URIs
  - Authentication parameters
  - Connection options
  - Database and collection specification
*/

#include "my_global.h"
#include <string>
#include <vector>
#include <map>

/*
  Parsed MongoDB URI components
*/
struct MongoURI {
  // Protocol
  bool is_srv;                    // true for mongodb+srv://, false for mongodb://
  
  // Authentication
  std::string username;
  std::string password;
  std::string auth_source;
  
  // Connection
  std::vector<std::pair<std::string, int>> hosts;  // host:port pairs
  std::string replica_set;
  
  // Target database and collection
  std::string database;
  std::string collection;
  
  // Connection options
  bool ssl;
  int connect_timeout_ms;
  int socket_timeout_ms;
  std::map<std::string, std::string> options;
  
  // Validation
  bool is_valid;
  std::string error_message;
  
  // Default constructor
  MongoURI() : is_srv(false), ssl(false), connect_timeout_ms(30000), 
               socket_timeout_ms(30000), is_valid(false) {}
  
  // Get the original connection string for mongo-c-driver
  std::string to_connection_string() const;
  
  // Get a safe connection string for logging (password masked)
  std::string to_safe_string() const;
};

/*
  MongoDB URI Parser class
*/
class MongoURIParser {
public:
  // Main parsing function
  static MongoURI parse(const std::string& connection_string);
  
  // Validation functions
  static bool validate_hostname(const std::string& hostname);
  static bool validate_port(int port);
  static bool validate_database_name(const std::string& database);
  static bool validate_collection_name(const std::string& collection);
  
private:
  // Internal parsing functions
  static bool parse_protocol(const std::string& uri, MongoURI& result, size_t& pos);
  static bool parse_credentials(const std::string& uri, MongoURI& result, size_t& pos);
  static bool parse_hosts(const std::string& uri, MongoURI& result, size_t& pos);
  static bool parse_database_collection(const std::string& uri, MongoURI& result, size_t& pos);
  static bool parse_options(const std::string& uri, MongoURI& result, size_t& pos);
  
  // Helper functions
  static std::string url_decode(const std::string& encoded);
  static std::pair<std::string, std::string> split_key_value(const std::string& kv, char delimiter = '=');
  static std::vector<std::string> split_string(const std::string& str, char delimiter);
  static bool is_valid_identifier(const std::string& str);
};

#endif // MONGODB_URI_PARSER_H
