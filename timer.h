/**
 * @file    timer.h
 * @author  Hua Liang <et@everet.org>
 * @Website http://EverET.org
 * @date    Sun May 20 21:40:12 2012
 * 
 * @brief   get the runtime of a function
 * 
 * 
 */
#ifndef _TIMER_H_
#define _TIMER_H_

#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>

double get_runtime(void (*func)(void *), void *param);



#endif /* _TIMER_H_ */



















