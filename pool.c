/**
 * @file    pool.c
 * @author  Hua Liang <et@everet.org>
 * @Website http://EverET.org
 * @date    Sun May 20 21:23:04 2012
 * 
 * @brief   Test python memory pool performance.
 * 
 * 
 */

#include <stdio.h>
#include "objimpl.h"
#include "timer.h"

int SIZE_LIST[] = {1, 3, 5, 7, 8, 13, 55, 77, 88, 99, 100};
int LENGTH = sizeof(SIZE_LIST) / sizeof(int);

typedef struct node
{
    struct node *next;
} node;

/* ==================================================
 * my_malloc is memory allocation function pointer
 * my_free is memeroy free function pointer
 * size is the size of each memory allocation
 * loop is the number of iteration
 */ 
/** free memory after malloc immediately. */
void test_imm_free(void *(*my_malloc)(size_t), void (*my_free)(void *),
                   int size, int loop)
{
    int i, j = 0;
    node *head = my_malloc(size), *tmp = NULL;
    head->next = NULL;
    for (i = 0; i < loop; ++i)
    {
        size = (SIZE_LIST[j = (j < sizeof(LENGTH) - 1 ? j + 1 : 0)]);
        tmp = (node *)my_malloc(size);
        my_free(tmp);
    }
}

/** free memory every 1000 iteration. */
void test_sometimes_free(void *(*my_malloc)(size_t), void (*my_free)(void *),
                         int size, int loop)
{
    int i, j = 0;
    node *head = my_malloc(size), *tmp = NULL;
    head->next = NULL;
    for (i = 0; i < loop; ++i)
    {
        size = (SIZE_LIST[j = (j < sizeof(LENGTH) - 1 ? j + 1 : 0)]);
        tmp = (node *)my_malloc(size);
        tmp->next = head;
        head = tmp;

        if (i % 1000)
        {
            while (head->next)
            {
                tmp = head;
                head = head->next;
                my_free(tmp);
            }
        }

    }
}

/** malloc memory and free at last. */
void test_last_free(void *(*my_malloc)(size_t), void (*my_free)(void *),
                    int size, int loop)
{
    int i, j = 0;
    node *head = my_malloc(size), *tmp = NULL;
    head->next = NULL;
    for (i = 0; i < loop; ++i)
    {
        size = (SIZE_LIST[j = (j < sizeof(LENGTH) - 1 ? j + 1 : 0)]);
        tmp = (node *)my_malloc(size);
        tmp->next = head;
        head = tmp;
    }
    while (head)
    {
        tmp = head;
        head = head->next;
        my_free(tmp);
    }
}

typedef struct param
{
    void (*test_func)(void *(*my_malloc)(size_t),
                      void (*my_free)(void *), int, int);
    void *(*my_malloc)(size_t);
    void (*my_free)(void *);
    uint malloc_size;
    uint nloops;
} param;

void run_test(void *pa)
{
    param *p = (param *)pa; 
    (p->test_func)(p->my_malloc, p->my_free, p->malloc_size, p->nloops);
}

void test()
{
#define PARAM(name, func, my_malloc, my_free, size, nloops) \
    param name = {func, my_malloc, my_free, size, nloops};

#define TEST_WITHOUT_POOL(func, p)                                      \
    printf(#func"\twithout pool:\t%lfs\n", get_runtime(run_test, &(p)));

#define TEST_WITH_POOL(func, p)                                         \
    printf(#func"\twith pool:\t%lfs\n", get_runtime(run_test, &(p)));

#define WITH_POOL(name, func, my_malloc, my_free, size, nloops) \
    PARAM(name, func, my_malloc, my_free, size, nloops)         \
        TEST_WITH_POOL(func, name) 
#define WITHOUT_POOL(name, func, my_malloc, my_free, size, nloops)  \
    PARAM(name, func, my_malloc, my_free, size, nloops)             \
        TEST_WITHOUT_POOL(func, name)

    WITHOUT_POOL(p1, test_imm_free, malloc, free, 10, 100000000);
    WITH_POOL(p4, test_imm_free, PyObject_Malloc, PyObject_Free, 10,
              100000000);

    WITHOUT_POOL(p2, test_sometimes_free, malloc, free, 10, 50000000);
    WITH_POOL(p5, test_sometimes_free, PyObject_Malloc, PyObject_Free, 10,
              50000000);

    WITHOUT_POOL(p3, test_last_free, malloc, free, 10, 50000000);
    WITH_POOL(p6, test_last_free, PyObject_Malloc, PyObject_Free, 10,
              50000000);
}

int main(int argc, char *argv[])
{
    test();
    return 0;
}












