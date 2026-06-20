/*
 * faulthandler_stub.c -- minimal replacement for Modules/faulthandler.c on
 * Plan 9. The real module's reentrant sigaction handlers + sigaltstack don't
 * port cleanly to kencc/APE and aren't needed to boot. The core only calls
 * _PyFaulthandler_Init/_Fini; config.c references PyInit_faulthandler.
 *
 * The Python-visible API is stubbed as no-ops so callers that drive it
 * (notably the regression-test harness, test.libregrtest.setup, which calls
 * faulthandler.enable()/dump_traceback_later()) keep working. We don't
 * actually install fault handlers -- on Plan 9 a faulting proc enters the
 * Broken state and can be inspected with acid instead.
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

static int faulthandler_enabled = 0;

/*ARGSUSED*/
static PyObject *
faulthandler_enable(PyObject *self, PyObject *args, PyObject *kwds)
{
	(void)self; (void)args; (void)kwds;
	faulthandler_enabled = 1;
	Py_RETURN_NONE;
}

/*ARGSUSED*/
static PyObject *
faulthandler_disable(PyObject *self, PyObject *Py_UNUSED(ignored))
{
	(void)self;
	faulthandler_enabled = 0;
	Py_RETURN_NONE;
}

/*ARGSUSED*/
static PyObject *
faulthandler_is_enabled(PyObject *self, PyObject *Py_UNUSED(ignored))
{
	(void)self;
	return PyBool_FromLong(faulthandler_enabled);
}

/*ARGSUSED*/
static PyObject *
faulthandler_dump_traceback(PyObject *self, PyObject *args, PyObject *kwds)
{
	(void)self; (void)args; (void)kwds;
	Py_RETURN_NONE;
}

/*ARGSUSED*/
static PyObject *
faulthandler_dump_traceback_later(PyObject *self, PyObject *args, PyObject *kwds)
{
	(void)self; (void)args; (void)kwds;
	Py_RETURN_NONE;
}

/*ARGSUSED*/
static PyObject *
faulthandler_cancel_dump_traceback_later(PyObject *self, PyObject *Py_UNUSED(ignored))
{
	(void)self;
	Py_RETURN_NONE;
}

/*ARGSUSED*/
static PyObject *
faulthandler_register(PyObject *self, PyObject *args, PyObject *kwds)
{
	(void)self; (void)args; (void)kwds;
	Py_RETURN_NONE;
}

/*ARGSUSED*/
static PyObject *
faulthandler_unregister(PyObject *self, PyObject *args)
{
	(void)self; (void)args;
	/* documented to return True if a handler was removed; we never have one */
	Py_RETURN_FALSE;
}

static PyMethodDef faulthandler_methods[] = {
	{"enable", (PyCFunction)(void(*)(void))faulthandler_enable,
	 METH_VARARGS | METH_KEYWORDS, "enable(): no-op on Plan 9"},
	{"disable", faulthandler_disable, METH_NOARGS, "disable(): no-op"},
	{"is_enabled", faulthandler_is_enabled, METH_NOARGS, "is_enabled()"},
	{"dump_traceback", (PyCFunction)(void(*)(void))faulthandler_dump_traceback,
	 METH_VARARGS | METH_KEYWORDS, "dump_traceback(): no-op"},
	{"dump_traceback_later",
	 (PyCFunction)(void(*)(void))faulthandler_dump_traceback_later,
	 METH_VARARGS | METH_KEYWORDS, "dump_traceback_later(): no-op"},
	{"cancel_dump_traceback_later", faulthandler_cancel_dump_traceback_later,
	 METH_NOARGS, "cancel_dump_traceback_later(): no-op"},
	{"register", (PyCFunction)(void(*)(void))faulthandler_register,
	 METH_VARARGS | METH_KEYWORDS, "register(): no-op"},
	{"unregister", faulthandler_unregister, METH_VARARGS, "unregister()"},
	{NULL, NULL, 0, NULL}
};

static struct PyModuleDef faulthandler_module = {
	PyModuleDef_HEAD_INIT,
	"faulthandler",
	"stub faulthandler (no-op on Plan 9)",
	0,
	faulthandler_methods, NULL, NULL, NULL, NULL
};

PyMODINIT_FUNC
PyInit_faulthandler(void)
{
	return PyModule_Create(&faulthandler_module);
}
