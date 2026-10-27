/* Minimal idle XPC stub for unsupported BlastDoor services on non-Apple SoCs.
 * Blocks quietly in pause() when launchd spawns it on demand, preventing
 * 10-second crash/respawn loops at zero CPU and <200 KB memory.
 */
#include <signal.h>
#include <unistd.h>

static void
on_term(int sig)
{
	(void)sig;
	_exit(0);
}

int
main(void)
{
	signal(SIGTERM, on_term);
	for (;;)
		pause();
	return 0;
}
