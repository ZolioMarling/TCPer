#include "hashmap.h"


HashMap *hashmap_create(void) {
    HashMap *dict = malloc(sizeof(*dict));
    if (!dict) return NULL;

    uint64 buckets = __next_power_base__(HASHMAP_INIT_BUCKETS, 2);

    if (!buckets) {
        fprintf(stderr, "\x1b[1;31m1. the base might be less than one.\x1b[0m\n");
        fprintf(stderr, "\x1b[1;31m2. an uint64_t overflow might occur.\x1b[0m\n");
        free(dict);
        return NULL;
    }

    dict->table = calloc(buckets, sizeof(_HashMapNode *));
    if (!dict->table) {
        free(dict);
        return NULL;
    }

    dict->bucket = buckets;
    dict->count = 0;
    dict->seed = (uint64)time(NULL) ^ uint64ptr(dict);
    return dict;
}


void hashmap_destroy(HashMap *dict) {
    if (!dict) return;
    for (uint64 i = 0; i < dict->bucket; i++) {
        _HashMapNode *node = dict->table[i];
        while (node) {
            _HashMapNode *temp = node;
            node = node->next;
            __hashmap_variant_cleanup__(temp->key);
            __hashmap_variant_cleanup__(temp->value);
            free(temp);
        }
    }
    free(dict->table);
    free(dict);
}


void hashmap_print_view(HashMap *dict) {
    if (!dict) return;
    if (dict->count == 0) {
        printf("HashMap = {}\n");
        return;
    }

    int align_width = snprintf(NULL, 0, "%llu", dict->bucket - 1);

    for (uint64 i = 0; i < dict->bucket; i++) {
        if (dict->bucket > 128 && i == 64) {
            printf("[%.*s] = ...............................\n", align_width, "................................");
            i = dict->bucket - 64 - 1;
            continue;
        }

        printf("[\x1b[1;37m%*llu\x1b[0m] = ", align_width, i);
        _HashMapNode *node = dict->table[i];
        if (!node) {
            printf("(null)\n");
            continue;
        }
        while (node) {
            printf("{");
            __hashmap_print__(node->key);
            printf(": ");
            __hashmap_print__(node->value);
            printf("} -> ");
            node = node->next;
        }
        printf("(null)\n");
    }

    char buffer[16];
    uint64 total = __hashmap_sizeof__(dict);
    uint64 table_size = dict->bucket * sizeof(_HashMapNode *);
    uint64 nodes_size = dict->count * sizeof(_HashMapNode);
    uint64 string_size = total - sizeof(HashMap) - table_size - nodes_size;

    __convert_unit__(total, buffer, sizeof(buffer));

    printf("\nBuckets = \x1b[1;37m%llu\x1b[0m | Count = \x1b[1;37m%llu\x1b[0m | Load = \x1b[1;37m%.3f%%\x1b[0m | Memory Usage\n", dict->bucket, dict->count, 100.0F * dict->count / dict->bucket);
    printf("HashMap Struct = \x1b[1;37m%zu B\x1b[0m\n", sizeof(HashMap));
    printf("Bucket Table = \x1b[1;37m%llu B\x1b[0m (%llu buckets x %zu B)\n", table_size, dict->bucket, sizeof(_HashMapNode *));
    printf("Node = \x1b[1;37m%llu B\x1b[0m (%llu nodes x %zu B)\n", nodes_size, dict->count, sizeof(_HashMapNode));
    printf("String Heap = \x1b[1;37m%llu B\x1b[0m\n", string_size);
    printf("Total = \x1b[1;37m%llu B\x1b[0m (\x1b[1;37m%s\x1b[0m)\n", total, buffer);
}


void hashmap_print_pairs(HashMap *dict) {
    if (!dict) return;
    if (dict->count == 0) {
        printf("{}\n");
        return;
    }
    HashMapVariant *key;
    HashMapVariant *value;
    uint64 count = 0;
    printf("{");
    hashmap_iter(dict, key, value) {
        __hashmap_print__(*key);
        printf(": ");
        __hashmap_print__(*value);
        if (count + 1 != dict->count) printf(", ");
        count++;
    }
    printf("}\n");
}


void hashmap_print_variant(HashMapVariant *x) {
    if (x == NULL) {
        printf("\x1b[1;31mnull\x1b[0m\n");
        return;
    }
    __hashmap_print__(*x);
    printf("\n");
}


