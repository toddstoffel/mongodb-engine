#!/bin/bash

# MongoDB Storage Engine - Quick Test Script
# Test the plugin without rebuilding

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

MARIADB_USER="toddstoffel"
TEST_DB="test_mongodb"

echo -e "${BLUE}🧪 MongoDB Storage Engine - Quick Test${NC}"
echo "========================================"

# Test 1: Check plugin status
echo -e "${YELLOW}🔍 Test 1: Plugin Status${NC}"
echo "SHOW ENGINES:"
mariadb -u "${MARIADB_USER}" -e "SHOW ENGINES;" | grep -i mongodb && echo -e "${GREEN}✅ Engine available${NC}" || echo -e "${RED}❌ Engine not available${NC}"

echo ""
echo "SHOW PLUGINS:"
mariadb -u "${MARIADB_USER}" -e "SHOW PLUGINS;" | grep -i mongodb && echo -e "${GREEN}✅ Plugin loaded${NC}" || echo -e "${RED}❌ Plugin not loaded${NC}"

# Test 2: Check table existence
echo ""
echo -e "${YELLOW}📊 Test 2: Table Status${NC}"
mariadb -u "${MARIADB_USER}" -e "USE ${TEST_DB}; SHOW TABLES;" 2>/dev/null | grep -i customers && echo -e "${GREEN}✅ Test table exists${NC}" || echo -e "${RED}❌ Test table missing${NC}"

# Test 3: Test DESCRIBE
echo ""
echo -e "${YELLOW}📋 Test 3: DESCRIBE Table${NC}"
if mariadb -u "${MARIADB_USER}" -e "USE ${TEST_DB}; DESCRIBE customers;" 2>/dev/null; then
    echo -e "${GREEN}✅ DESCRIBE successful${NC}"
else
    echo -e "${RED}❌ DESCRIBE failed${NC}"
    mariadb -u "${MARIADB_USER}" -e "USE ${TEST_DB}; DESCRIBE customers;" 2>&1 | head -3
fi

# Test 4: Test SELECT
echo ""
echo -e "${YELLOW}🎯 Test 4: SELECT Query${NC}"
if mariadb -u "${MARIADB_USER}" -e "USE ${TEST_DB}; SELECT * FROM customers LIMIT 1;" 2>/dev/null; then
    echo -e "${GREEN}✅ SELECT successful${NC}"
else
    echo -e "${RED}❌ SELECT failed${NC}"
    mariadb -u "${MARIADB_USER}" -e "USE ${TEST_DB}; SELECT * FROM customers LIMIT 1;" 2>&1 | head -3
fi

# Test 5: Connection test
echo ""
echo -e "${YELLOW}🔗 Test 5: MongoDB Connection Test${NC}"
if ping -c 1 holly.local >/dev/null 2>&1; then
    echo -e "${GREEN}✅ MongoDB server holly.local is reachable${NC}"
else
    echo -e "${RED}❌ MongoDB server holly.local is not reachable${NC}"
fi

echo ""
echo -e "${BLUE}========================================"
echo -e "🏁 Quick test complete!${NC}"
