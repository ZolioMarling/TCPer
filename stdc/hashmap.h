#ifndef _HASHMAP_H_
#define _HASHMAP_H_


#if !defined(__OS_WINDOWS__) && !defined(__OS_UNIX__)
    #if defined(_WIN32) || defined(__WIN32__) || defined(__WINDOWS__)
        #define __OS_WINDOWS__
    #elif defined(__linux__) || defined(__APPLE__) || defined(__MACH__)
        #define __OS_UNIX__
        #ifndef _GNU_SOURCE
            #define _GNU_SOURCE
        #endif
    #else
        #error "Unsupported platforms."
    #endif
#endif


#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>


#define True 1
#define False 0
#define intptr(x) ((intptr_t)(x))
#define uintptr(x) ((uintptr_t)(x))
#define int32ptr(x) ((int32_t)(intptr_t)(x))
#define int64ptr(x) ((long long)(intptr_t)(x))
#define uint32ptr(x) ((uint32_t)(uintptr_t)(x))
#define uint64ptr(x) ((unsigned long long)(uintptr_t)(x))
typedef int bool;
typedef size_t usize;
typedef ptrdiff_t isize;
typedef uint8_t byte;
typedef int8_t int8;
typedef int16_t int16;
typedef int32_t int32;
typedef long long int64;
typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef unsigned long long uint64;
typedef float float32;
typedef double float64;


#define HASHMAP_INIT_BUCKETS 8  // The value of `HASHMAP_INIT_BUCKETS` must be the power of `2`.
#define HASHMAP_LOAD_FACTOR 0.75
#define HASHMAP_SHRINK_FACTOR 0.25


/**
 * @brief The operator `(1 ? (x) : (x))` can automatically convert `char` or `short` to `int`.
 * @param x The macro input.
 * @return The specific function signature.
**/
#define HASHMAP_GENERIC_VAR(x) _Generic((1 ? (x) : (x)), \
    int: __hashmap_var_int__, \
    long: __hashmap_var_long__, \
    long long: __hashmap_var_long_long__, \
    unsigned int: __hashmap_var_unsigned_int__, \
    unsigned long: __hashmap_var_unsigned_long__, \
    unsigned long long: __hashmap_var_unsigned_long_long__, \
    float: __hashmap_var_float__, \
    double: __hashmap_var_double__, \
    char *: __hashmap_var_string__, \
    const char *: __hashmap_var_const_string__, \
    void *: __hashmap_var_pointer__, \
    const void *: __hashmap_var_const_pointer__, \
    default: __hashmap_var_const_pointer__ \
)((x))


/**
 * @brief Put a key-value pair into the `HashMap` dictionary.
 * @param dict The `HashMap` dictionary.
 * @param key The key (not allowed to be `NULL`).
 * @param value The value (if all the element values are `NULL`, it will be decayed to a `set`).
 * @return `1` for success, `0` for failure.
**/
#define hashmap_add(dict, key, value) __hashmap_add__(dict, HASHMAP_GENERIC_VAR(key), HASHMAP_GENERIC_VAR(value))


/**
 * @brief Get the value in the `HashMap` dictionary by key.
 * @param dict The `HashMap` dictionary.
 * @param key The key (not allowed to be `NULL`).
 * @return The `HashMapVariant` pointer of value (`NULL` for failure).
**/
#define hashmap_get(dict, key) __hashmap_get__(dict, HASHMAP_GENERIC_VAR(key))


/**
 * @brief Delete a key-value pair from the `HashMap` dictionary.
 * @param dict The `HashMap` dictionary.
 * @param key The key (not allowed to be `NULL`).
 * @return `1` for success, `0` for failure.
**/
#define hashmap_del(dict, key) __hashmap_remove__(dict, HASHMAP_GENERIC_VAR(key))


/**
 * @brief Determine whether an element exists.
 * @param dict The `HashMap` dictionary.
 * @param key The key (not allowed to be `NULL`).
 * @return `1` for existence, `0` for nonexistence.
**/
#define hashmap_contains(dict, key) __hashmap_contains__(dict, HASHMAP_GENERIC_VAR(key))


