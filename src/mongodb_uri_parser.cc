/*
  MongoDB URI Parser Implementation
  
  Comprehensive parser for MongoDB connection strings supporting
  all standard MongoDB URI formats and connection options.
*/

#include "mongodb_uri_parser.h"
#include "my_global.h"
#include <regex>
#include <sstream>
#include <algorithm>
#include <cctype>

/*
  Convert parsed URI back to connection string for mongo-c-driver
*/
std::string MongoURI::to_connection_string() const {
  if (!is_valid) {
    return "";
  }
  
  std::ostringstream uri;
  
  // Protocol
  uri << (is_srv ? "mongodb+srv://" : "mongodb://");
  
  // Credentials
  if (!username.empty()) {
    uri << username;
    if (!password.empty()) {
      uri << ":" << password;
    }
    uri << "@";
  }
  
  // Hosts
  for (size_t i = 0; i < hosts.size(); ++i) {
    if (i > 0) uri << ",";
    uri << hosts[i].first;
    if (hosts[i].second != 27017) {  // Only include port if not default
      uri << ":" << hosts[i].second;
    }
  }
  
  // Database (collection is not part of standard MongoDB URI)
  if (!database.empty()) {
    uri << "/" << database;
  }
  
  // Options
  std::vector<std::string> option_parts;
  
  if (!auth_source.empty() && auth_source != database) {
    option_parts.push_back("authSource=" + auth_source);
  }
  
  if (!replica_set.empty()) {
    option_parts.push_back("replicaSet=" + replica_set);
  }
  
  if (ssl) {
    option_parts.push_back("ssl=true");
  }
  
  if (connect_timeout_ms != 30000) {
    option_parts.push_back("connectTimeoutMS=" + std::to_string(connect_timeout_ms));
  }
  
  if (socket_timeout_ms != 30000) {
    option_parts.push_back("socketTimeoutMS=" + std::to_string(socket_timeout_ms));
  }
  
  // Add any other custom options
  for (const auto& opt : options) {
    option_parts.push_back(opt.first + "=" + opt.second);
  }
  
  if (!option_parts.empty()) {
    uri << "?";
    for (size_t i = 0; i < option_parts.size(); ++i) {
      if (i > 0) uri << "&";
      uri << option_parts[i];
    }
  }
  
  return uri.str();
}

/*
  Get safe connection string for logging (password masked)
*/
std::string MongoURI::to_safe_string() const {
  if (!is_valid) {
    return "INVALID_URI";
  }
  
  std::ostringstream uri;
  
  // Protocol
  uri << (is_srv ? "mongodb+srv://" : "mongodb://");
  
  // Credentials (mask password)
  if (!username.empty()) {
    uri << username;
    if (!password.empty()) {
      uri << ":***";
    }
    uri << "@";
  }
  
  // Hosts
  for (size_t i = 0; i < hosts.size(); ++i) {
    if (i > 0) uri << ",";
    uri << hosts[i].first;
    if (hosts[i].second != 27017) {
      uri << ":" << hosts[i].second;
    }
  }
  
  // Database and collection
  if (!database.empty()) {
    uri << "/" << database;
    if (!collection.empty()) {
      uri << "/" << collection;
    }
  }
  
  return uri.str();
}

/*
  Main URI parsing function
*/
MongoURI MongoURIParser::parse(const std::string& connection_string) {
  MongoURI result;
  
  if (connection_string.empty()) {
    result.error_message = "Empty connection string";
    return result;
  }
  
  size_t pos = 0;
  
  // Parse protocol
  if (!parse_protocol(connection_string, result, pos)) {
    return result;
  }
  
  // Parse credentials (optional)
  if (!parse_credentials(connection_string, result, pos)) {
    return result;
  }
  
  // Parse hosts
  if (!parse_hosts(connection_string, result, pos)) {
    return result;
  }
  
  // Parse database and collection (optional)
  if (!parse_database_collection(connection_string, result, pos)) {
    return result;
  }
  
  // Parse options (optional)
  if (!parse_options(connection_string, result, pos)) {
    return result;
  }
  
  // Validate the parsed components
  if (result.hosts.empty()) {
    result.error_message = "No hosts specified";
    return result;
  }
  
  if (result.database.empty()) {
    result.error_message = "Database name is required";
    return result;
  }
  
  if (result.collection.empty()) {
    result.error_message = "Collection name is required for storage engine";
    return result;
  }
  
  // All validation passed
  result.is_valid = true;
  return result;
}

