# Contributing to MongoDB Storage Engine for MariaDB

Thank you for your interest in contributing to the MongoDB Storage Engine! This document provides guidelines and information for contributors.

## Getting Started

### Development Environment Setup

1. **Fork the repository**

   ```bash
   git clone https://github.com/toddstoffel/mongodb-engine.git
   cd mongodb-engine
   ```

2. **Install dependencies**

   - MariaDB 11.x+ development headers
   - MongoDB C Driver (libmongoc) - included in sources
   - CMake 3.15+
   - C++17 compatible compiler

3. **Build the project**

   ```bash
   mkdir build && cd build
   cmake .. -DCMAKE_BUILD_TYPE=Debug
   make
   ```

## Development Workflow

### Branch Strategy

- `main` - Stable releases
- `develop` - Active development
- `feature/*` - New features
- `bugfix/*` - Bug fixes
- `hotfix/*` - Critical production fixes

### Making Changes

1. **Create a feature branch**

   ```bash
   git checkout -b feature/your-feature-name
   ```

2. **Make your changes**

   - Follow the coding standards below
   - Add tests for new functionality
   - Update documentation as needed

3. **Test your changes**

   ```bash
   cd build
   make test
   ```

4. **Commit and push**

   ```bash
   git add .
   git commit -m "feat: add your feature description"
   git push origin feature/your-feature-name
   ```

5. **Create a Pull Request**

   - Use the PR template
   - Include clear description of changes
   - Reference any related issues

## Development Phases

### Current Phase: Phase 2 - Core Functionality

**Priority Areas for Contribution:**

- Document scanning and row conversion
- Basic query translation (WHERE, ORDER BY, LIMIT)
- Schema inference from MongoDB collections
- Index operations implementation

### Phase 3: Advanced Features

- Complex query translation
- Aggregation pipeline optimization
- Cross-engine join support

### Phase 4: Production Features

- Performance monitoring
- Advanced error handling
- Comprehensive testing

## Coding Standards

### C++ Guidelines

- **Standard**: C++17
- **Style**: Follow MariaDB coding conventions
- **Naming**:
  - Classes: `PascalCase` (e.g., `MongoConnectionPool`)
  - Functions: `snake_case` (e.g., `parse_connection_string`)
  - Variables: `snake_case` (e.g., `connection_timeout`)
  - Constants: `UPPER_SNAKE_CASE` (e.g., `MONGODB_DEFAULT_TIMEOUT`)

### Code Structure

```cpp
/*
  Brief description of the file/class
  
  Detailed description if needed.
*/

#include "system_headers.h"
#include "project_headers.h"

// Class definition
class MongoExample {
private:
  // Private members first
  std::string connection_string;
  
public:
  // Constructors
  MongoExample(const std::string& conn_str);
  
  // Public methods
  bool connect();
  void disconnect();
};
```

### Error Handling

- Use MariaDB error codes and patterns
- Provide detailed error messages
- Clean up resources properly
- Log errors appropriately

```cpp
int ha_mongodb::example_method() {
  DBUG_ENTER("ha_mongodb::example_method");
  
  if (!validate_input()) {
    // Log error and return appropriate code
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  }
  
  DBUG_RETURN(0);
}
```

### Memory Management

- Use RAII principles
- Prefer smart pointers for dynamic allocation
- Clean up MongoDB C driver resources properly
- Use MariaDB memory allocation functions when needed

## Testing Guidelines

### Unit Tests

- Test individual components in isolation
- Use descriptive test names
- Cover both success and failure cases
- Mock external dependencies

### Integration Tests

- Test complete workflows
- Use real MongoDB connections when possible
- Test cross-engine operations
- Verify error handling

### Test Structure

```cpp
// Test file: test_connection_pool.cc
#include "gtest/gtest.h"
#include "mongodb_connection.h"

class MongoConnectionPoolTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Setup test environment
  }
  
  void TearDown() override {
    // Cleanup
  }
};

TEST_F(MongoConnectionPoolTest, CreateValidConnection) {
  // Test implementation
}
```

## Documentation

### Code Documentation

- Document all public APIs
- Use Doxygen-style comments
- Include usage examples
- Document complex algorithms

```cpp
/**
 * Parse MongoDB connection string and validate components
 * 
 * @param connection_string MongoDB URI in standard format
 * @return MongoURI parsed components or error details
 * 
 * @example
 * MongoURI uri = MongoURIParser::parse("mongodb://localhost:27017/db/coll");
 * if (uri.is_valid) {
 *   // Use parsed components
 * }
 */
static MongoURI parse(const std::string& connection_string);
```

### User Documentation

- Update README.md for user-facing changes
- Add examples for new features
- Document configuration options
- Update troubleshooting guides

## Issue Reporting

### Bug Reports

Include:

- MariaDB version
- MongoDB version
- Operating system
- Steps to reproduce
- Expected vs actual behavior
- Error messages/logs

### Feature Requests

Include:

- Use case description
- Proposed solution
- Alternative approaches considered
- Impact on existing functionality

## Code Review Process

### Pull Request Guidelines

- **Title**: Use conventional commit format
  - `feat:` - New features
  - `fix:` - Bug fixes
  - `docs:` - Documentation updates
  - `test:` - Test additions/updates
  - `refactor:` - Code refactoring

- **Description**:
  - Clear summary of changes
  - Motivation and context
  - Breaking changes (if any)
  - Testing performed

### Review Criteria

- Code quality and standards compliance
- Test coverage
- Documentation updates
- Performance impact
- Security considerations
- Backward compatibility

## Security

### Reporting Security Issues

- **DO NOT** open public issues for security vulnerabilities
- Email security issues to security contact
- Include detailed reproduction steps
- Allow time for assessment and fix

### Security Guidelines

- Validate all input data
- Sanitize connection strings for logging
- Use secure connection options by default
- Handle authentication failures gracefully

## Getting Help

### Communication Channels

- **GitHub Issues**: Bug reports and feature requests
- **GitHub Discussions**: General questions and ideas
- **Pull Requests**: Code review and collaboration

### Development Resources

- MariaDB Storage Engine Documentation
- MongoDB C Driver Documentation
- Project Architecture (docs/architecture.md)
- Development Setup (docs/development.md)

## Recognition

Contributors will be:

- Listed in the project's CONTRIBUTORS.md
- Mentioned in release notes
- Invited to join the core team (for significant contributions)

Thank you for contributing to the MongoDB Storage Engine! Together, we're building bridges between SQL and NoSQL worlds.
