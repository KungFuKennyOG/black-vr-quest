/* TEST ONLY (native x86-64 Linux builds of tests/nv2a_arm64): the Kelvin sources include
 * <windows.h> on every non-aarch64 host. Map it to the POSIX compat layer. */
#include "platform/win32_compat.h"
