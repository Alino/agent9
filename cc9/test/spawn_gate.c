/* spawn_gate — proves cc9's real posix_spawn carries Ladybird's helper-launch
 * protocol: file_actions (dup2/close/open) applied in order, env reaching the
 * child through /env (SOCKET_TAKEOVER), CLOEXEC fds dying at exec while the
 * dup2'd transport fd survives, and reaping >4 concurrent children
 * (the old rtab cap). Prints "PASS n/n".
 */
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

extern int socketpair(int, int, int, int[2]);

static int npass;
static void ok(const char *what) { npass++; fprintf(stderr, "  ok %d: %s\n", npass, what); }
static void die(const char *what) {
	fprintf(stderr, "FAIL: %s (errno %d %s)\n", what, errno, strerror(errno));
	exit(1);
}

static int child_main(void) {
	/* transport fd arrived via dup2 file action at 7 */
	char *st = getenv("SOCKET_TAKEOVER");
	if (!st || strcmp(st, "WebContent:7") != 0) {
		fprintf(stderr, "child: SOCKET_TAKEOVER=[%s]\n", st ? st : "(nil)");
		return 2;
	}
	/* the parent's CLOEXEC end must be gone in us */
	char *pc = getenv("PCLOEXEC");
	if (pc) {
		int pfd = atoi(pc);
		if (write(pfd, "x", 1) == 1) return 3;    /* leaked CLOEXEC fd! */
	}
	/* the addclose'd fd must be gone */
	char *cl = getenv("CLOSED");
	if (cl) {
		int cfd = atoi(cl);
		if (write(cfd, "x", 1) == 1) return 4;
	}
	/* addopen landed a file at 8 */
	if (write(8, "F8", 2) != 2) return 5;
	/* speak through the transport fd */
	if (write(7, "hello-via-7", 11) != 11) return 6;
	return 0;
}

static int child_quick(void) { return 0; }

int main(int argc, char **argv) {
	if (argc >= 2 && strcmp(argv[1], "child") == 0) return child_main();
	if (argc >= 2 && strcmp(argv[1], "quick") == 0) return child_quick();

	/* 1: spawn with the full Ladybird-shaped file-action set */
	int sp[2];
	if (socketpair(1, 1, 0, sp) < 0) die("socketpair");
	if (fcntl(sp[0], F_SETFD, FD_CLOEXEC) < 0) die("cloexec parent end");

	int tobeclosed = open("/dev/null", O_WRONLY);
	if (tobeclosed < 0) die("open /dev/null");

	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, sp[1], 7);  /* transport -> fixed fd */
	posix_spawn_file_actions_addclose(&fa, tobeclosed);
	posix_spawn_file_actions_addopen(&fa, 8, "/tmp/spawn_gate.dat",
	                                 O_WRONLY | O_CREAT | O_TRUNC, 0644);

	char env_st[] = "SOCKET_TAKEOVER=WebContent:7";
	char env_pc[32], env_cl[32];
	snprintf(env_pc, sizeof env_pc, "PCLOEXEC=%d", sp[0]);
	snprintf(env_cl, sizeof env_cl, "CLOSED=%d", tobeclosed);
	char *envp[] = { env_st, env_pc, env_cl, 0 };
	char *cargv[] = { argv[0], "child", 0 };

	int pid = -1;
	int rc = posix_spawn(&pid, argv[0], &fa, 0, cargv, envp);
	if (rc != 0 || pid <= 0) die("posix_spawn");
	posix_spawn_file_actions_destroy(&fa);
	ok("posix_spawn launched with dup2+close+open actions");

	/* 2: transport fd works parent<-child */
	char buf[32];
	long r = read(sp[0], buf, sizeof buf);
	if (r != 11 || memcmp(buf, "hello-via-7", 11) != 0) die("transport greeting");
	ok("dup2'd fd 7 carried data (SOCKET_TAKEOVER honored)");

	/* 3: child's other checks (CLOEXEC death, addclose, addopen) via status */
	int st;
	if (waitpid(pid, &st, 0) != pid) die("waitpid");
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
		fprintf(stderr, "child exit status %d\n", WEXITSTATUS(st));
		die("child checks failed");
	}
	ok("child verified CLOEXEC death + addclose + env");

	int dfd = open("/tmp/spawn_gate.dat", O_RDONLY);
	if (dfd < 0 || read(dfd, buf, sizeof buf) != 2 || memcmp(buf, "F8", 2) != 0)
		die("addopen file content");
	close(dfd);
	unlink("/tmp/spawn_gate.dat");
	ok("addopen landed at fd 8");
	close(sp[0]); close(sp[1]);

	/* 4: 8 concurrent children (old reaper cap was 4 forking procs) */
	int pids[8];
	char *qargv[] = { argv[0], "quick", 0 };
	for (int i = 0; i < 8; i++)
		if (posix_spawn(&pids[i], argv[0], 0, 0, qargv, 0) != 0) die("spawn burst");
	int reaped = 0;
	for (int tries = 0; tries < 2000 && reaped < 8; tries++) {
		int got = waitpid(-1, &st, WNOHANG);
		if (got > 0) reaped++;
		else usleep(10000);
	}
	if (reaped != 8) die("burst reap (got fewer than 8)");
	ok("8 concurrent spawns reaped via waitpid(WNOHANG)");

	printf("PASS %d/%d\n", npass, npass);
	return 0;
}
