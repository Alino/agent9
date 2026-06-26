// P0.1 regression: crt0 must pass the real kernel argc/argv and populate environ
// from /env. The suite runner invokes with no args (argc==1); run manually as
// `regress_args alpha beta` to also assert the arg strings.
// EXPECT: regress_args: argv/env PASS
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <cstdio>
extern "C" char **environ;
int main(int argc, char **argv) {
	assert(argc >= 1);
	assert(argv[0] != nullptr);
	assert(argv[argc] == nullptr);          // argv is NULL-terminated
	if (argc >= 3) {                        // when invoked `regress_args alpha beta`
		assert(strcmp(argv[1], "alpha") == 0);
		assert(strcmp(argv[2], "beta") == 0);
	}
	assert(environ != nullptr);
	assert(environ[0] != nullptr);          // /env is never empty on 9front
	const char *u = getenv("user");
	assert(u && u[0]);                      // /env/user exists
	(void)argv; (void)u;
	printf("regress_args: argv/env PASS\n");   // deterministic for the suite runner
	return 0;
}
