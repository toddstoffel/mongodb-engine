# MongoDB Storage Engine for MariaDB

A professional storage engine that enables seamless SQL queries on MongoDB collections through MariaDB's storage engine interface with advanced query optimization.

## 🎯 Current Status: Phase 2 Complete - Core Functionality Operational

**Major Milestone Achieved (August 2025)**: All fundamental storage engine operations are working correctly with verified accuracy and performance benefits.

### ✅ **Fully Working Features**

- **Plugin Registration**: Loads successfully as `MONGODB` engine in MariaDB 11.x+
- **Table Operations**: CREATE TABLE, SELECT, scanning, and row counting all operational
- **Condition Pushdown**: WHERE clauses automatically translated to MongoDB server-side filters
- **Authentication**: MongoDB connections with username/password and authSource parameter
- **Data Accuracy**: All queries return correct results from MongoDB collections
- **Query Isolation**: Proper condition cleanup prevents filter persistence between queries
- **Error Handling**: Comprehensive error reporting and connection management
- **Cross-Platform**: Clean build with zero compilation warnings

### 🎯 **Recent Critical Fixes**

- **✅ Fixed Condition Persistence Bug**: Resolved issue where WHERE clause filters persisted between queries
- **✅ Fixed COUNT(*) Accuracy**: All COUNT operations now return correct results (was stuck at 3, now properly returns 121)
- **✅ Fixed Operation 46 Misidentification**: Correctly identified `HA_EXTRA_DETACH_CHILDREN` vs COUNT detection
- **✅ Fixed Table Scanning**: Queries now return diverse data instead of filtered subsets

### ⚠️ **Current Limitations**

- **COUNT Operations**: Return correct results but NOT optimized - still fetch all documents instead of using MongoDB native count
- **Performance**: COUNT queries process all matching documents rather than using efficient `countDocuments()` calls

## Features

- **✅ Condition Pushdown**: WHERE clauses automatically translated to MongoDB server-side filters for optimal performance
- **✅ SQL Table Operations**: Full table scanning, row-by-row access, and document-to-row conversion  
- **✅ MongoDB Integration**: Native connection handling with authentication and error management
- **✅ Data Integrity**: Accurate query results with proper condition isolation between operations
- **⚠️ COUNT Operations**: Functionally correct but NOT performance-optimized (fetches all documents)
- **Cross-Engine Joins**: Join MongoDB data with traditional SQL tables (InnoDB, MyISAM, etc.) - *planned*
- **Dynamic Schema Mapping**: Automatic schema inference from MongoDB's flexible document structure - *planned*
- **Advanced Query Translation**: Complex WHERE conditions (AND, OR, comparisons) - *in development*
- **Connection Pooling**: Efficient connection management with automatic reconnection - *planned*

## Quick Start

### Prerequisites

- MariaDB 11.x+ with development headers
- MongoDB C Driver (included in sources)
- CMake 3.15+
- C++17 compatible compiler

### Build and Install

```bash
# Clone the repository
git clone https://github.com/toddstoffel/mongodb-engine.git
cd mongodb-engine

# Build the plugin
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .

# Install plugin to MariaDB
cmake --install . --component plugin
```

### Usage

```sql
-- Install the storage engine
INSTALL SONAME 'ha_mongodb';

-- Create a table mapping to MongoDB collection
CREATE TABLE customers (
  _id VARCHAR(24) PRIMARY KEY,
  customerName VARCHAR(255),
  phone VARCHAR(50),
  city VARCHAR(100)
) ENGINE=MONGODB 
CONNECTION='mongodb://localhost:27017/classicmodels/customers';

-- Query MongoDB data using SQL
SELECT customerName, city 
FROM customers 
WHERE city = 'Boston'
ORDER BY customerName;

-- Cross-engine joins
SELECT c.customerName, COUNT(o.orderNumber) as order_count
FROM customers c
LEFT JOIN mysql_orders o ON c._id = o.customer_id
GROUP BY c._id;
```

## Verified Test Results (August 2025)

### ✅ **Core Operations - All Passing**

| Test Case | Expected | Actual | Status |
|-----------|----------|---------|--------|
| `SELECT COUNT(*) FROM customers` | 121 | 121 | ✅ PASS |
| `SELECT COUNT(*) WHERE city = 'Paris'` | 3 | 3 | ✅ PASS |
| `SELECT * FROM customers LIMIT 5` | Diverse cities | Las Vegas, Madrid, Luleå, etc. | ✅ PASS |
| `SELECT customerName FROM customers WHERE city = 'Paris'` | 3 Paris records | 3 correct Paris records | ✅ PASS |
| Condition isolation between queries | No filter persistence | ✅ No persistence | ✅ PASS |

### ✅ **Advanced Features - Working**

- **Condition Pushdown**: WHERE clauses translated to MongoDB `{ "city" : "Paris" }` filters
- **Authentication**: Connects to MongoDB with `authSource=admin` parameter  
- **Error Handling**: Proper connection error reporting and cleanup
- **Plugin Management**: Clean install/uninstall with MariaDB plugin system

### 🎯 **Performance Status**

- **Correctness**: ✅ 100% accurate results across all test cases
- **Condition Pushdown**: ✅ Working - server-side filtering operational  
- **COUNT Optimization**: ❌ NOT implemented - COUNT queries fetch all documents instead of using MongoDB `countDocuments()`

