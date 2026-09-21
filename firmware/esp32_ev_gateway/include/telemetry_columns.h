#pragma once
#include <stddef.h>
#include <string.h>

// Split our flat JSON object into a key array and a value array without
// rounding/reformatting any value. Quoted strings and nested arrays/objects
// stay byte-for-byte intact. The server validates JSON and row lengths.
inline bool telemetryColumns(const char* json, char* keys, size_t keySize,
                             char* values, size_t valueSize) {
    size_t k = 0, v = 0;
    auto append = [](char* out, size_t size, size_t& used, const char* p, size_t n) {
        if (used + n + 1 > size) return false;
        memcpy(out + used, p, n); used += n; out[used] = '\0'; return true;
    };
    auto space = [](const char*& p) { while (*p==' ' || *p=='\n' || *p=='\r' || *p=='\t') ++p; };
    const char* p = json;
    space(p);
    if (*p++ != '{' || !append(keys,keySize,k,"[",1) || !append(values,valueSize,v,"[",1)) return false;
    bool first = true;
    for (;;) {
        space(p);
        if (*p == '}' && first) { ++p; break; }
        const char* key = p;
        if (*p++ != '"') return false;
        while (*p && *p != '"') { if (*p == '\\') { ++p; if (!*p) return false; } ++p; }
        if (*p++ != '"') return false;
        const size_t keyLength = p - key;
        space(p);
        if (*p++ != ':') return false;
        space(p);
        const char* value = p;
        int depth = 0; bool quoted = false;
        for (; *p; ++p) {
            if (quoted) {
                if (*p == '\\') { ++p; if (!*p) return false; }
                else if (*p == '"') quoted = false;
            } else if (*p == '"') quoted = true;
            else if (*p == '[' || *p == '{') ++depth;
            else if (*p == '}' && depth == 0) break;
            else if (*p == ']' || *p == '}') { if (--depth < 0) return false; }
            else if (*p == ',' && depth == 0) break;
        }
        if (!*p || quoted || depth || p == value) return false;
        if ((!first && (!append(keys,keySize,k,",",1) || !append(values,valueSize,v,",",1))) ||
            !append(keys,keySize,k,key,keyLength) || !append(values,valueSize,v,value,p-value)) return false;
        first = false;
        if (*p++ == '}') break;
    }
    space(p);
    return !*p && append(keys,keySize,k,"]",1) && append(values,valueSize,v,"]",1);
}
