/* arm64 build only (port/android): MSVC <intrin.h>. The runtime only reaches
 * it under _MSC_VER/_WIN32; BLACK's own sources use it for SSE. */
#include "xmmintrin.h"
