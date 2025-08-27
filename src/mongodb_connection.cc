/*
  MongoDB Connection Pool Implementation
  
  Provides thread-safe connection pooling for MongoDB connections.
*/

#include "mongodb_connection.h"
#include "my_global.h"
// Skip problematic sql_class.h for now

// Global connection pool storage
std::mutex global_pool_mutex;
std::map<std::string, std::shared_ptr<MongoConnectionPool>> global_connection_pools;

/*
  MongoConnectionPool implementation
*/
MongoConnectionPool::MongoConnectionPool(const std::string& connection_string)
  : base_connection_string(connection_string),
    max_connections(MONGODB_DEFAULT_MAX_CONNECTIONS),
    connection_timeout(std::chrono::milliseconds(MONGODB_DEFAULT_CONNECTION_TIMEOUT_MS)),
    idle_timeout(std::chrono::seconds(MONGODB_DEFAULT_IDLE_TIMEOUT_SECONDS)),
    next_connection_id(1),
    active_connections(0),
    total_connections_created(0)
{
  // Parse and validate the connection string
  parsed_uri = MongoURIParser::parse(connection_string);
  
  if (!parsed_uri.is_valid) {
  }
  
  // Reserve space for expected connections
  connections.reserve(max_connections);
}

MongoConnectionPool::~MongoConnectionPool()
{
  cleanup();
}

mongoc_client_t* MongoConnectionPool::acquire_connection()
{
  std::lock_guard<std::mutex> lock(pool_mutex);
  
  // Clean up idle connections first
  cleanup_idle_connections();
  
  // Try to find an available connection
  MongoConnectionInfo* conn_info = find_available_connection();
  if (conn_info)
  {
    conn_info->in_use = true;
    conn_info->last_used = std::chrono::steady_clock::now();
    active_connections++;
    return conn_info->client;
  }
  
  // Create new connection if under limit
  if (connections.size() < max_connections)
  {
    mongoc_client_t* new_client = create_new_connection();
    if (new_client)
    {
      auto conn_info = std::make_unique<MongoConnectionInfo>(
        new_client, base_connection_string, next_connection_id++);
      conn_info->in_use = true;
      connections.push_back(std::move(conn_info));
      
      active_connections++;
      total_connections_created++;
      return new_client;
    }
  }
  
  // No available connections
  return nullptr;
}

void MongoConnectionPool::release_connection(mongoc_client_t* client)
{
  if (!client) return;
  
  std::lock_guard<std::mutex> lock(pool_mutex);
  
  for (auto& conn_info : connections)
  {
    if (conn_info->client == client && conn_info->in_use.load())
    {
      conn_info->in_use = false;
      conn_info->last_used = std::chrono::steady_clock::now();
      active_connections--;
      break;
    }
  }
}

mongoc_client_t* MongoConnectionPool::create_new_connection()
{
  // Ensure we have a valid parsed URI
  if (!parsed_uri.is_valid) {
    return nullptr;
  }
  
  // Get the connection string for mongo-c-driver (without collection)
  std::string mongo_connection_string = parsed_uri.to_connection_string();
  
  mongoc_uri_t* uri = mongoc_uri_new(mongo_connection_string.c_str());
  if (!uri)
  {
    return nullptr;
  }
  
  mongoc_client_t* client = mongoc_client_new_from_uri(uri);
  mongoc_uri_destroy(uri);
  
  // Test the connection by pinging the target database
  if (client)
  {
    bson_error_t error;
    bson_t* ping = BCON_NEW("ping", BCON_INT32(1));
    
    // Use the specific database from our parsed URI
    const char* database_name = parsed_uri.database.empty() ? "admin" : parsed_uri.database.c_str();
    bool success = mongoc_client_command_simple(client, database_name, ping, nullptr, nullptr, &error);
    bson_destroy(ping);
    
    if (!success)
    {
      //                   database_name, error.message);
      mongoc_client_destroy(client);
      return nullptr;
    }
    
    // Additional test: verify the collection exists
    mongoc_database_t* database = mongoc_client_get_database(client, database_name);
    if (database) {
      mongoc_collection_t* collection = mongoc_database_get_collection(database, parsed_uri.collection.c_str());
      if (collection) {
        // Just verify we can access the collection metadata
        bson_error_t coll_error;
        bson_t* stats = BCON_NEW("collStats", BCON_UTF8(parsed_uri.collection.c_str()));
        bool coll_exists = mongoc_database_command_simple(database, stats, nullptr, nullptr, &coll_error);
        bson_destroy(stats);
        
        if (!coll_exists) {
          // Collection might not exist, but that's okay - it could be created later
          // Just log a warning for now
          //                   parsed_uri.collection.c_str(), database_name);
        }
        
        mongoc_collection_destroy(collection);
      }
      mongoc_database_destroy(database);
    }
  }
  
  return client;
}