void hashmap_clear(HashMap *dict) {
    if (!dict) return;

    for (uint64 i = 0; i < dict->bucket; i++) {
        _HashMapNode *node = dict->table[i];
        while (node) {
            _HashMapNode *temp = node;
            node = node->next;

            __hashmap_variant_cleanup__(temp->key);
            __hashmap_variant_cleanup__(temp->value);
            free(temp);
        }
        dict->table[i] = NULL;
    }
    dict->count = 0ULL;

    if (dict->bucket > __next_power_base__(HASHMAP_INIT_BUCKETS, 2)) {
        _HashMapNode **table = calloc(__next_power_base__(HASHMAP_INIT_BUCKETS, 2), sizeof(_HashMapNode *));
        if (table) {
            free(dict->table);
            dict->table = table;
            dict->bucket = __next_power_base__(HASHMAP_INIT_BUCKETS, 2);
        } else fprintf(stderr, "\x1b[1;33mWarning: HashMap shrinking failed, keeping current buckets.\x1b[0m\n");
    }
}


uint64 hashmap_count(HashMap *dict) {
    if (!dict) return 0ULL;
    return dict->count;
}


bool __hashmap_add__(HashMap *dict, HashMapVariant key, HashMapVariant value) {
    if (!dict || key.type == _HASHMAP_NULL) return False;
    if (key.type == _HASHMAP_STRING && !key.as.str) return False;
    if (key.type == _HASHMAP_POINTER && !key.as.ptr) return False;

    uint64 hash = __hashmap_hash__(key, dict->seed);
    uint64 idx = hash & (dict->bucket - 1);

    for (_HashMapNode *node = dict->table[idx]; node; node = node->next) {
        if (__hashmap_variant_equals__(node->key, key)) {
            HashMapVariant var = __hashmap_variant_copy__(value);
            if (value.type == _HASHMAP_STRING && value.as.str && var.type == _HASHMAP_NULL) return False;
            __hashmap_variant_cleanup__(node->value);
            node->value = var;
            return True;
        }
    }

    if (dict->count >= HASHMAP_LOAD_FACTOR * dict->bucket) {
        if (!__hashmap_resize__(dict, 2 * dict->bucket)) {
            fprintf(stderr, "\x1b[1;33mWarning: HashMap expanding failed, degenerating into a single linked list.\x1b[0m\n");
        }
    }

    HashMapVariant key_copy = __hashmap_variant_copy__(key);
    if (key.type == _HASHMAP_STRING && key.as.str && key_copy.type == _HASHMAP_NULL) return False;
    HashMapVariant value_copy = __hashmap_variant_copy__(value);
    if (value.type == _HASHMAP_STRING && value.as.str && value_copy.type == _HASHMAP_NULL) {
        __hashmap_variant_cleanup__(key_copy);
        return False;
    }

    _HashMapNode *node = malloc(sizeof(*node));
    if (!node) {
        __hashmap_variant_cleanup__(key_copy);
        __hashmap_variant_cleanup__(value_copy);
        return False;
    }

    idx = hash & (dict->bucket - 1);
    node->hash = hash;
    node->key = key_copy;
    node->value = value_copy;
    node->next = dict->table[idx];
    dict->table[idx] = node;
    dict->count++;
    return True;
}


HashMapVariant *__hashmap_get__(HashMap *dict, HashMapVariant key) {
    if (!dict || dict->count == 0 || key.type == _HASHMAP_NULL) return NULL;
    if (key.type == _HASHMAP_STRING && !key.as.str) return NULL;
    if (key.type == _HASHMAP_POINTER && !key.as.ptr) return NULL;

    _HashMapNode *node = dict->table[__hashmap_hash__(key, dict->seed) & (dict->bucket - 1)];
    while (node) {
        if (__hashmap_variant_equals__(node->key, key)) return &node->value;
        node = node->next;
    }
    return NULL;
}


bool __hashmap_contains__(HashMap *dict, HashMapVariant key) {
    return __hashmap_get__(dict, key) != NULL;
}


