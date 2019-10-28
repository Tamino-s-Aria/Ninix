#ifndef _ZJUNIX_ERR_H
#define _ZJUNIX_ERR_H

#include <zjunix/type.h>

#define MAX_ERRNO	4095

#define IS_ERR_VALUE(x) (u32)(x) >= (u32)-MAX_ERRNO

static inline void * ERR_PTR(u32 error)
{
	return (void *) error;
}

static inline u32 PTR_ERR(const void *ptr)
{
	return (u32) ptr;
}

static inline u32  IS_ERR(const void *ptr)
{
	return IS_ERR_VALUE((u32)ptr);
}

static inline u32 IS_ERR_OR_NULL(const void *ptr)
{
	return (!ptr) || IS_ERR_VALUE((u32)ptr);
}

/**
 * ERR_CAST - Explicitly cast an error-valued pointer to another pointer type
 * @ptr: The pointer to cast.
 *
 * Explicitly cast an error-valued pointer to another pointer type in such a
 * way as to make it clear that's what's going on.
 */
static inline void *  ERR_CAST(const void *ptr)
{
	/* cast away the const */
	return (void *) ptr;
}

static inline u32 PTR_ERR_OR_ZERO(const void *ptr)
{
	if (IS_ERR(ptr))
		return PTR_ERR(ptr);
	else
		return 0;
}

/* Deprecated */
#define PTR_RET(p) PTR_ERR_OR_ZERO(p)

#endif /* __ERR_H */