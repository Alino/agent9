/* sigtab_gate — signal() and sigaction() share one handler table.
 *
 * Before: signal() wrote its own table; a sigaction() save/restore over a
 * signal()-installed handler read SIG_DFL from sigaction's table and
 * "restored" it, silently deregistering the live handler. This checks the
 * two now agree.
 */
#include <stdio.h>
#include <signal.h>
#include <string.h>

static int pass, total, ran;
static void ck(int ok, const char *what) {
	total++;
	printf("%d %s: %s\n", total, what, ok ? "PASS" : "FAIL");
	if (ok) pass++;
}
static void handler(int s) { (void)s; ran++; }

int main(void)
{
	/* install via signal() */
	signal(SIGUSR1, handler);

	/* sigaction must SEE that handler as the old action, not SIG_DFL */
	struct sigaction old;
	memset(&old, 0, sizeof old);
	sigaction(SIGUSR1, 0, &old);
	ck(old.sa_handler == handler, "sigaction sees signal()-installed handler");

	/* the classic save / temporarily-override / restore dance */
	struct sigaction tmp;
	memset(&tmp, 0, sizeof tmp);
	tmp.sa_handler = SIG_IGN;
	sigaction(SIGUSR1, &tmp, 0);          /* override */
	sigaction(SIGUSR1, &old, 0);          /* restore the saved (real) handler */

	/* after restore, the handler must still be installed */
	struct sigaction now;
	memset(&now, 0, sizeof now);
	sigaction(SIGUSR1, 0, &now);
	ck(now.sa_handler == handler, "handler survives save/override/restore");

	/* and running it works */
	raise(SIGUSR1);
	ck(ran >= 1, "raise runs the restored handler");

	/* signal()'s return value = the prior handler */
	void (*prev)(int) = signal(SIGUSR1, SIG_DFL);
	ck(prev == handler, "signal() returns the prior handler");

	printf("sigtab_gate %d/%d %s\n", pass, total, pass == total ? "PASS" : "FAIL");
	return pass == total ? 0 : 1;
}
