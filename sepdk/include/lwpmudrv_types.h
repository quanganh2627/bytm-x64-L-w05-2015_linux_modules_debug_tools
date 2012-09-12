/*COPYRIGHT**
 * -------------------------------------------------------------------------
 *               INTEL CORPORATION PROPRIETARY INFORMATION
 *  This software is supplied under the terms of the accompanying license
 *  agreement or nondisclosure agreement with Intel Corporation and may not
 *  be copied or disclosed except in accordance with the terms of that
 *  agreement.
 *        Copyright (c) 2007-2011 Intel Corporation.  All Rights Reserved.
 * -------------------------------------------------------------------------
**COPYRIGHT*/

/*
 *  File  : lwpmudrv_types.h
 *  cvsid[] = "$Id: lwpmudrv_types.h 203263 2011-12-09 07:15:49Z hmaiya $"
 */

#ifndef _LWPMUDRV_TYPES_H_
#define _LWPMUDRV_TYPES_H_

#if defined(__cplusplus)
extern "C" {
#endif

typedef          long       DRV_BOOL;

typedef unsigned char       U8;
typedef          char       S8;
typedef          short      S16;
typedef unsigned short      U16;
typedef unsigned int        U32;
typedef          int        S32;
#if defined(DRV_OS_WINDOWS)
typedef unsigned __int64    U64;
typedef          __int64    S64;
#elif defined (DRV_OS_LINUX) || defined(DRV_OS_SOLARIS) || defined(DRV_OS_MAC) || defined(DRV_OS_ANDROID)
typedef unsigned long long  U64;
typedef          long long  S64;
typedef unsigned long       ULONG;
typedef          void       VOID;
typedef          void*      LPVOID;
#else
#error "Undefined OS"
#endif

#if defined(DRV_IA32)

typedef S32                 SIOP;
typedef U32                 UIOP;

#elif defined(DRV_EM64T) || defined(DRV_IA64)

typedef S64                 SIOP;
typedef U64                 UIOP;

#else
#error "Unexpected Architecture seen"
#endif

typedef   void*             PVOID;

#if defined(UNICODE)
typedef   wchar_t           STCHAR;
#else
typedef   char              STCHAR;
#endif

#if defined(DRV_OS_WINDOWS)
#include <wchar.h>
typedef   wchar_t           DRV_STCHAR;
#else
typedef   char              DRV_STCHAR;
#endif

//
// Handy Defines
//
typedef   U32                             DRV_STATUS;

#define   MAX_STRING_LENGTH             1024
#define   MAXNAMELEN                     256

#if defined(DRV_OS_WINDOWS)
#define   UNLINK                        _unlink
#define   RENAME                        rename
#define   WCSDUP                        _wcsdup
#endif
#if defined(DRV_OS_LINUX) || defined(DRV_OS_SOLARIS) || defined(DRV_OS_MAC) || defined(DRV_OS_ANDROID)
#define   UNLINK                        unlink
#define   RENAME                        rename
#endif

#if (defined(DRV_OS_SOLARIS) || defined(DRV_OS_MAC) || defined(DRV_OS_ANDROID)) && !defined(_KERNEL)
//wcsdup is missing on Solaris
#include <stdlib.h>
#include <wchar.h>

static inline wchar_t* solaris_wcsdup(const wchar_t *wc)
{
	wchar_t *tmp = (wchar_t *)malloc(wcslen(wc) + 1);
	wcscpy(tmp, wc);
	return tmp;
}
#define   WCSDUP                        solaris_wcsdup
#endif

#if defined(DRV_OS_LINUX)
#define   WCSDUP                        wcsdup
#endif

#define   TRUE                          1
#define   FALSE                         0

#define ALIGN_4(x)    (((x) +  3) &  ~3)
#define ALIGN_8(x)    (((x) +  7) &  ~7)
#define ALIGN_16(x)   (((x) + 15) & ~15)
#define ALIGN_32(x)   (((x) + 31) & ~31)

#if defined(__cplusplus)
}
#endif

#endif  /* _LWPMUDRV_TYPES_H_ */