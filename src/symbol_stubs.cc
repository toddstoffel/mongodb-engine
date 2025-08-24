// Stubs for missing Berkeley DB symbols from static mongo-c-driver
// These symbols are expected but not needed for our MongoDB functionality
extern "C" {
    // Berkeley DB keyword table - create empty stub (2 underscores!)
    const void* __db_keyword_ = nullptr;
    
    // Berkeley DB assertion function - create no-op stub (2 underscores!)
    void __db_my_assert(const char *failedexpr, const char *file, int line) {
        // No-op: we don't use Berkeley DB functionality
    }
}
