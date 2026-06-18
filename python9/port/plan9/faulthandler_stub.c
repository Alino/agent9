/*
 * faulthandler_stub.c -- minimal replacement for Modules/faulthandler.c on
 * Plan 9. The real module's reentrant sigaction handlers + sigaltstack don't
 * port cleanly to kencc/APE and aren't needed to boot. The core only calls
 * _PyFaulthandler_Init/_Fini; config.c references PyInit_faulthandler.
 */
#include "Python.h"

int
_PyFaulthandler_Init(int enable)
{
	(void)enable;
	return 0;
}

void
_PyFaulthandler_Fini(void)
{
}

static struct PyModuleDef faulthandler_module = {
	PyModuleDef_HEAD_INIT,
	"faulthandler",
	"stub faulthandler (unsupported on Plan 9)",
	0,
	NULL, NULL, NULL, NULL, NULL
};

PyMODINIT_FUNC
PyInit_faulthandler(void)
{
	return PyModule_Create(&faulthandler_module);
}