### ⚠️ **Performance Note**

While all COUNT operations return **correct results**, they are currently **not optimized**:

- **Current Behavior**: COUNT queries fetch and process all matching documents
- **Performance Impact**: Inefficient for large collections (processes 121 documents for simple COUNT(*))
- **Expected Optimization**: Should use MongoDB `countDocuments()` calls with zero document fetching
- **Evidence**: Debug logs show `CONVERT_SIMPLE_FIELD` calls during COUNT operations

## Current Status

### ✅ **Phase 2 Complete - Core Functionality Working**

The MongoDB Storage Engine has reached a major milestone with **full basic functionality**:

#### **Working Features (Verified August 2025)**

- **✅ Plugin Installation**: Loads as `MONGODB` engine in MariaDB 11.x+
- **✅ Table Creation**: `CREATE TABLE ... ENGINE=MONGODB CONNECTION='...'`
- **✅ Basic Queries**: `SELECT` statements with simple `WHERE` conditions
- **✅ Condition Pushdown**: Server-side filtering in MongoDB (performance optimized)
- **✅ Authentication**: MongoDB connection with username/password and authSource
- **✅ Document Conversion**: MongoDB BSON documents → MariaDB table rows

#### **Performance Benefits Achieved**

- **Server-side filtering**: `WHERE city = 'Paris'` executes in MongoDB, not MariaDB
- **Reduced data transfer**: Only matching documents transferred over network
- **Native MongoDB indexing**: Queries leverage existing MongoDB indexes

#### **Example Working Queries**

```sql
-- Simple condition pushdown (server-side filtering)
SELECT customerName FROM customers WHERE city = 'Paris';

-- Table scanning 
SELECT COUNT(*) FROM customers;

-- Field projection
SELECT customerName, city FROM customers LIMIT 10;
```

## Development Status

**Current Phase**: Phase 3 - Advanced Features Implementation

### Phase 1: Foundation - ✅ COMPLETED

- [x] Plugin registration and initialization
- [x] Basic handler class with MariaDB interface compliance
- [x] Connection management using libmongoc
- [x] Table creation with ENGINE=MONGODB syntax

### Phase 2: Core Functionality - ✅ COMPLETED

- [x] **Condition Pushdown**: WHERE clauses translated to MongoDB server-side filters
- [x] URI parsing and connection configuration
- [x] Dynamic document-to-row conversion
- [x] Basic table scanning operations
- [x] Error handling and logging integration

### Phase 3: Advanced Features - 🚧 IN PROGRESS

- [ ] Complex WHERE conditions (AND, OR, NOT, IN, etc.)
- [ ] Index operations and key-based lookups
- [ ] ORDER BY and LIMIT optimization
- [ ] Aggregation pipeline generation

### Phase 4: Production Features - 📋 PLANNED

- [ ] Cross-engine JOIN optimization
- [ ] Advanced connection pooling
- [ ] Performance monitoring and metrics
- [ ] Schema evolution handling

## Architecture

The storage engine follows a layered architecture:

```text
MariaDB SQL Layer
        ↓
Storage Engine Interface (ha_mongodb)
        ↓
MongoDB Abstraction Layer
        ↓
Connection & Resource Management
        ↓
MongoDB Database
```

## Contributing

We welcome contributions! Please see [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines on:

- Development environment setup
- Coding standards and best practices
- Testing requirements
- Pull request process

## System Requirements

### Supported Platforms

- Linux (Ubuntu 20.04+, CentOS 8+, etc.)
- macOS (10.15+)
- Windows (with MSYS2/MinGW or Visual Studio 2017+)

### Dependencies

- **MariaDB**: 11.x+ (for storage engine API compatibility)
- **MongoDB**: 4.4+ (for connection and query features)
- **CMake**: 3.15+ (for cross-platform builds)
- **Compiler**: GCC 7+, Clang 6+, or MSVC 2017+

## Configuration

### CONNECTION String Format

```text
mongodb://[username:password@]host[:port]/database/collection[?options]
```

### Examples

```sql
-- Local MongoDB
CONNECTION='mongodb://localhost:27017/myapp/users'

-- With authentication
CONNECTION='mongodb://user:pass@mongo.example.com:27017/production/orders'

-- MongoDB Atlas
CONNECTION='mongodb+srv://user:pass@cluster0.mongodb.net/ecommerce/products'
```

## Troubleshooting

### Common Issues

**Plugin not loading**: Ensure correct naming (`ha_mongodb.so`) and plugin directory permissions.

**Connection failures**: Verify MongoDB URI format and network connectivity.

**Schema mismatches**: Check field mappings and use explicit type definitions.

### Getting Help

- **Issues**: Report bugs and request features on GitHub Issues
- **Discussions**: Ask questions in GitHub Discussions
- **Documentation**: Check `.github/copilot-instructions.md` for detailed development guidance

## License

This project is licensed under the GPL-2.0 License - see the MariaDB licensing model for details.

## Acknowledgments

- **MariaDB Foundation** - For the excellent storage engine framework
- **MongoDB Inc.** - For the robust C driver and database engine
- **Contributors** - Thank you to everyone who helps make this project better

---

**Status**: Active Development | **Version**: 1.0.0-beta | **Last Updated**: August 2025
