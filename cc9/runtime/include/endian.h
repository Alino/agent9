#ifndef _ENDIAN_H
#define _ENDIAN_H
/* x86-64 target: little-endian, always. */
#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN    4321
#define __BYTE_ORDER    __LITTLE_ENDIAN
#define LITTLE_ENDIAN   __LITTLE_ENDIAN
#define BIG_ENDIAN      __BIG_ENDIAN
#define BYTE_ORDER      __BYTE_ORDER
#define htole16(x) ((unsigned short)(x))
#define le16toh(x) ((unsigned short)(x))
#define htole32(x) ((unsigned int)(x))
#define le32toh(x) ((unsigned int)(x))
#define htole64(x) ((unsigned long long)(x))
#define le64toh(x) ((unsigned long long)(x))
#define htobe16(x) __builtin_bswap16((unsigned short)(x))
#define be16toh(x) __builtin_bswap16((unsigned short)(x))
#define htobe32(x) __builtin_bswap32((unsigned int)(x))
#define be32toh(x) __builtin_bswap32((unsigned int)(x))
#define htobe64(x) __builtin_bswap64((unsigned long long)(x))
#define be64toh(x) __builtin_bswap64((unsigned long long)(x))
#endif