/**
 * @brief Iterate over all key-value pairs in the `HashMap` dictionary.
 * @param dict The `HashMap` dictionary.
 * @param k The pointer of `HashMapVariant` key.
 * @param v The pointer of `HashMapVariant` value.
**/
#define hashmap_iter(dict, k, v) \
    for (uint64 i_##k = 0; (dict) && i_##k < (dict)->bucket; i_##k++) \
        for (_HashMapNode *node_##k = (dict)->table[i_##k]; node_##k && (((k) = &node_##k->key), ((v) = &node_##k->value), 1); node_##k = node_##k->next)


typedef enum {
    _HASHMAP_NULL,
    _HASHMAP_STRING,
    _HASHMAP_POINTER,
    _HASHMAP_INT32,
    _HASHMAP_INT64,
    _HASHMAP_UINT32,
    _HASHMAP_UINT64,
    _HASHMAP_FLOAT,
    _HASHMAP_DOUBLE
} _HashMapDataType;


typedef struct HashMapVariant {
    _HashMapDataType type;
    union {
        int32 i;
        int64 l;
        uint32 ui;
        uint64 ul;
        float32 f;
        float64 d;
        char *str;
        void *ptr;
    } as;
} HashMapVariant;


typedef struct _HashMapNode {
    uint64 hash;
    HashMapVariant key;
    HashMapVariant value;
    struct _HashMapNode *next;
} _HashMapNode;


typedef struct HashMap {
    uint64 seed;
    uint64 count;
    uint64 bucket;
    _HashMapNode **table;
} HashMap;


/**
 * @brief Create a new `HashMap` dictionary.
 * @return The pointer to the new `HashMap` dictionary.
**/
HashMap *hashmap_create(void);


/**
 * @brief Free the memory of `HashMap` dictionary.
 * @param dict The `HashMap` dictionary.
**/
void hashmap_destroy(HashMap *dict);


/**
 * @brief View the data structure of the `HashMap` dictionary.
 * @param dict The `HashMap` dictionary.
**/
void hashmap_print_view(HashMap *dict);


/**
 * @brief Print the pairs in the `HashMap`.
 * @param dict The `HashMap` dictionary.
**/
void hashmap_print_pairs(HashMap *dict);


/**
 * @brief Print the content of `HashMapVariant`.
 * @param x The `HashMapVariant` input.
**/
void hashmap_print_variant(HashMapVariant *x);


/**
 * @brief Clear all key-value pairs in the `HashMap` dictionary and reset it to initial state. It's just a simple shallow release, because the ownership of `_HASHMAP_POINTER` heap memory belongs to the user. It is necessary to prevent `_HASHMAP_POINTER` heap memory from leaking.
 * @param dict The `HashMap` dictionary.
**/
void hashmap_clear(HashMap *dict);


/**
 * @brief Get the number of elements (key-value pairs) in the `HashMap` dictionary.
 * @param dict The `HashMap` dictionary.
 * @return The count of elements (`0` for `NULL` dictionary).
**/
uint64 hashmap_count(HashMap *dict);


/**
 * @brief The wrapper of implementation of `add` function.
 * @param dict The `HashMap` dictionary.
 * @param key The `HashMapVariant` key (not allowed to be `NULL`).
 * @param value The `HashMapVariant` value.
 * @return `1` for success, `0` for failure.
**/
bool __hashmap_add__(HashMap *dict, HashMapVariant key, HashMapVariant value);


/**
 * @brief The wrapper of implementation of `get` function.
 * @param dict The `HashMap` dictionary.
 * @param key The `HashMapVariant` key (not allowed to be `NULL`).
 * @return The `HashMapVariant` pointer of value (`NULL` for failure).
**/
HashMapVariant *__hashmap_get__(HashMap *dict, HashMapVariant key);


/**
 * @brief The wrapper of implementation of `contains` function.
 * @param dict The `HashMap` dictionary.
 * @param key The `HashMapVariant` key (not allowed to be `NULL`).
 * @return `1` for existence, `0` for nonexistence.
**/
bool __hashmap_contains__(HashMap *dict, HashMapVariant key);


/**
 * @brief The wrapper of implementation of `remove` function.
 * @param dict The `HashMap` dictionary.
 * @param key The `HashMapVariant` key (not allowed to be `NULL`).
 * @return `1` for success, `0` for failure.
**/
bool __hashmap_remove__(HashMap *dict, HashMapVariant key);


/**
 * @brief Print `HashMapVariant` type to terminal.
 * @param x The `HashMapVariant` input.
**/
void __hashmap_print__(HashMapVariant x);


/**
 * @brief Calculate the hash value of `HashMapVariant` (note: deep hashing is not automatically supported for `struct` containing pointer heap memory fields.).
 * @param x The `HashMapVariant` input.
 * @param seed The random seed.
 * @return The hash value.
**/
uint64 __hashmap_hash__(HashMapVariant x, uint64 seed);


/**
 * @brief Shallow equality check without reflection.
 * @param x The `HashMapVariant` x.
 * @param y The `HashMapVariant` y.
 * @return `1` for `True`, `0` for `False`.
**/
bool __hashmap_variant_equals__(HashMapVariant x, HashMapVariant y);


/**
 * @brief Copy the `HashMapVariant` string to heap memory. The ownership of `_HASHMAP_POINTER` memory is belonging to user, so the function just copies shallowly.
 * @param src The `HashMapVariant` source.
 * @return The copied `HashMapVariant` destination.
**/
HashMapVariant __hashmap_variant_copy__(HashMapVariant src);


/**
 * @brief Clean up the heap memory of `_HASHMAP_STRING` in `_HashMapNode`. The ownership of `_HASHMAP_POINTER` memory is belonging to user, so user must release and free the heap `_HASHMAP_POINTER` memory mutually.
 * @param x The `HashMapVariant` input.
**/
void __hashmap_variant_cleanup__(HashMapVariant x);


/**
 * @brief Resize the count of `HashMap` bucket, including expanding and shrinking.
 * @param dict The `HashMap` dictionary.
 * @param n The new count of `HashMap` bucket.
 * @return `1` for success, `0` for failure.
**/
bool __hashmap_resize__(HashMap *dict, uint64 n);


/**
 * @brief Get the total memory size of the `HashMap` dictionary (note: the ownership of `_HASHMAP_POINTER` memory belongs to the user and is not included in the statistics).
 * @param dict The `HashMap` dictionary.
 * @return The total bytes.
**/
uint64 __hashmap_sizeof__(HashMap *dict);


/**
 * @brief Obtain the next power of `base` that is greater than or equal to `n`.
 * @param n The value.
 * @param base The base.
 * @return The next power.
**/
uint64 __next_power_base__(uint64 n, uint64 base);


/**
 * @brief Use the FNV-1a function to calculate the hash value.
 * @param data The raw data.
 * @param length The length of data.
 * @param seed The random seed.
 * @return The hash value.
**/
uint64 __hash_fnv1a__(void *data, usize length, uint64 seed);


/**
 * @brief Convert a given byte count into a human-readable string format.
 * @param x The raw size in bytes to be converted.
 * @param buffer The destination character array where the formatted string will be stored.
 * @param size The maximum number of bytes to write to the buffer (including the null terminator).
**/
void __convert_unit__(uint64 x, char *buffer, int size);


static inline HashMapVariant __hashmap_var_int__(int x) {
    return (HashMapVariant) {
        .type = _HASHMAP_INT32,
        .as.i = (int32)(x)
    };
}


static inline HashMapVariant __hashmap_var_long_long__(long long x) {
    return (HashMapVariant) {
        .type = _HASHMAP_INT64,
        .as.l = (int64)(x)
    };
}


static inline HashMapVariant __hashmap_var_unsigned_int__(unsigned int x) {
    return (HashMapVariant) {
        .type = _HASHMAP_UINT32,
        .as.ui = (uint32)(x)
    };
}


static inline HashMapVariant __hashmap_var_long__(long x) {
    #if defined(__OS_UNIX__)
        return (HashMapVariant) {
            .type = _HASHMAP_INT64,
            .as.l = (int64)(x)
        };
    #elif defined(__OS_WINDOWS__)
        return (HashMapVariant) {
            .type = _HASHMAP_INT32,
            .as.i = (int32)(x)
        };
    #endif
}


static inline HashMapVariant __hashmap_var_unsigned_long__(unsigned long x) {
    #if defined(__OS_UNIX__)
        return (HashMapVariant) {
            .type = _HASHMAP_UINT64,
            .as.ul = (uint64)(x)
        };
    #elif defined(__OS_WINDOWS__)
        return (HashMapVariant) {
            .type = _HASHMAP_UINT32,
            .as.ui = (uint32)(x)
        };
    #endif
}


static inline HashMapVariant __hashmap_var_unsigned_long_long__(unsigned long long x) {
    return (HashMapVariant) {
        .type = _HASHMAP_UINT64,
        .as.ul = (uint64)(x)
    };
}


static inline HashMapVariant __hashmap_var_float__(float x) {
    return (HashMapVariant) {
        .type = _HASHMAP_FLOAT,
        .as.f = (float32)(x)
    };
}


static inline HashMapVariant __hashmap_var_double__(double x) {
    return (HashMapVariant) {
        .type = _HASHMAP_DOUBLE,
        .as.d = (float64)(x)
    };
}


static inline HashMapVariant __hashmap_var_string__(char *x) {
    return (HashMapVariant) {
        .type = _HASHMAP_STRING,
        .as.str = x
    };
}


static inline HashMapVariant __hashmap_var_const_string__(const char *x) {
    return (HashMapVariant) {
        .type = _HASHMAP_STRING,
        .as.str = (char *)x
    };
}


static inline HashMapVariant __hashmap_var_pointer__(void *x) {
    return (HashMapVariant) {
        .type = _HASHMAP_POINTER,
        .as.ptr = x
    };
}


static inline HashMapVariant __hashmap_var_const_pointer__(const void *x) {
    return (HashMapVariant) {
        .type = _HASHMAP_POINTER,
        .as.ptr = (void *)x
    };
}


#endif
