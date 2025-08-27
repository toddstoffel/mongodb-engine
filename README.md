# MongoDB Storage Engine for MariaDB

A professional storage engine that enables seamless SQL queries on MongoDB collections through MariaDB's storage engine interface with advanced query optimization.

## Features

- **✅ Condition Pushdown**: WHERE clauses are automatically translated to MongoDB filters for server-side query optimization
- **SQL-to-MongoDB Translation**: Query MongoDB collections using standard SQL syntax
- **Cross-Engine Joins**: Join MongoDB data with traditional SQL tables (InnoDB, MyISAM, etc.)
- **Dynamic Schema Mapping**: Automatic schema inference from MongoDB's flexible document structure
- **Connection Pooling**: Efficient connection management with automatic reconnection
- **Aggregation Pushdown**: Push complex queries to MongoDB for optimal performance (planned)

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
