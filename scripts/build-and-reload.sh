#!/bin/bash

# MongoDB Storage Engine - Build and Reload Script
# This script provides a consistent workflow for building, installing, and testing the plugin

set -e  # Exit on any error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
PROJECT_ROOT="/Users/toddstoffel/Documents/git/mongodb-engine"
BUILD_DIR="${PROJECT_ROOT}/build"
MARIADB_USER="toddstoffel"
TEST_DB="test_mongodb"

echo -e "${BLUE}🔧 MongoDB Storage Engine - Build and Reload${NC}"
echo "=================================================="

# Step 1: Build the plugin
echo -e "${YELLOW}📦 Step 1: Building plugin...${NC}"
cd "${BUILD_DIR}"
make -j4
if [ $? -eq 0 ]; then
    echo -e "${GREEN}✅ Build successful${NC}"
else
    echo -e "${RED}❌ Build failed${NC}"
    exit 1
fi

# Step 2: Get plugin directory
echo -e "${YELLOW}📁 Step 2: Discovering plugin directory...${NC}"
PLUGIN_DIR=$(mariadb-config --plugindir 2>/dev/null || echo "/opt/homebrew/lib/mariadb/plugin")
echo "Plugin directory: ${PLUGIN_DIR}"

# Step 3: Stop MariaDB to ensure clean reload
echo -e "${YELLOW}🛑 Step 3: Stopping MariaDB...${NC}"
brew services stop mariadb
sleep 2

# Step 4: Install the plugin file
echo -e "${YELLOW}📋 Step 4: Installing plugin file...${NC}"
cp ha_mongodb.so "${PLUGIN_DIR}/"
if [ $? -eq 0 ]; then
    echo -e "${GREEN}✅ Plugin file installed to ${PLUGIN_DIR}${NC}"
else
    echo -e "${RED}❌ Failed to install plugin file${NC}"
    exit 1
fi

# Step 5: Start MariaDB
echo -e "${YELLOW}🚀 Step 5: Starting MariaDB...${NC}"
brew services start mariadb
echo "Waiting for MariaDB to start..."
sleep 5

# Step 6: Wait for MariaDB to be ready
echo -e "${YELLOW}⏳ Step 6: Waiting for MariaDB to be ready...${NC}"
for i in {1..30}; do
    if mariadb -u "${MARIADB_USER}" -e "SELECT 1;" >/dev/null 2>&1; then
        echo -e "${GREEN}✅ MariaDB is ready${NC}"
        break
    fi
    echo "Attempt $i/30: MariaDB not ready yet..."
    sleep 1
done

# Step 7: Install the plugin in MariaDB
echo -e "${YELLOW}🔌 Step 7: Installing MongoDB storage engine plugin...${NC}"
mariadb -u "${MARIADB_USER}" -e "
-- Uninstall existing plugin if present
SET sql_mode = '';
SELECT IF(COUNT(*) > 0, 'UNINSTALL SONAME ''ha_mongodb'';', 'SELECT ''Plugin not installed'';') as cmd
FROM information_schema.plugins WHERE plugin_name = 'MONGODB'
INTO @sql;
PREPARE stmt FROM @sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

-- Install the plugin
INSTALL SONAME 'ha_mongodb';
"

if [ $? -eq 0 ]; then
    echo -e "${GREEN}✅ Plugin installed in MariaDB${NC}"
else
    echo -e "${RED}❌ Failed to install plugin in MariaDB${NC}"
    exit 1
fi

# Step 8: Verify plugin installation
echo -e "${YELLOW}🔍 Step 8: Verifying plugin installation...${NC}"
echo "Checking SHOW ENGINES:"
mariadb -u "${MARIADB_USER}" -e "SHOW ENGINES;" | grep -i mongodb || echo "❌ MongoDB engine not found in SHOW ENGINES"

echo ""
echo "Checking SHOW PLUGINS:"
mariadb -u "${MARIADB_USER}" -e "SHOW PLUGINS;" | grep -i mongodb || echo "❌ MongoDB plugin not found in SHOW PLUGINS"

# Step 9: Prepare test environment
echo -e "${YELLOW}🧪 Step 9: Preparing test environment...${NC}"
mariadb -u "${MARIADB_USER}" -e "
CREATE DATABASE IF NOT EXISTS ${TEST_DB};
USE ${TEST_DB};
DROP TABLE IF EXISTS customers;
"

# Step 10: Create test table
echo -e "${YELLOW}📊 Step 10: Creating test table...${NC}"
mariadb -u "${MARIADB_USER}" -e "
USE ${TEST_DB};
CREATE TABLE customers (
    customerNumber INT PRIMARY KEY,
    customerName VARCHAR(255),
    phone VARCHAR(50),
    city VARCHAR(100)
) ENGINE=MONGODB 
CONNECTION='mongodb://tom:jerry@holly.local:27017/classicmodels/customers';
"

if [ $? -eq 0 ]; then
    echo -e "${GREEN}✅ Test table created successfully${NC}"
else
    echo -e "${RED}❌ Failed to create test table${NC}"
    exit 1
fi

# Step 11: Test basic operations
echo -e "${YELLOW}🎯 Step 11: Testing basic operations...${NC}"

echo "Testing DESCRIBE table:"
mariadb -u "${MARIADB_USER}" -e "USE ${TEST_DB}; DESCRIBE customers;" 2>&1 | head -10

echo ""
echo "Testing SELECT query:"
mariadb -u "${MARIADB_USER}" -e "USE ${TEST_DB}; SELECT * FROM customers LIMIT 1;" 2>&1 | head -10

echo ""
echo -e "${BLUE}=================================================="
echo -e "🏁 Build and reload complete!"
echo -e "📊 Test database: ${TEST_DB}"
echo -e "📋 Test table: customers"
echo -e "🔗 Connection: mongodb://tom:jerry@holly.local:27017/classicmodels/customers"
echo -e "=================================================="${NC}

echo ""
echo "To run additional tests:"
echo "  mariadb -u ${MARIADB_USER} -e \"USE ${TEST_DB}; SELECT * FROM customers LIMIT 5;\""
echo ""
echo "To check plugin status:"
echo "  mariadb -u ${MARIADB_USER} -e \"SHOW ENGINES;\" | grep -i mongodb"
echo "  mariadb -u ${MARIADB_USER} -e \"SHOW PLUGINS;\" | grep -i mongodb"
