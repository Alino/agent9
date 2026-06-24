# node9 Node.js compatibility test suite

Borrows Node.js's own `test/parallel/test-*.js` (v20.20.2) for the core modules node9
implements (path, buffer, events, url, querystring, util, string_decoder, ...), run against
the node9/QuickJS runtime via `run-one.js` + a minimal `common` shim (`common-index.js`).

Harness layout on the VM:
  /usr/glenda/node9/nt/common/index.js   (the shim)
  /usr/glenda/node9/nt/parallel/test-*.js (Node's tests)
  /usr/glenda/node9/nt/run-one.js         (per-test runner: PASS/FAIL/SKIP)
  run-all.rc                              (loops over all tests)

NOTE: Node's test files are monolithic (one file = hundreds of assertions); a single
Node-internal/exact-message/engine-limit assertion fails the whole file. Whole-file pass
counts therefore undersell coverage. Running these tests drove real fixes to the port
(path.basename, util.inspect/format/inherits, the full Buffer numeric API, string_decoder
multi-byte, and the kencc NaN-comparison codegen bugs). Out-of-scope failures: tests that
require Node internals (internal/*, internal/test/binding), node:test-heavy suites, and
Windows-path (path.win32) behavior — node9 aliases win32->posix.
