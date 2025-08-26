# Phase 2 — MongoDB Storage Engine: Status Summary

This document records the current stop-point in Phase 2: what has been implemented, what remains, and prioritized next steps with references to key files.

## Checklist (what to read/verify)
- [x] Plugin loads and basic table creation works
- [x] MongoDB connection and collection access via libmongoc
- [x] BSON → MariaDB row conversion with `_id` handling and `document` JSON
- [x] CONNECT-style position handling (`position()` / `rnd_pos()`) and `scan_position` tracking
- [x] ORDER BY, ORDER BY + LIMIT, and ORDER BY DESC validated via external sorting


## Quick status
Phase 2 progress: ~60% complete.


## What we completed (high level)

- Plugin infrastructure: handler registration, build target, plugin install flow (see `src/ha_mongodb.cc`, CMake artifacts).

- Connection handling: MongoDB URI parsing and connection via libmongoc (`src/ha_mongodb_handler.cc`, `src/mongodb_connection.*`).

- Document-to-row conversion: robust BSON → MariaDB row conversion including `_id` extraction and storing the full document JSON (`src/ha_mongodb_handler.cc`).

- Position-based access: implemented CONNECT-style `position()` and `rnd_pos()` using `my_store_ptr` / `my_get_ptr` and `scan_position` tracking; `rnd_next()` increments scan position.

- ORDER BY behavior: validated end-to-end for ordering and LIMITs by relying on MariaDB external sorting combined with the engine's position functions.


## Remaining Phase 2 tasks (prioritized)

1. Condition pushdown (HIGH)

   - Implement `cond_push()` translation from MariaDB `Item` trees to MongoDB BSON match stages.
   - Create or extend a `SQLToMongoTranslator` to produce BSON match documents and simple aggregation stages.
   - Files to edit/add: `src/ha_mongodb_handler.cc` (cond_push wiring), `src/mongodb_translator.cc` + `include/mongodb_translator.h`.

2. Index operations (HIGH)

   - Implement real `index_init`, `index_read_map`, `index_next`, and `index_end` to use MongoDB indexes for key lookups.
   - Integrate index usage with the translator so key-based queries are pushed to MongoDB.
   - Files to edit/add: `src/ha_mongodb_handler.cc`, `src/mongodb_index.cc` (optional).

3. Schema inference & registry (MEDIUM)

   - Implement `MongoSchemaRegistry` to infer fields, support dot-notation mapping, and map BSON types to SQL types.
   - Files: `src/mongodb_schema.cc`, `include/mongodb_schema.h`.

4. SQL → Mongo translator (MEDIUM)

   - Complete a translator for WHERE, ORDER BY and LIMIT to enable safe pushdown or decide when post-filtering is required.
   - Files: `src/mongodb_translator.cc`.

5. Enhanced statistics & optimizer integration (LOW/MEDIUM)

   - Improve `info()` and expose status vars so the optimizer can make better decisions.
   - Files: `src/ha_mongodb_handler.cc`.

6. Tests & CI (MEDIUM)

   - Add unit tests for translator and schema inference and small integration tests that exercise pushdown and index paths.


## Suggested next concrete steps (short-term)

- Implement a minimal `SQLToMongoTranslator::translateCondition()` that handles equality and basic range comparisons, wire it into `cond_push()`, and add a quick integration test for `WHERE field = value`.

- Implement `index_init()` to accept a simple key and create a MongoDB cursor with an appropriate filter; validate `index_read_map()` returns the first matching row.

- Add 2–3 small tests (script or gtest) verifying equality pushdown and index-based lookup behavior.


## How to validate locally (quick commands)

Build plugin:

```bash
cmake --build build
```

Copy plugin and restart MariaDB (example dev flow used in this repo):

```bash
cp build/ha_mongodb.so "$(mariadb --help --verbose 2>/dev/null | grep -i plugindir | awk '{print $2}')/ha_mongodb.so"
brew services restart mariadb
sleep 10
mariadb -u mongodb -ptestpassword -e "INSTALL SONAME 'ha_mongodb';"
```

Sanity queries:

```bash
mariadb -u mongodb -ptestpassword --protocol=TCP -e "USE test; SELECT COUNT(*) FROM customers;"
mariadb -u mongodb -ptestpassword --protocol=TCP -e "USE test; SELECT customerNumber, customerName FROM customers ORDER BY customerNumber LIMIT 5;"
```


## Files of interest (quick pointer)

- `src/ha_mongodb_handler.cc` — core handler, position logic, conversion helpers, `cond_push` stub
- `include/ha_mongodb.h` — handler class declaration and flags
- `src/mongodb_connection.*` — connection utilities
- `src/mongodb_translator.*` — translator (to add)
- `src/mongodb_schema.*` — schema registry (to add)
- `sources/` — reference server and drivers (do not modify)


## Closing note
The engine is functional and integrated with MariaDB. The highest value work remaining is implementing condition pushdown and index support so queries can be executed efficiently on MongoDB instead of being post-filtered by MariaDB. If you want, I can start implementing the minimal equality pushdown and a small test harness next — I’ll wire it into `cond_push()` and add a regression test.
