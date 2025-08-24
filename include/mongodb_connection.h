#ifndef MONGODB_CONNECTION_H
#define MONGODB_CONNECTION_H

/*
  MongoDB Connection Pool Management
  
  Provides thread-safe connection pooling for MongoDB connections
  with automatic connection lifecycle management.
*/

#include "my_global.h"
#include "mongodb_uri_parser.h"
#include <mongoc/mongoc.h>
#include <chrono>
#include <memory>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>

/*
  Connection pool configuration
*/
#define MONGODB_DEFAULT_MAX_CONNECTIONS 10
#define MONGODB_DEFAULT_CONNECTION_TIMEOUT_MS 30000
#define MONGODB_DEFAULT_IDLE_TIMEOUT_SECONDS 300

/*
  Individual connection information in the pool
*/
struct MongoConnectionInfo {
  mongoc_client_t* client;
  std::chrono::steady_clock::time_point last_used;
  std::atomic<bool> in_use;
  std::string connection_string;
  uint64_t connection_id;
  
  MongoConnectionInfo(mongoc_client_t* c, const std::string& uri, uint64_t id)
    : client(c), last_used(std::chrono::steady_clock::now()), 
      in_use(false), connection_string(uri), connection_id(id) {}
};

/*
  Thread-safe MongoDB connection pool
*/
class MongoConnectionPool {
private:
  std::vector<std::unique_ptr<MongoConnectionInfo>> connections;
  std::mutex pool_mutex;
  std::string base_connection_string;
  MongoURI parsed_uri;  // Parsed connection components
  
  // Configuration
  size_t max_connections;
  std::chrono::milliseconds connection_timeout;
  std::chrono::seconds idle_timeout;
  
  // Statistics
  std::atomic<uint64_t> next_connection_id;
  std::atomic<size_t> active_connections;
  std::atomic<size_t> total_connections_created;
  
  // Internal methods
  mongoc_client_t* create_new_connection();
  void cleanup_idle_connections();
  MongoConnectionInfo* find_available_connection();

public:
  MongoConnectionPool(const std::string& connection_string);
  ~MongoConnectionPool();
  
  // Connection management
  mongoc_client_t* acquire_connection();
  void release_connection(mongoc_client_t* client);
  void cleanup();
  
  // Configuration
  void set_max_connections(size_t max_conn);
  void set_connection_timeout(std::chrono::milliseconds timeout);
  void set_idle_timeout(std::chrono::seconds timeout);
  
  // Connection information access
  const MongoURI& get_parsed_uri() const { return parsed_uri; }
  bool is_connection_valid() const { return parsed_uri.is_valid; }
  std::string get_connection_error() const { return parsed_uri.error_message; }
  std::string get_database_name() const { return parsed_uri.database; }
  std::string get_collection_name() const { return parsed_uri.collection; }
  std::string get_safe_connection_string() const { return parsed_uri.to_safe_string(); }
  
  // Statistics and monitoring
  size_t get_active_connections() const { return active_connections.load(); }
  size_t get_total_connections() const { return connections.size(); }
  size_t get_total_created() const { return total_connections_created.load(); }
  
  // Pool health
  bool is_healthy() const;
  void force_reconnect_all();
  
  // Thread safety
  void lock() { pool_mutex.lock(); }
  void unlock() { pool_mutex.unlock(); }
};

/*
  Global connection pool management
*/
extern std::mutex global_pool_mutex;
extern std::map<std::string, std::shared_ptr<MongoConnectionPool>> global_connection_pools;

/*
  Helper functions
*/
MongoConnectionPool* get_or_create_connection_pool(const std::string& connection_string);
void cleanup_all_connection_pools();
bool test_mongodb_connection(const std::string& connection_string);

/*
  Connection string validation and parsing
*/
bool validate_mongodb_connection_string(const std::string& connection_string, std::string& error_message);
MongoURI parse_mongodb_connection_string(const std::string& connection_string);

#endif /* MONGODB_CONNECTION_H */
