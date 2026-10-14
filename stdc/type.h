#ifndef _TYPE_H_
#define _TYPE_H_


#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>


#if UINTPTR_MAX == 0xffffffffffffffff
    #define __ARCH_64__
#else
    #error "Unsupported architecture. TCPer supports 64-bit Windows, Linux, and macOS only."
#endif


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


#if defined(__OS_UNIX__)
    #include <sys/types.h>
    typedef ssize_t ssize;
#elif defined(__OS_WINDOWS__)
    #include <basetsd.h>
    typedef SSIZE_T ssize;
#endif


#if defined(__OS_UNIX__)
    /**
     * @brief Cast a pointer to a structure member out to the containing structure (`GNU`).
     * @param ptr A pointer to the member.
     * @param type The type of the container struct this is embedded in.
     * @param member The name of the member within the struct.
    **/
    #define type_container_of(ptr, type, member) ({const typeof(((type *)0)->member) *mptr = (ptr); (type *)((char *)mptr - offsetof(type, member));})
#elif defined(__OS_WINDOWS__)
    /**
     * @brief Cast a pointer to a structure member out to the containing structure (`Universal`).
     * @param ptr A pointer to the member.
     * @param type The type of the container struct this is embedded in.
     * @param member The name of the member within the struct.
    **/
    #define type_container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif


#define KB ((unsigned long long)(1 << 10))
#define MB ((unsigned long long)(1 << 20))
#define GB ((unsigned long long)(1 << 30))


#define True 1
#define False 0
#define intptr(x) ((intptr_t)(x))
#define uintptr(x) ((uintptr_t)(x))
#define int32ptr(x) ((int32_t)(intptr_t)(x))
#define int64ptr(x) ((long long)(intptr_t)(x))
#define uint32ptr(x) ((uint32_t)(uintptr_t)(x))
#define uint64ptr(x) ((unsigned long long)(uintptr_t)(x))


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


#endif
