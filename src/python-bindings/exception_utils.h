#ifndef   _PYTHON_BINDINGS_EXCEPTION_UTILS_H
#define   _PYTHON_BINDINGS_EXCEPTION_UTILS_H

PyObject * CreateExceptionInModule( const char * qualifiedName,
	const char * name, PyObject * base = NULL );

#endif /* _PYTHON_BINDINGS_EXCEPTION_UTILS_H */
