# MongoDB Flexible Schema Handling Strategy

## The Challenge

MongoDB collections can contain documents with vastly different field structures:

```javascript
// Document 1: Basic customer
{
  "_id": ObjectId("..."),
  "customerNumber": 103,
  "customerName": "Atelier graphique",
  "phone": "40.32.2555",
  "city": "Nantes",
  "country": "France"
}

// Document 2: Customer with additional fields
{
  "_id": ObjectId("..."),
  "customerNumber": 112,
  "customerName": "Signal Gift Stores",
  "contactName": "King",           // New field
  "phone": "7025551838",
  "address": {                     // Nested structure
    "street": "8489 Strong St.",
    "city": "Las Vegas",
    "state": "Nevada",
    "zipCode": "83030"
  },
  "country": "USA",
  "tags": ["electronics", "gifts"], // Array field
  "isActive": true                  // Boolean field
}

// Document 3: Legacy customer with different naming
{
  "_id": ObjectId("..."),
  "customer_number": 141,          // snake_case instead of camelCase
  "customer_name": "Euro+ Shopping Channel",
  "phone_number": "(91) 555 94 44", // Different field name
  "city": "Madrid",
  "country": "Spain",
  "revenue": "227600.00"           // String number instead of numeric
}
```

## Our Multi-Level Solution

### 1. **Virtual Column Approach with Graceful Degradation**

```sql
CREATE TABLE customers_flexible (
  _id VARCHAR(24) PRIMARY KEY,
  document JSON,                    -- Full document always available
  
  -- Core fields (common across most documents)
  customerNumber INT,
  customerName VARCHAR(255),
  phone VARCHAR(50),
  city VARCHAR(100),
  country VARCHAR(100),
  creditLimit DECIMAL(10,2),
  
  -- Extended fields (may be NULL for many documents)
  contactName VARCHAR(255),         -- Optional field
  isActive BOOLEAN,                 -- Type conversion from various formats
  tags JSON,                        -- Array/complex data as JSON
  
  -- Nested fields using dot notation paths
  address_street VARCHAR(255),      -- Extracted from address.street
  address_city VARCHAR(100),        -- Extracted from address.city
  address_state VARCHAR(50),        -- Extracted from address.state
  address_zipCode VARCHAR(20)       -- Extracted from address.zipCode
  
) ENGINE=MONGODB 
CONNECTION='mongodb://tom:jerry@holly.local:27017/classicmodels/customers?ssl=false';
```

### 2. **Field Resolution Strategy**

Our enhanced storage engine handles missing/different fields through:

#### A. **Field Name Variations**
- `customerNumber` → tries `customer_number`, `customernumber`, `CUSTOMERNUMBER`
- `contactName` → tries `contact_name`, `contactname`, `contact`

#### B. **Type Coercion**
- String numbers → Numeric types: `"227600.00"` → `227600.00`
- Numeric → String: `103` → `"103"`
- Boolean variations: `true`, `"true"`, `1`, `"yes"` → `TRUE`

#### C. **Nested Field Extraction**
- `address.street` → Extracts from nested documents
- `customer.contact.email` → Multi-level nesting support

#### D. **Missing Field Handling**
- Fields not present in documents → `NULL` values
- Maintains SQL semantics for missing data

### 3. **Query Examples with Flexible Schema**

```sql
-- Query works even with inconsistent field presence
SELECT 
  customerNumber,
  customerName,
  COALESCE(address_city, city) as customer_city,  -- Fallback logic
  country,
  CASE 
    WHEN isActive IS NULL THEN 'Unknown'
    WHEN isActive = 1 THEN 'Active'
    ELSE 'Inactive'
  END as status
FROM customers_flexible 
WHERE country IN ('USA', 'France')
ORDER BY customerNumber;

-- JSON extraction for fields not in table schema
SELECT 
  customerName,
  country,
  JSON_EXTRACT(document, '$.specialNotes') as notes,
  JSON_EXTRACT(document, '$.lastOrderDate') as last_order,
  JSON_EXTRACT(tags, '$[0]') as primary_tag
FROM customers_flexible 
WHERE JSON_EXTRACT(document, '$.vipCustomer') = true;

-- Cross-engine joins still work
SELECT 
  c.customerName,
  c.country,
  COUNT(o.order_id) as order_count,
  AVG(o.total_amount) as avg_order
FROM customers_flexible c
LEFT JOIN mysql_orders o ON c.customerNumber = o.customer_id
WHERE c.creditLimit > 50000
GROUP BY c.customerNumber, c.customerName, c.country
HAVING order_count > 3;
```

