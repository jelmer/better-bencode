#include <Python.h>

struct benc_state {
	int size;
	int offset;
	char buffer[1024];
	PyObject* fout;
};

static void benc_state_flush(struct benc_state* bs) {
	if (bs->offset > 0) {
		PyObject_CallMethod(bs->fout, "write", "s#", bs->buffer, bs->offset);
		bs->offset = 0;
	}
}

static void benc_state_try_flush(struct benc_state* bs) {
	if (bs->offset >= bs->size) {
		PyObject_CallMethod(bs->fout, "write", "s#", bs->buffer, bs->offset);
		bs->offset = 0;
	}
}

static void benc_state_write_char(struct benc_state* bs, char c) {
	bs->buffer[bs->offset++] = c;
	benc_state_try_flush(bs);
}

static void benc_state_write_string(struct benc_state* bs, char* str) {
	while (*str) {
		bs->buffer[bs->offset++] = *str++;
		benc_state_try_flush(bs);
	}
}

static void benc_state_write_buffer(struct benc_state* bs, char* buff, int size) {
	int i;
	for (i = 0; i < size; i++) {
		bs->buffer[bs->offset++] = buff[i];
		benc_state_try_flush(bs);
	}
}

static void benc_state_write_format(struct benc_state* bs, const int limit, const void *format, ...) {
	char buffer[limit + 1]; // moze by malloca()?

	va_list ap;
	va_start(ap, format);
	int size = vsnprintf(buffer, limit, format, ap);
	va_end(ap);

	return benc_state_write_buffer(bs, buffer, (size < limit) ? size : (limit - 1));
}


static int do_dump(struct benc_state *bs, PyObject* obj);

static int do_dump(struct benc_state *bs, PyObject* obj) {
	int i = 0, n = 0;
	if (obj == Py_None) {
		benc_state_write_char(bs, 'n');
	} else if (obj == Py_True) {
		benc_state_write_char(bs, 't');
	} else if (obj == Py_False) {
		benc_state_write_char(bs, 'f');
	} else if (PyBytes_CheckExact(obj)) {
		char *buff = PyBytes_AS_STRING(obj);
		int size = PyBytes_GET_SIZE(obj);

		benc_state_write_format(bs, 12, "%d:", size);
		benc_state_write_buffer(bs, buff, size);
	} else if (PyInt_Check(obj) || PyLong_Check(obj)) {
		// long x = PyLong_AsLong(obj);
		// benc_state_write_format(bs, 20, "i%lde", x);
		PyObject *encoded = PyObject_Str(obj);
		char *buff = PyBytes_AS_STRING(encoded);
		int size = PyBytes_GET_SIZE(encoded);
		benc_state_write_char(bs, 'i');
		benc_state_write_buffer(bs, buff, size);
		benc_state_write_char(bs, 'e');
	} else if (PyFloat_Check(obj)) {
		double real_val = PyFloat_AS_DOUBLE(obj);
		printf("REAL (%G)\n", real_val);
	} else if (PyList_CheckExact(obj)) {
		n = PyList_GET_SIZE(obj);
		benc_state_write_char(bs, 'l');
		for (i = 0; i < n; i++) {
			do_dump(bs, PyList_GET_ITEM(obj, i));
		}
		benc_state_write_char(bs, 'e');
	} else if (PyDict_CheckExact(obj)) {
		Py_ssize_t pos = 0;
		PyObject *key, *value;

		benc_state_write_char(bs, 'd');
		while (PyDict_Next(obj, &pos, &key, &value)) {
			do_dump(bs, key);
			do_dump(bs, value);
		}
		benc_state_write_char(bs, 'e');
	} else {
		printf("WTF??\n");
	}
	return 0;
}

static PyObject* dump(PyObject* self, PyObject* args) {
	PyObject* obj;
	PyObject* write;
	struct benc_state bs;
	bs.size = 1000;
	bs.offset = 0;

	if (!PyArg_ParseTuple(args, "OO", &obj, &write))
		return NULL;

	bs.fout = write;
	
	do_dump(&bs, obj);

	benc_state_flush(&bs);

	return Py_BuildValue("s#", bs.buffer, bs.offset);
}


