#pragma once

#include <stddef.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Bounded string copy.
//
// Replaces the strcpy() and strncpy(dst, src, <hard-coded length>) calls that
// were spread through the firmware. Taking the destination by array reference
// means the size can never drift away from the buffer it belongs to, and the
// result is always null-terminated - plain strncpy is not, when the source
// exactly fills the destination.
//
// No Arduino dependency, so the native tests cover it directly.
// ---------------------------------------------------------------------------
template <size_t N>
inline void copyField(char (&destination)[N], const char *source)
{
  if (source == nullptr)
  {
    destination[0] = '\0';
    return;
  }
  strncpy(destination, source, N - 1);
  destination[N - 1] = '\0';
}
