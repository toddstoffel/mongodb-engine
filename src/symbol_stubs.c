// Stubs for missing Berkeley DB symbols from static mongo-c-driver
// On macOS, symbols get an automatic underscore prefix, so we need to account for that
// _db_keyword_ becomes __db_keyword_ after the automatic prefix

// Berkeley DB keyword table - create empty stub
const void* _db_keyword_ = 0;

// Berkeley DB assertion function - create no-op stub
void _db_my_assert(const char *failedexpr, const char *file, int line) {
    // No-op: we don't use Berkeley DB functionality
}