struct benc_read {
	int size;
	int offset;
	char *buffer;
	// PyObject* fin;
};

static char benc_read_char(struct benc_read *br) {
	if (br->buffer != NULL) {
		return br->buffer[br->offset++];
	}
}


static PyObject *do_load(struct benc_read *br) {
	PyObject *retval = NULL;

	char first = benc_read_char(br);
	//printf("%c\n", first);

	switch (first) {
		case 'n':
			Py_INCREF(Py_None);
			retval = Py_None;
			break;
		case 'f':
			Py_INCREF(Py_False);
			retval = Py_False;
			break;
		case 't':
			Py_INCREF(Py_True);
			retval = Py_True;
			break;
		case 'i': {
			int sign = 1;
			long long value = 0;
			char current = benc_read_char(br);
			if (current == '-') {
				sign = -1;
				current = benc_read_char(br);
			}
			// TODO: sprawdzanie przedzialow
			while (('0' <= current) && (current <= '9')) {
				value = value * 10 + (current - '0');
				current = benc_read_char(br);
			}
			value *= sign;
			retval = PyLong_FromLongLong(value);

			} break;

		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': {
			int size = first - '0';
			char current = benc_read_char(br);
			// TODO: sprawdzanie przedzialow
			while (('0' <= current) && (current <= '9')) {
				size = size * 10 + (current - '0');
				current = benc_read_char(br);
			}
			retval = PyString_FromStringAndSize(br->buffer + br->offset, size);
			br->offset += size;

			} break;
		case 'e':
			Py_INCREF(PyExc_StopIteration);
			retval = PyExc_StopIteration;
			break;
		case 'l': {
			PyObject *v = PyList_New(0);
			PyObject *item;

			while (1) {
				item = do_load(br);
				if (item == PyExc_StopIteration) {
					Py_DECREF(PyExc_StopIteration);
					break;
				}


				// if ( v2 == NULL ) {
				// 	if (!PyErr_Occurred())
				// 		PyErr_SetString(PyExc_TypeError,
				// 			"NULL object in marshal data for list");
				// 	Py_DECREF(v);
				// 	v = NULL;
				// 	break;
				// }
				PyList_Append(v, item);
			}

			retval = v;
			} break;
		case 'd': {
			PyObject *v = PyDict_New();
			// R_REF(v);
			// if (v == NULL) {
			// 	retval = NULL;
			// 	break;
			// }
			while (1) {
				PyObject *key, *val;
				key = do_load(br);
				if (key == NULL)
					break;
				if (key == PyExc_StopIteration) {
					Py_DECREF(PyExc_StopIteration);
					break;
				}
				val = do_load(br);
				if (val != NULL)
					PyDict_SetItem(v, key, val);
				Py_DECREF(key);
				Py_XDECREF(val);
			}
			// if (PyErr_Occurred()) {
			// 	Py_DECREF(v);
			// 	v = NULL;
			// }
			retval = v;
			} break;

		default:
			/* Bogus data got written, which isn't ideal.
			   This will let you keep working and recover. */
			// PyErr_SetString(PyExc_ValueError, "bad input data");
			retval = NULL;
			break;
	}
	return retval;
}


static PyObject* loads(PyObject* self, PyObject* args) {
	struct benc_read br;
	br.offset = 0;

	if (!PyArg_ParseTuple(args, "s#", &(br.buffer), &(br.size)))
		return NULL;

	PyObject* obj = do_load(&br);

	return obj;
	//return Py_BuildValue("s#", bs.buffer, bs.offset);
}


static PyMethodDef cBencodeMethods[] = {
	{"loads", loads, METH_VARARGS, "loads"},
	{"dump", dump, METH_VARARGS, "Write the value on the open file."},
	{NULL, NULL, 0, NULL}
};

PyMODINIT_FUNC

initcBencode(void) {
	(void) Py_InitModule("cBencode", cBencodeMethods);
}