bool __hashmap_remove__(HashMap *dict, HashMapVariant key) {
    if (!dict || dict->count == 0 || key.type == _HASHMAP_NULL) return False;
    if (key.type == _HASHMAP_STRING && !key.as.str) return False;
    if (key.type == _HASHMAP_POINTER && !key.as.ptr) return False;

    uint64 idx = __hashmap_hash__(key, dict->seed) & (dict->bucket - 1);
    // Linus Torvalds: the concept of double pointers for removing target in linked list.
    _HashMapNode **indirect = &dict->table[idx];

    while (*indirect) {
        if (__hashmap_variant_equals__((*indirect)->key, key)) {
            _HashMapNode *target = *indirect;
            *indirect = target->next;

            __hashmap_variant_cleanup__(target->key);
            __hashmap_variant_cleanup__(target->value);
            free(target);
            dict->count--;

            if (dict->bucket > __next_power_base__(HASHMAP_INIT_BUCKETS, 2) && dict->count <= HASHMAP_SHRINK_FACTOR * dict->bucket) {
                if (!__hashmap_resize__(dict, __next_power_base__(dict->bucket / 2, 2))) {
                    fprintf(stderr, "\x1b[1;33mWarning: HashMap shrinking failed, being continue to work with redundant buckets.\x1b[0m\n");
                }
            }
            return True;
        }
        indirect = &(*indirect)->next;
    }
    return False;
}


void __hashmap_print__(HashMapVariant x) {
    switch (x.type) {
        case _HASHMAP_NULL: {
            printf("\x1b[1;31mnull\x1b[0m");
            return;
        }
        case _HASHMAP_STRING: {
            if (x.as.str) printf("\x1b[1;32m\"%s\"\x1b[0m", x.as.str);
            else printf("\x1b[1;31m%p\x1b[0m", NULL);
            return;
        }
        case _HASHMAP_POINTER: {
            if (x.as.ptr) printf("\x1b[1;33m%p\x1b[0m", x.as.ptr);
            else printf("\x1b[1;31m%p\x1b[0m", NULL);
            return;
        }
        case _HASHMAP_INT32: {
            printf("\x1b[1;35m%d\x1b[0m", x.as.i);
            return;
        }
        case _HASHMAP_INT64: {
            printf("\x1b[1;35m%lld\x1b[0m", x.as.l);
            return;
        }
        case _HASHMAP_UINT32: {
            printf("\x1b[1;35m%u\x1b[0m", x.as.ui);
            return;
        }
        case _HASHMAP_UINT64: {
            printf("\x1b[1;35m%llu\x1b[0m", x.as.ul);
            return;
        }
        case _HASHMAP_FLOAT: {
            printf("\x1b[1;35m%f\x1b[0m", x.as.f);
            return;
        }
        case _HASHMAP_DOUBLE: {
            printf("\x1b[1;35m%lf\x1b[0m", x.as.d);
            return;
        }
        default:
            printf("\x1b[1;31mnull\x1b[0m");
    }
}


uint64 __hashmap_hash__(HashMapVariant x, uint64 seed) {
    switch (x.type) {
        case _HASHMAP_NULL:
            return 0ULL;
        case _HASHMAP_STRING:
            return __hash_fnv1a__(x.as.str, strlen(x.as.str), seed);
        case _HASHMAP_POINTER: {
            uint64 p = uint64ptr(x.as.ptr);
            return (uint64)(((p ^ seed) ^ (p >> 32)) * 11400714819323198485ULL);
        }
        case _HASHMAP_INT32:
            return __hash_fnv1a__(&x.as.i, sizeof(int32), seed);
        case _HASHMAP_INT64:
            return __hash_fnv1a__(&x.as.l, sizeof(int64), seed);
        case _HASHMAP_UINT32:
            return __hash_fnv1a__(&x.as.ui, sizeof(uint32), seed);
        case _HASHMAP_UINT64:
            return __hash_fnv1a__(&x.as.ul, sizeof(uint64), seed);
        case _HASHMAP_FLOAT: {
            // Handle the `NAN` exception.
            if (x.as.f != x.as.f) return seed ^ 0x7FF8000000000000ULL;
            // Handle the hash anomaly between `-0.0F` and `+0.0F`.
            if (x.as.f == -0.0F) {
                float temp = 0.0F;
                return __hash_fnv1a__(&temp, sizeof(float32), seed);
            }
            return __hash_fnv1a__(&x.as.f, sizeof(float32), seed);
        }
        case _HASHMAP_DOUBLE: {
            // Handle the `NAN` exception.
            if (x.as.d != x.as.d) return seed ^ 0x7FF8000000000000ULL;
            // Handle the hash anomaly between `-0.0` and `+0.0`.
            if (x.as.d == -0.0) {
                double temp = 0.0;
                return __hash_fnv1a__(&temp, sizeof(float64), seed);
            }
            return __hash_fnv1a__(&x.as.d, sizeof(float64), seed);
        }
        default:
            return 0ULL;
    }
}


