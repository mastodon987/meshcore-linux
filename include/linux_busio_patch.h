#pragma once
// Fix 1: Disable AVR-specific fast pin I/O — these macros don't exist on Linux
#ifdef BUSIO_USE_FAST_PINIO
  #undef BUSIO_USE_FAST_PINIO
#endif
