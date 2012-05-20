#ifndef _OBJIMPL_H_
#define _OBJIMPL_H_

void * PyObject_Malloc(size_t);
void * PyObject_Realloc(void *, size_t);
void PyObject_Free(void *);


#endif /* _OBJIMPL_H_ */
