#include <sys/sysmacros.h>
#include <spl-debug.h>



/* Debug log support enabled */
__attribute__((noinline)) int assfail(char *str) __attribute__((optnone))
{
	return 1; // Must return true for ASSERT macro
}