void MongoConnectionPool::cleanup_idle_connections()
{
  auto now = std::chrono::steady_clock::now();
  
  connections.erase(
    std::remove_if(connections.begin(), connections.end(),
      [&](const std::unique_ptr<MongoConnectionInfo>& conn_info) {
        if (!conn_info->in_use.load())
        {
          auto idle_time = std::chrono::duration_cast<std::chrono::seconds>(
            now - conn_info->last_used);
          if (idle_time > idle_timeout)
          {
            mongoc_client_destroy(conn_info->client);
            return true;
          }
        }
        return false;
      }),
    connections.end());
}

MongoConnectionInfo* MongoConnectionPool::find_available_connection()
{
  for (auto& conn_info : connections)
  {
    if (!conn_info->in_use.load())
    {
      return conn_info.get();
    }
  }
  return nullptr;
}

void MongoConnectionPool::cleanup()
{
  std::lock_guard<std::mutex> lock(pool_mutex);
  
  for (auto& conn_info : connections)
  {
    if (conn_info->client)
    {
      mongoc_client_destroy(conn_info->client);
    }
  }
  connections.clear();
  active_connections = 0;
}

bool MongoConnectionPool::is_healthy() const
{
  return connections.size() <= max_connections && active_connections <= connections.size();
}

void MongoConnectionPool::set_max_connections(size_t max_conn)
{
  std::lock_guard<std::mutex> lock(pool_mutex);
  max_connections = max_conn;
}

void MongoConnectionPool::set_connection_timeout(std::chrono::milliseconds timeout)
{
  connection_timeout = timeout;
}

void MongoConnectionPool::set_idle_timeout(std::chrono::seconds timeout)
{
  idle_timeout = timeout;
}

/*
  Global helper functions
*/
MongoConnectionPool* get_or_create_connection_pool(const std::string& connection_string)
{
  std::lock_guard<std::mutex> lock(global_pool_mutex);
  
  auto it = global_connection_pools.find(connection_string);
  if (it != global_connection_pools.end())
  {
    return it->second.get();
  }
  
  auto pool = std::make_shared<MongoConnectionPool>(connection_string);
  global_connection_pools[connection_string] = pool;
  return pool.get();
}

void cleanup_all_connection_pools()
{
  std::lock_guard<std::mutex> lock(global_pool_mutex);
  global_connection_pools.clear();
}

bool test_mongodb_connection(const std::string& connection_string)
{
  mongoc_uri_t* uri = mongoc_uri_new(connection_string.c_str());
  if (!uri)
  {
    return false;
  }
  
  mongoc_client_t* client = mongoc_client_new_from_uri(uri);
  mongoc_uri_destroy(uri);
  
  if (!client)
  {
    return false;
  }
  
  bson_error_t error;
  bson_t* ping = BCON_NEW("ping", BCON_INT32(1));
  bool success = mongoc_client_command_simple(client, "admin", ping, nullptr, nullptr, &error);
  
  bson_destroy(ping);
  mongoc_client_destroy(client);
  
  return success;
}

/*
  Validate MongoDB connection string using our URI parser
*/
bool validate_mongodb_connection_string(const std::string& connection_string, std::string& error_message) {
  MongoURI parsed = MongoURIParser::parse(connection_string);
  
  if (!parsed.is_valid) {
    error_message = parsed.error_message;
    return false;
  }
  
  // Additional validation for storage engine requirements
  if (parsed.database.empty()) {
    error_message = "Database name is required in connection string";
    return false;
  }
  
  if (parsed.collection.empty()) {
    error_message = "Collection name is required in connection string (format: mongodb://host/database/collection)";
    return false;
  }
  
  error_message = "";
  return true;
}

/*
  Parse MongoDB connection string and return parsed components
*/
MongoURI parse_mongodb_connection_string(const std::string& connection_string) {
  return MongoURIParser::parse(connection_string);
}
