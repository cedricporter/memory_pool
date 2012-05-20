/**
 * @file    timer.c
 * @author  Hua Liang <et@everet.org>
 * @Website http://EverET.org
 * @date    Sun May 20 21:40:37 2012
 * 
 * @brief   get the run time of a func.
 * 
 * 
 */

#include "timer.h"

double get_runtime(void (*func)(void *), void *param)
{
    struct timeval start, end;
    double timeuse;

    gettimeofday(&start, NULL);
    func(param);
    gettimeofday(&end, NULL);
     
    timeuse = 1000000 * (end.tv_sec - start.tv_sec) + end.tv_usec - start.tv_usec;
    timeuse /= 1000000;

    return timeuse;
}
