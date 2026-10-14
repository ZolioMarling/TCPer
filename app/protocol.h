#ifndef _PROTOCOL_H_
#define _PROTOCOL_H_


#if !defined(__OS_WINDOWS__) && !defined(__OS_UNIX__)
    #if defined(_WIN32) || defined(__WIN32__) || defined(__WINDOWS__)
        #define __OS_WINDOWS__
    #elif defined(__linux__) || defined(__APPLE__)
        #define __OS_UNIX__
        #ifndef _GNU_SOURCE
            #define _GNU_SOURCE
        #endif
    #else
        #error "Unsupported platforms."
    #endif
#endif


#define PATH_LENGTH_RESTRICTION 512
#define DEFAULT_SERVICE_PORT 31415


#endif