/*
  Parse protocol (mongodb:// or mongodb+srv://)
*/
bool MongoURIParser::parse_protocol(const std::string& uri, MongoURI& result, size_t& pos) {
  if (uri.substr(0, 14) == "mongodb+srv://") {
    result.is_srv = true;
    pos = 14;
    return true;
  } else if (uri.substr(0, 10) == "mongodb://") {
    result.is_srv = false;
    pos = 10;
    return true;
  } else {
    result.error_message = "Invalid protocol. Must start with mongodb:// or mongodb+srv://";
    return false;
  }
}

/*
  Parse credentials (username:password@)
*/
bool MongoURIParser::parse_credentials(const std::string& uri, MongoURI& result, size_t& pos) {
  size_t at_pos = uri.find('@', pos);
  if (at_pos == std::string::npos) {
    // No credentials
    return true;
  }
  
  // Check if @ is actually part of credentials (not in hostname)
  size_t slash_pos = uri.find('/', pos);
  size_t question_pos = uri.find('?', pos);
  size_t end_pos = std::min(slash_pos != std::string::npos ? slash_pos : uri.length(),
                           question_pos != std::string::npos ? question_pos : uri.length());
  
  if (at_pos > end_pos) {
    // @ is not part of credentials
    return true;
  }
  
  std::string creds = uri.substr(pos, at_pos - pos);
  pos = at_pos + 1;
  
  size_t colon_pos = creds.find(':');
  if (colon_pos != std::string::npos) {
    result.username = url_decode(creds.substr(0, colon_pos));
    result.password = url_decode(creds.substr(colon_pos + 1));
  } else {
    result.username = url_decode(creds);
  }
  
  return true;
}

/*
  Parse hosts (host:port,host:port,...)
*/
bool MongoURIParser::parse_hosts(const std::string& uri, MongoURI& result, size_t& pos) {
  size_t end_pos = uri.find('/', pos);
  if (end_pos == std::string::npos) {
    end_pos = uri.find('?', pos);
    if (end_pos == std::string::npos) {
      end_pos = uri.length();
    }
  }
  
  std::string hosts_str = uri.substr(pos, end_pos - pos);
  pos = end_pos;
  
  if (hosts_str.empty()) {
    result.error_message = "No hosts specified";
    return false;
  }
  
  std::vector<std::string> host_list = split_string(hosts_str, ',');
  
  for (const std::string& host_port : host_list) {
    size_t colon_pos = host_port.find(':');
    std::string host;
    int port = 27017;
    
    if (colon_pos != std::string::npos) {
      host = host_port.substr(0, colon_pos);
      try {
        port = std::stoi(host_port.substr(colon_pos + 1));
      } catch (const std::exception&) {
        result.error_message = "Invalid port number: " + host_port.substr(colon_pos + 1);
        return false;
      }
    } else {
      host = host_port;
    }
    
    if (!validate_hostname(host)) {
      result.error_message = "Invalid hostname: " + host;
      return false;
    }
    
    if (!validate_port(port)) {
      result.error_message = "Invalid port: " + std::to_string(port);
      return false;
    }
    
    result.hosts.push_back(std::make_pair(host, port));
  }
  
  return true;
}

/*
  Parse database and collection (/database/collection)
*/
bool MongoURIParser::parse_database_collection(const std::string& uri, MongoURI& result, size_t& pos) {
  if (pos >= uri.length() || uri[pos] != '/') {
    return true;  // Optional
  }
  
  pos++;  // Skip '/'
  
  size_t end_pos = uri.find('?', pos);
  if (end_pos == std::string::npos) {
    end_pos = uri.length();
  }
  
  std::string path = uri.substr(pos, end_pos - pos);
  pos = end_pos;
  
  size_t slash_pos = path.find('/');
  if (slash_pos != std::string::npos) {
    result.database = path.substr(0, slash_pos);
    result.collection = path.substr(slash_pos + 1);
  } else {
    result.database = path;
  }
  
  if (!result.database.empty() && !validate_database_name(result.database)) {
    result.error_message = "Invalid database name: " + result.database;
    return false;
  }
  
  if (!result.collection.empty() && !validate_collection_name(result.collection)) {
    result.error_message = "Invalid collection name: " + result.collection;
    return false;
  }
  
  return true;
}

