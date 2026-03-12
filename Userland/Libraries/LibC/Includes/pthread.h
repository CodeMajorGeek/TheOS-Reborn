#ifndef _PTHREAD_H
#define _PTHREAD_H

#include <sys/types.h>

typedef int pthread_t;

typedef struct pthread_attr
{
    int reserved;
} pthread_attr_t;

typedef struct pthread_mutex
{
    volatile int locked;
} pthread_mutex_t;

#define PTHREAD_MUTEX_INITIALIZER { 0 }

int pthread_create(pthread_t* thread,
                   const pthread_attr_t* attr,
                   void* (*start_routine)(void*),
                   void* arg);
int pthread_join(pthread_t thread, void** retval);
void pthread_exit(void* retval);
pthread_t pthread_self(void);

int pthread_mutex_init(pthread_mutex_t* mutex, const void* attr);
int pthread_mutex_destroy(pthread_mutex_t* mutex);
int pthread_mutex_lock(pthread_mutex_t* mutex);
int pthread_mutex_unlock(pthread_mutex_t* mutex);

#endif
