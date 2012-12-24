#ifndef CEPH_INTTYPES_H
#define CEPH_INTTYPES_H

#include "acconfig.h"

#ifdef HAVE_STDINT_H
#include <stdint.h>
#else
#error "System without stdint.h not yet supported. Please report."
#endif

#ifdef HAVE_LINUX_TYPES_H
#include <linux/types.h>
#endif

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#ifndef HAVE___U8
typedef uint8_t __u8;
#endif

#ifndef HAVE___S8
typedef int8_t __s8;
#endif

#ifndef HAVE___U16
typedef uint16_t __u16;
#endif

#ifndef HAVE___S16
typedef int16_t __s16;
#endif

#ifndef HAVE___U32
typedef uint32_t __u32;
#endif

#ifndef HAVE___S32
typedef int32_t __s32;
#endif

#ifndef HAVE___U64
typedef uint64_t __u64;
#endif

#ifndef HAVE___S64
typedef int64_t __s64;
#endif

/*
 * These types with bitwise attributes are used by Sparse. On Linux they'll be
 * provided by linux/types.h, otherwise we make them normal integers.
 */

#ifndef HAVE___LE16
typedef __u16 __le16;
#endif

#ifndef HAVE___BE16
typedef __u16 __be16;
#endif

#ifndef HAVE___LE32
typedef __u32 __le32;
#endif

#ifndef HAVE___BE32
typedef __u32 __be32;
#endif

#ifndef HAVE___LE64
typedef __u64 __le64;
#endif

#ifndef HAVE___BE64
typedef __u64 __be64;
#endif

#endif