/*
  Parse options (?key=value&key=value)
*/
bool MongoURIParser::parse_options(const std::string& uri, MongoURI& result, size_t& pos) {
  if (pos >= uri.length() || uri[pos] != '?') {
    return true;  // Optional
  }
  
  pos++;  // Skip '?'
  
  std::string options_str = uri.substr(pos);
  std::vector<std::string> option_pairs = split_string(options_str, '&');
  
  for (const std::string& pair : option_pairs) {
    auto kv = split_key_value(pair, '=');
    if (kv.first.empty()) continue;
    
    std::string key = url_decode(kv.first);
    std::string value = url_decode(kv.second);
    
    // Handle known options
    if (key == "authSource") {
      result.auth_source = value;
    } else if (key == "replicaSet") {
      result.replica_set = value;
    } else if (key == "ssl" || key == "tls") {
      result.ssl = (value == "true" || value == "1");
    } else if (key == "connectTimeoutMS") {
      try {
        result.connect_timeout_ms = std::stoi(value);
      } catch (const std::exception&) {
        result.error_message = "Invalid connectTimeoutMS: " + value;
        return false;
      }
    } else if (key == "socketTimeoutMS") {
      try {
        result.socket_timeout_ms = std::stoi(value);
      } catch (const std::exception&) {
        result.error_message = "Invalid socketTimeoutMS: " + value;
        return false;
      }
    } else {
      // Store unknown options for pass-through
      result.options[key] = value;
    }
  }
  
  return true;
}

/*
  Validation functions
*/
bool MongoURIParser::validate_hostname(const std::string& hostname) {
  if (hostname.empty() || hostname.length() > 253) {
    return false;
  }
  
  // Allow localhost, IP addresses, and domain names
  if (hostname == "localhost") {
    return true;
  }
  
  // Simple validation - more comprehensive regex would be better for production
  std::regex hostname_regex(R"(^[a-zA-Z0-9]([a-zA-Z0-9\-\.]{0,251}[a-zA-Z0-9])?$)");
  return std::regex_match(hostname, hostname_regex);
}

bool MongoURIParser::validate_port(int port) {
  return port > 0 && port <= 65535;
}

bool MongoURIParser::validate_database_name(const std::string& database) {
  if (database.empty() || database.length() > 64) {
    return false;
  }
  
  // MongoDB database name restrictions
  const std::string invalid_chars = "/\\. \"$*<>:|?";
  return database.find_first_of(invalid_chars) == std::string::npos;
}

bool MongoURIParser::validate_collection_name(const std::string& collection) {
  if (collection.empty() || collection.length() > 120) {
    return false;
  }
  
  // MongoDB collection name restrictions
  if (collection[0] == '$' || collection.find('\0') != std::string::npos) {
    return false;
  }
  
  return true;
}

/*
  Helper functions
*/
std::string MongoURIParser::url_decode(const std::string& encoded) {
  std::string decoded;
  for (size_t i = 0; i < encoded.length(); ++i) {
    if (encoded[i] == '%' && i + 2 < encoded.length()) {
      int hex_val;
      std::istringstream(encoded.substr(i + 1, 2)) >> std::hex >> hex_val;
      decoded += static_cast<char>(hex_val);
      i += 2;
    } else if (encoded[i] == '+') {
      decoded += ' ';
    } else {
      decoded += encoded[i];
    }
  }
  return decoded;
}

std::pair<std::string, std::string> MongoURIParser::split_key_value(const std::string& kv, char delimiter) {
  size_t pos = kv.find(delimiter);
  if (pos != std::string::npos) {
    return std::make_pair(kv.substr(0, pos), kv.substr(pos + 1));
  } else {
    return std::make_pair(kv, "");
  }
}

std::vector<std::string> MongoURIParser::split_string(const std::string& str, char delimiter) {
  std::vector<std::string> result;
  std::stringstream ss(str);
  std::string item;
  
  while (std::getline(ss, item, delimiter)) {
    if (!item.empty()) {
      result.push_back(item);
    }
  }
  
  return result;
}

bool MongoURIParser::is_valid_identifier(const std::string& str) {
  if (str.empty()) return false;
  
  for (char c : str) {
    if (!std::isalnum(c) && c != '_') {
      return false;
    }
  }
  
  return true;
}