### 4. **Implementation Architecture**

```cpp
// Enhanced field conversion with fallbacks
int ha_mongodb::convert_document_to_row(const bson_t *doc, uchar *buf)
{
  // 1. Initialize all fields to NULL
  memset(buf, 0, table->s->reclength);
  for (Field **field = table->field; *field; field++) {
    (*field)->set_null();
    (*field)->ptr = buf + (*field)->offset(table->record[0]);
  }
  
  // 2. Convert each field with flexible handling
  for (Field **field = table->field; *field; field++) {
    const char *field_name = (*field)->field_name.str;
    
    // Try direct field match first
    if (!convert_simple_field_from_document(doc, *field, field_name)) {
      continue; // Success
    }
    
    // Try field variations (camelCase ↔ snake_case)
    if (!try_field_variations(doc, *field, field_name)) {
      continue; // Success
    }
    
    // Try nested field extraction (address.city)
    if (!handle_nested_field_extraction(doc, *field, field_name)) {
      continue; // Success
    }
    
    // Field remains NULL - this is normal for flexible schema
  }
  
  return 0;
}

// Type-aware conversion with coercion
int ha_mongodb::convert_bson_value_with_coercion(bson_iter_t *iter, Field *field)
{
  bson_type_t bson_type = bson_iter_type(iter);
  enum_field_types sql_type = field->type();
  
  switch (bson_type) {
    case BSON_TYPE_UTF8:
      if (sql_type == MYSQL_TYPE_LONG) {
        // Try to convert string to number
        const char *str_val = bson_iter_utf8(iter, nullptr);
        if (is_numeric_string(str_val)) {
          field->store(atoi(str_val));
          return 0;
        }
      }
      // Fall through to store as string
      break;
      
    case BSON_TYPE_INT32:
      if (sql_type == MYSQL_TYPE_VARCHAR) {
        // Convert number to string
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", bson_iter_int32(iter));
        field->store(buf, strlen(buf), &my_charset_latin1);
        return 0;
      }
      break;
  }
  
  // Standard conversion...
}
```

### 5. **Schema Evolution Support**

The system tracks schema changes over time:

```cpp
struct SchemaEvolution {
  time_t last_analysis;
  std::map<std::string, FieldStatistics> field_distribution;
  std::vector<std::string> newly_discovered_fields;
  std::vector<std::string> deprecated_fields;
};

// Periodic schema analysis
void analyze_collection_schema(mongoc_collection_t *collection) {
  // Sample documents to understand field distribution
  // Track new fields that appear
  // Detect type changes over time
  // Recommend schema updates
}
```

### 6. **Benefits of This Approach**

1. **SQL Compatibility**: Standard SQL queries work on MongoDB data
2. **Performance**: Common fields are directly accessible without JSON parsing
3. **Flexibility**: New fields don't break existing queries
4. **Evolution**: Schema can adapt to changing MongoDB structures
5. **Interoperability**: Cross-engine joins with traditional SQL tables
6. **Graceful Degradation**: Missing fields become NULL, not errors

### 7. **Real-World Example**

```sql
-- This query works regardless of document variations:
SELECT 
  COALESCE(customerNumber, customer_number) as id,
  COALESCE(customerName, customer_name) as name,
  COALESCE(phone, phone_number, phoneNumber) as contact,
  country,
  -- Use JSON for fields that vary too much to pre-define
  JSON_EXTRACT(document, '$.lastLoginDate') as last_login,
  JSON_EXTRACT(document, '$.preferences.language') as language
FROM customers_flexible 
WHERE country = 'USA'
  AND (creditLimit > 50000 OR JSON_EXTRACT(document, '$.vipStatus') = true)
ORDER BY name;
```

This approach provides the best of both worlds: structured SQL access to common fields with flexible JSON access to variable fields, all while handling MongoDB's natural schema flexibility gracefully.