bool __hashmap_variant_equals__(HashMapVariant x, HashMapVariant y) {
    if (x.type != y.type) return False;
    switch (x.type) {
        case _HASHMAP_NULL:
            return False;
        case _HASHMAP_STRING: {
            if (!x.as.str || !y.as.str) return x.as.str == y.as.str;
            return strcmp(x.as.str, y.as.str) == 0;
        }
        case _HASHMAP_POINTER:
            // Shallow equality check without reflection.
            return x.as.ptr == y.as.ptr;
        case _HASHMAP_INT32:
            return x.as.i == y.as.i;
        case _HASHMAP_INT64:
            return x.as.l == y.as.l;
        case _HASHMAP_UINT32:
            return x.as.ui == y.as.ui;
        case _HASHMAP_UINT64:
            return x.as.ul == y.as.ul;
        case _HASHMAP_FLOAT: {
            // Handle the `NAN` exception.
            if (x.as.f != x.as.f && y.as.f != y.as.f) return True;
            return x.as.f == y.as.f;
        }
        case _HASHMAP_DOUBLE: {
            // Handle the `NAN` exception.
            if (x.as.d != x.as.d && y.as.d != y.as.d) return True;
            return x.as.d == y.as.d;
        }
        default:
            return False;
    }
}


HashMapVariant __hashmap_variant_copy__(HashMapVariant src) {
    HashMapVariant dst = src;
    if (src.type == _HASHMAP_STRING) {
        if (src.as.str) {
            dst.as.str = strdup(src.as.str);
            if (!dst.as.str) dst.type = _HASHMAP_NULL;
        } else dst.as.str = NULL;
    }
    return dst;
}


void __hashmap_variant_cleanup__(HashMapVariant x) {
    if (x.type == _HASHMAP_STRING && x.as.str) free(x.as.str);
}


bool __hashmap_resize__(HashMap *dict, uint64 n) {
    n = __next_power_base__(n, 2);
    if (n < HASHMAP_INIT_BUCKETS) n = __next_power_base__(HASHMAP_INIT_BUCKETS, 2);
    if (n == dict->bucket) return False;

    _HashMapNode **table = calloc(n, sizeof(_HashMapNode *));
    if (!table) return False;

    for (uint64 i = 0; i < dict->bucket; i++) {
        _HashMapNode *node = dict->table[i];
        while (node) {
            _HashMapNode *temp = node->next;
            uint64 idx = node->hash & (n - 1);
            node->next = table[idx];
            table[idx] = node;
            node = temp;
        }
    }

    free(dict->table);
    dict->table = table;
    dict->bucket = n;
    return True;
}


uint64 __hashmap_sizeof__(HashMap *dict) {
    if (!dict) return 0ULL;

    uint64 total = sizeof(HashMap);
    total = total + dict->bucket * sizeof(_HashMapNode *);

    for (uint64 i = 0; i < dict->bucket; i++) {
        for (_HashMapNode *node = dict->table[i]; node; node = node->next) {
            total = total + sizeof(_HashMapNode);
            if (node->key.type == _HASHMAP_STRING && node->key.as.str) total = total + strlen(node->key.as.str) + 1;
            if (node->value.type == _HASHMAP_STRING && node->value.as.str) total = total + strlen(node->value.as.str) + 1;
        }
    }
    return total;
}


uint64 __next_power_base__(uint64 n, uint64 base) {
    if (n <= 1) return 1;
    if (base <= 1) return 0ULL;
    uint64 p = 1;
    while (p < n) {
        if (18446744073709551615ULL / base < p) return 0ULL;
        p = p * base;
    }
    return p;
}


uint64 __hash_fnv1a__(void *data, usize length, uint64 seed) {
    byte *input = (byte *)data;
    uint64 hash = seed ^ 0xcbf29ce484222325ULL;
    for (usize i = 0; i < length; i++) hash = (hash ^ input[i]) * 0x100000001B3ULL;
    return hash;
}


void __convert_unit__(uint64 x, char *buffer, int size) {
    if (x < 1024ULL) snprintf(buffer, size, "%llu B", x);
    else if (x < 1048576ULL) snprintf(buffer, size, "%.3lf KB", x / 1024.0);
    else if (x < 1073741824ULL) snprintf(buffer, size, "%.3lf MB", x / 1024.0 / 1024.0);
    else if (x < 1099511627776ULL) snprintf(buffer, size, "%.3lf GB", x / 1024.0 / 1024.0 / 1024.0);
    else if (x < 1125899906842624ULL) snprintf(buffer, size, "%.3lf TB", x / 1024.0 / 1024.0 / 1024.0 / 1024.0);
    else snprintf(buffer, size, "%.3lf PB", x / 1024.0 / 1024.0 / 1024.0 / 1024.0 / 1024.0);
}
