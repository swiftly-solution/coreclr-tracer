// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#ifndef _src_coreclr_guid_h
#define _src_coreclr_guid_h

#include "rpc.h"
#include "guiddef.h"

#ifndef EXTERN_C
#define EXTERN_C extern "C"
#endif // EXTERN_C

#ifdef DEFINE_GUID
#undef DEFINE_GUID
#endif

#ifndef _WIN32
#include <cstring>
bool minipal_guid_equals(const GUID *v1, const GUID *v2)
{
  return memcmp(v1, v2, sizeof(GUID)) == 0;
}
#endif

#define DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
  EXTERN_C const GUID name = {l, w1, w2, {b1, b2, b3, b4, b5, b6, b7, b8}}

#endif
