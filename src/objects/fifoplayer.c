#include <Python.h>
#include "structmember.h"
#include <math.h>
#include <stdio.h> // for memcopy
#include "pyomodule.h"
#include "streammodule.h"
#include "servermodule.h"
#include "dummymodule.h"

#ifdef DEBUG
    #include <time.h>
    unsigned int GetTimeStamp() {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        return tv.tv_sec * (unsigned int)1000000 + tv.tv_usec;
    }
#endif

/*****************************************************
FIFOPlayer struct. Here comes every attributes required
for the proper operation of the object.
*****************************************************/
typedef struct {
    /* Mandatory macro intializing common properties of PyoObjects */
    pyo_audio_HEAD
    /* modebuffer keeps trace of the type of attributes that can
    be float or PyoObject. Processing function is selected to
    optimize operations. Need at least 2 slots for mul & add. */
    int modebuffer[3];
    /* Object's attributes. Attribute that can be float or PyoObject
    must be defined as PyObject and must be coupled with a Stream
    object (to hold the vector of samples of the audio signal). */
    // we have none

    PyObject *_queue; // a thread safe queue object (from Python's std-lib)
    Py_buffer current_buffer; // The current buffer (already taken out of the queue)
    int current_buffer_idx; // Track position in current_buffer
    int data_idx; // Track position in data
    MYFLT last_sample; // Value of the last sample to repeat it, if buffer runs empty
    MYFLT *backup_data; // When changing data pointer, back it up to here
    PyObject *python_str_get; // "get" string as Python object (we need it often)
    PyObject *python_str_put;
    int always_memcpy;
    /* Floating-point values must always use the MYFLT macro which
    handles float vs double builds. */
} FIFOPlayer;

/**********************************************************************
Processing functions. The last letter(s) of the name indicates the type
of object assigned to PyObject attribute, in this case "db" attribute.
"i" is for floating-point value and "a" is for audio signal.
**********************************************************************/
static void
FIFOPlayer_process_i(FIFOPlayer *self) {
    int total;              // number of samples in the self->current_buffer
    int available;          // number of samples not yet processed
    int needed = self->bufsize - self->data_idx; // number of items needed
    #ifdef DEBUG
        unsigned int t_start = GetTimeStamp(), t_delta;
    #endif

    // Do we have a current_buffer?
    if (self->current_buffer.obj != NULL) {
        assert(self->current_buffer.itemsize == sizeof(MYFLT));
        total = (int)self->current_buffer.len / (int)self->current_buffer.itemsize;
        available = total - self->current_buffer_idx;

        #ifdef DEBUG
            printf("--> Current_buffer: total=%i available=%i needed=%i.\n", total, available, needed);
        #endif

        // First, we let self->stream->data point to the chunk of mem that Pyo
        // allocated for us in the first place.
        if (self->backup_data != NULL) {
            self->stream->data = self->backup_data;
            self->backup_data = NULL;
        }

        if (available == 0) {
            // Clean-up if we are finished with the self->current_buffer

            #ifdef DEBUG
                unsigned int t_start_clean = GetTimeStamp(), t_delta_clean;
            #endif

            // PyBuffer_Release will also DECREF the object we got from the
            // self->_queue. We didn't store a reference to that object, but
            // the Py_Buffer->obj stores a reference and calling
            // PyBuffer_Release will Py_DECREF it.
            // Doc: http://docs.python.org/3.3/c-api/buffer.html
            PyBuffer_Release(&(self->current_buffer));
            self->current_buffer_idx = 0;
            total = 0;
            available = 0;
            // Note, don't (re)set self->data_idx here!! It's value is needed.

            #ifdef DEBUG
                t_delta_clean = GetTimeStamp() - t_start_clean;
                printf( "   * Clean up: PyBuffer_Release(). Now total=%i available=%i needed=%i (%u µs)\n",
                        total, available, needed, t_delta_clean);
            #endif

        } else if (self->always_memcpy == 0 && available >= self->bufsize && needed == self->bufsize) {
            // self->data buffer is empty and we have enough samples available
            // in the current_buffer so we can basically just let the
            // self->data pointer point into the right position in the
            // current_buffer.

            #ifdef DEBUG
                unsigned int t_start_pointer = GetTimeStamp(), t_delta_pointer;
            #endif

            // But first, we store the current self->data into
            // self->backup_data for later reuse.
            if (self->backup_data == NULL) {
                self->backup_data = self->data;
            }
            // Copy-free pointer reassignment. You may consider this a hack.
            // For some reason self->data does not work, because the Server uses
            // self->stream->data. So we have to change the self->stream->data pointer.
            self->stream->data = (MYFLT*)(self->current_buffer.buf) + self->current_buffer_idx;

            // Update some counters and last_sample
            self->current_buffer_idx += self->bufsize;
            available -= self->bufsize;
            needed -= self->bufsize;
            self->last_sample = self->data[self->bufsize-1];

            #ifdef DEBUG
                t_delta_pointer = GetTimeStamp() - t_start_pointer;
                printf( "   * Let self->data point to <%u>: total=%i available=%i needed=%i (%u µs)\n",
                        (unsigned int)self->stream->data, total, available, needed, t_delta_pointer);
            #endif

        } else {
            // Fall back to memcpy!

            #ifdef DEBUG
                unsigned int t_start_memcpy = GetTimeStamp(), t_delta_memcpy;
            #endif

            // Copy what is available but not more than what is needed.
            // memcpy operates on bytes!
            int n = (available < needed) ? available : needed;
            memcpy( self->data + self->data_idx, /* dest (pointer has right type) */
                    (MYFLT*)(self->current_buffer.buf) + self->current_buffer_idx, /* source */
                    n * sizeof(MYFLT));

            self->current_buffer_idx += n;
            self->data_idx += n;
            available -= n;
            needed -= n;
            self->last_sample = self->data[self->data_idx-1];

            #ifdef DEBUG
                t_delta_memcpy = GetTimeStamp() - t_start_memcpy;
                printf( "   * memcpy (%lu bytes (%i MYFLTs) to data_idx=%i): total=%i available=%i needed=%i (%u µs)\n",
                        n * sizeof(MYFLT), n, self->data_idx, total, available, needed, t_delta_memcpy);
            #endif
        }

        #ifdef DEBUG
            // available should not be negative!
            assert(available >= 0);
            // if there is more available, it should have been used up and needed == 0 !
            if (available > 0) { assert(needed == 0); }
        #endif

    } // end: if (self->current_buffer.obj != NULL)


    if (needed > 0 && self->current_buffer.obj == NULL) {
        // We have to get the next buffer from the queue (in non-blocking style)
        // because more samples are needed to be put into self->data ...

        #ifdef DEBUG
            unsigned int t_start_get = GetTimeStamp(), t_delta_get;
        #endif

        PyObject *ret = PyObject_CallMethodObjArgs( self->_queue,
                                                    self->python_str_get,
                                                    Py_False, /*non-blocking*/
                                                    NULL ); // NULL as last arg to flag the end
        if (ret) {
            // Python Buffer structure, so we don't need numpy's include files
            if (PyObject_GetBuffer(ret, &(self->current_buffer), PyBUF_CONTIG_RO ) != 0) {
                printf("ERROR: FIFOPlayer: Could not get C-continuous buffer from _queue.\n");
                PyErr_Print();
                exit(-3);
            }

            if (PyErr_Occurred()) {
                // Exporter may raise an err if it does not support PyBUF_CONTIG_RO
                printf("ERROR: FIFOPlayer exception during PyObject_GetBuffer...\n");
                PyErr_Print();
                exit(-1);
            }

            // The buffer's ndim (number of dimensions) must be equal to one
            if (self->current_buffer.ndim != 1) {
                printf("ERROR: FIFOPlayer: Given buffer has to be one-dimensional for audio!\n");
                exit(-4);
            }

            // Alright, we got a new self->current_buffer, so in a few lines
            // down we recursively call FIFOPlayer_process_i and we don't need
            // to update any of the vars, as they will be updated right after
            // the beginning of FIFOPlayer_process_i!

        }

        #ifdef DEBUG
            t_delta_get = GetTimeStamp() - t_start_get;
            if (self->current_buffer.obj != NULL) {
                printf( "   * Getting new Py_Buffer from queue: itemsize=%u, len=%u, ndim=%u (%u µs)\n",
                        (unsigned int)self->current_buffer.itemsize,
                        (unsigned int)(self->current_buffer.len/self->current_buffer.itemsize),
                        (unsigned int)self->current_buffer.ndim,
                        t_delta_get );
            }
        #endif
    } // enf of getting next buffer from queue


    if (needed > 0) {
        if (self->current_buffer.obj == NULL) {
            // The queue was empty, so we resort to play the last sample (constant output; no sound!)

            #ifdef DEBUG
                unsigned int t_start_const = GetTimeStamp(), t_delta_const;
            #endif

            int i;
            for (i = self->data_idx; i < self->bufsize; i++) {
                self->data[i] = self->last_sample;
            }

            #ifdef DEBUG
                t_delta_const = GetTimeStamp() - t_start_const;
                if (self->data_idx > 0 ) {
                    // Don't print this on empty (idle) loop
                    printf("   * Buffer empty. Const filling with last sample=%f (%u µs)\n", (double)self->last_sample, t_delta_const);
                }
            #endif

            self->data_idx = 0;

        } else {
            // recursive call to FIFOPlayer_process_i

            #ifdef DEBUG
                printf("--> Recursive call to FIFOPlayer_process_i to fill %i remaining samples...\n", needed);
            #endif

            // self->data_idx will be used to determine where to continue.
            FIFOPlayer_process_i(self);

        }
    } else {
        // The self->data buffer has been filled. Nothing more needed.

        #ifdef DEBUG
            t_delta = GetTimeStamp() - t_start;
            if (self->data_idx > 0) {
                // Don't print this on empty (idle) loop, so we check data_idx
                printf("==> Finished. The self->data buffer has been filled (%u µs).\n\n", t_delta);
            }
            // should not be negative!
            assert(needed >= 0);
        #endif

        self->data_idx = 0; // reset for next call
    }
}

/**********************************************************************
End of processing function definitions.
**********************************************************************/

/**********************************************************************
Post-Processing functions. These are functions where are applied "mul"
and "add" attributes. Macros are defined in pyomodule.h. Just keep them
and change the object's name.
**********************************************************************/
static void FIFOPlayer_postprocessing_ii(FIFOPlayer *self) { POST_PROCESSING_II };
static void FIFOPlayer_postprocessing_ai(FIFOPlayer *self) { POST_PROCESSING_AI };
static void FIFOPlayer_postprocessing_ia(FIFOPlayer *self) { POST_PROCESSING_IA };
static void FIFOPlayer_postprocessing_aa(FIFOPlayer *self) { POST_PROCESSING_AA };
static void FIFOPlayer_postprocessing_ireva(FIFOPlayer *self) { POST_PROCESSING_IREVA };
static void FIFOPlayer_postprocessing_areva(FIFOPlayer *self) { POST_PROCESSING_AREVA };
static void FIFOPlayer_postprocessing_revai(FIFOPlayer *self) { POST_PROCESSING_REVAI };
static void FIFOPlayer_postprocessing_revaa(FIFOPlayer *self) { POST_PROCESSING_REVAA };
static void FIFOPlayer_postprocessing_revareva(FIFOPlayer *self) { POST_PROCESSING_REVAREVA };

/**********************************************************************
setProcMode is called everytime a new value is assigned to one of the
object's attributes. Here are specified pointers to processing and
post-processing functions according to attribute types.
**********************************************************************/
static void
FIFOPlayer_setProcMode(FIFOPlayer *self)
{
    // FIFOPlayer only defines process_i
    self->proc_func_ptr = FIFOPlayer_process_i;

    int muladdmode;
    /* "muladdmode" swith statement should be left as is. */
    muladdmode = self->modebuffer[0] + self->modebuffer[1] * 10;

    switch (muladdmode) {
        case 0:
            self->muladd_func_ptr = FIFOPlayer_postprocessing_ii;
            break;
        case 1:
            self->muladd_func_ptr = FIFOPlayer_postprocessing_ai;
            break;
        case 2:
            self->muladd_func_ptr = FIFOPlayer_postprocessing_revai;
            break;
        case 10:
            self->muladd_func_ptr = FIFOPlayer_postprocessing_ia;
            break;
        case 11:
            self->muladd_func_ptr = FIFOPlayer_postprocessing_aa;
            break;
        case 12:
            self->muladd_func_ptr = FIFOPlayer_postprocessing_revaa;
            break;
        case 20:
            self->muladd_func_ptr = FIFOPlayer_postprocessing_ireva;
            break;
        case 21:
            self->muladd_func_ptr = FIFOPlayer_postprocessing_areva;
            break;
        case 22:
            self->muladd_func_ptr = FIFOPlayer_postprocessing_revareva;
            break;
    }
}

/**********************************************************************
"compute_next_data_frame" is the function called by the server on each
processing loop. Processing functions are executed first and then, the
post-processing function is called.
**********************************************************************/
static void
FIFOPlayer_compute_next_data_frame(FIFOPlayer *self)
{
    (*self->proc_func_ptr)(self);
    (*self->muladd_func_ptr)(self);
}

/**********************************************************************
Garbage-collector. Every PyObject and Stream objects must be added to
"traverse" and "clear" functions to allow the interpreter to clean memory
when ref count drop to zero. These functions must be registered as
"tp_traverse" and "tp_clear" in the object's PyTypeObject structure below.
Py_TPFLAGS_HAVE_GC flag must be added to the class flags (tp_flags).
**********************************************************************/
static int
FIFOPlayer_traverse(FIFOPlayer *self, visitproc visit, void *arg)
{
    pyo_VISIT
    Py_VISIT(self->python_str_put);
    Py_VISIT(self->python_str_get);
    Py_VISIT(self->_queue);
    return 0;
}

static int
FIFOPlayer_clear(FIFOPlayer *self)
{
    pyo_CLEAR
    Py_CLEAR(self->python_str_get);
    Py_CLEAR(self->python_str_put);
    Py_CLEAR(self->_queue);
    return 0;
}

/**********************************************************************
Deallocation function, registered as "tp_dealloc". Here must be freed
every malloc'ed and realloc'ed variables of the object.
**********************************************************************/
static void
FIFOPlayer_dealloc(FIFOPlayer* self)
{
    pyo_DEALLOC
    FIFOPlayer_clear(self);
    // in FIFOPlayer_new, we increased Py_False and Py_True (the global objects)
    // to pass them to _queue.get(False) and _queue.put(...,True) instead
    // of INCREFing and DECREFing them after each call.
    Py_DECREF(Py_False);
    Py_DECREF(Py_True);
    self->ob_type->tp_free((PyObject*)self);
}

/**********************************************************************
Function called at the object creation, registered as "tp_new".
**********************************************************************/
static PyObject *
FIFOPlayer_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    int i;
    PyObject *multmp=NULL, *addtmp=NULL, *always_memcpy=NULL;

    /* Object's allocation */
    FIFOPlayer *self;
    self = (FIFOPlayer *)type->tp_alloc(type, 0);

    /* Initialization of object's attributes */
    self->modebuffer[0] = 0;
    self->modebuffer[1] = 0;
    self->modebuffer[2] = 0; // so setProcMode will set self->proc_func_ptr to FIFOPlayer_process_i

    /* Initialization of common properties of PyoObject */
    INIT_OBJECT_COMMON

    /* Assign the stream's pointer to the object's processing callback. */
    /* The stream struct is what is registered in the server. */
    Stream_setFunctionPtr(self->stream, FIFOPlayer_compute_next_data_frame);
    /* Assign setProcMode to the common mode_func_ptr pointer */
    self->mode_func_ptr = FIFOPlayer_setProcMode;

    /* Object's keyword list. These are arguments to the object's creation */
    static char *kwlist[] = { "mul", "add", "copy_free", NULL};

    /* Argument parsing. If there is float values in the type list ("|OO"),
    a macro must be given to handle float vs double argument. See pyomodule.h
    for the list of macros already available. */
    if (! PyArg_ParseTupleAndKeywords(args, kwds, "|OOO", kwlist, &multmp, &addtmp, &always_memcpy))
        Py_RETURN_NONE;

    self->backup_data = NULL;
    self->current_buffer_idx = 0;
    self->data_idx = 0;
    self->last_sample = (MYFLT)0.0;
    self->python_str_get = PyString_FromString("get");
    self->python_str_put = PyString_FromString("put");
    self->always_memcpy = !PyObject_IsTrue(always_memcpy);
    // these two are used globally to pass as an argument,
    Py_INCREF(Py_False);
    Py_INCREF(Py_True);

    if (multmp) {
        PyObject_CallMethod((PyObject *)self, "setMul", "O", multmp);
    }

    if (addtmp) {
        PyObject_CallMethod((PyObject *)self, "setAdd", "O", addtmp);
    }

    /* Add the object's stream struct to the server registry */
    PyObject_CallMethod(self->server, "addStream", "O", self->stream);

    /* Call setProcMode to be sure pointers are correctly assigned */
    (*self->mode_func_ptr)(self);

    return (PyObject *)self;
}

/**********************************************************************
Functions common to almost all PyoObjects. The macros are defined in
pyomodule.h. Sometime the "out" function is removed to prevent sending
non-normalized signal (like the fft analysis of FFT object) to the soundcard.
Object dependant initializations can be added before PLAY, OUT and STOP macros.
**********************************************************************/
static PyObject * FIFOPlayer_getServer(FIFOPlayer *self) { GET_SERVER };
static PyObject * FIFOPlayer_getStream(FIFOPlayer *self) { GET_STREAM };
static PyObject * FIFOPlayer_setMul(FIFOPlayer *self, PyObject *arg) { SET_MUL };
static PyObject * FIFOPlayer_setAdd(FIFOPlayer *self, PyObject *arg) { SET_ADD };
static PyObject * FIFOPlayer_setSub(FIFOPlayer *self, PyObject *arg) { SET_SUB };
static PyObject * FIFOPlayer_setDiv(FIFOPlayer *self, PyObject *arg) { SET_DIV };

static PyObject * FIFOPlayer_play(FIFOPlayer *self, PyObject *args, PyObject *kwds) { PLAY };
static PyObject * FIFOPlayer_out(FIFOPlayer *self, PyObject *args, PyObject *kwds) { OUT };
static PyObject * FIFOPlayer_stop(FIFOPlayer *self) { STOP };

static PyObject * FIFOPlayer_multiply(FIFOPlayer *self, PyObject *arg) { MULTIPLY };
static PyObject * FIFOPlayer_inplace_multiply(FIFOPlayer *self, PyObject *arg) { INPLACE_MULTIPLY };
static PyObject * FIFOPlayer_add(FIFOPlayer *self, PyObject *arg) { ADD };
static PyObject * FIFOPlayer_inplace_add(FIFOPlayer *self, PyObject *arg) { INPLACE_ADD };
static PyObject * FIFOPlayer_sub(FIFOPlayer *self, PyObject *arg) { SUB };
static PyObject * FIFOPlayer_inplace_sub(FIFOPlayer *self, PyObject *arg) { INPLACE_SUB };
static PyObject * FIFOPlayer_div(FIFOPlayer *self, PyObject *arg) { DIV };
static PyObject * FIFOPlayer_inplace_div(FIFOPlayer *self, PyObject *arg) { INPLACE_DIV };

static PyObject *
FIFOPlayer_put(FIFOPlayer *self, PyObject *arg)
{
    if (arg == NULL) {
        Py_INCREF(Py_None);
        return Py_None;
    }

    // Does the given arg support the buffer protocol at all?
    if (!PyObject_CheckBuffer(arg)) {
        PyErr_SetString(PyExc_TypeError, "FIFOPlayer.put: Object with buffer protocol expected (e.g. str, numpy.array, ...)");
        return NULL;
    }

    // we pass True as second arg, so it will block until the queue has a free slot...
    PyObject* ret;
    Py_INCREF(Py_True);
    ret = PyObject_CallMethodObjArgs(self->_queue, self->python_str_put, arg, Py_True, NULL); // NULL as last arg to flag the end
    Py_DECREF(Py_True);
    if (!ret) {
        // todo: raise err or what? I dunno. queue.put(arg,True) should not fail
        PyErr_SetString(PyExc_TypeError, "FIFOPlayer.put: Object with buffer protocol expected (e.g. str, numpy.array, ...)");
        return NULL;
    }
    Py_DECREF(ret);

    Py_INCREF(Py_None);
    return Py_None;
}

/**********************************************************************
Object's members descriptors. Here should appear the server and stream
refs and also every PyObjects declared in the object's struct.
Registered as "tp_members".
**********************************************************************/
static PyMemberDef FIFOPlayer_members[] = {
{"server", T_OBJECT_EX, offsetof(FIFOPlayer, server), 0, "Pyo server."},
{"stream", T_OBJECT_EX, offsetof(FIFOPlayer, stream), 0, "Stream object."},
{"mul", T_OBJECT_EX, offsetof(FIFOPlayer, mul), 0, "Mul factor."},
{"add", T_OBJECT_EX, offsetof(FIFOPlayer, add), 0, "Add factor."},
{"_queue", T_OBJECT_EX, offsetof(FIFOPlayer, _queue), 0, "The Python Queue (from stdlib) for this channel."},
{NULL}  /* Sentinel */
};

/**********************************************************************
Object's method descriptors. Here should appear every methods needed to
be exposed to the python interpreter, casted to PyCFunction.
Registered as "tp_methods".
**********************************************************************/
static PyMethodDef FIFOPlayer_methods[] = {
{"getServer", (PyCFunction)FIFOPlayer_getServer, METH_NOARGS, "Returns server object."},
{"_getStream", (PyCFunction)FIFOPlayer_getStream, METH_NOARGS, "Returns stream object."},
{"play", (PyCFunction)FIFOPlayer_play, METH_VARARGS|METH_KEYWORDS, "Starts dbuting without sending sound to soundcard."},
{"out", (PyCFunction)FIFOPlayer_out, METH_VARARGS|METH_KEYWORDS, "Starts dbuting and sends sound to soundcard channel speficied by argument."},
{"stop", (PyCFunction)FIFOPlayer_stop, METH_NOARGS, "Stops dbuting."},
{"setMul", (PyCFunction)FIFOPlayer_setMul, METH_O, "Sets oscillator mul factor."},
{"setAdd", (PyCFunction)FIFOPlayer_setAdd, METH_O, "Sets oscillator add factor."},
{"setSub", (PyCFunction)FIFOPlayer_setSub, METH_O, "Sets inverse add factor."},
{"setDiv", (PyCFunction)FIFOPlayer_setDiv, METH_O, "Sets inverse mul factor."},
{"put", (PyCFunction)FIFOPlayer_put, METH_O, "Put a numpy array into the FIFO buffer."},
{NULL}  /* Sentinel */
};

/**********************************************************************
Number protocol struct. This allow to override the behaviour of mathematical
operations on the object. At the moment of the writing, only +, -, * and /,
and the corresponding "inplace" operations (a *= 1) are implemented. More
to comme in the future. Registered as "tp_as_number".
**********************************************************************/
static PyNumberMethods FIFOPlayer_as_number = {
(binaryfunc)FIFOPlayer_add,                           /*nb_add*/
(binaryfunc)FIFOPlayer_sub,                           /*nb_subtract*/
(binaryfunc)FIFOPlayer_multiply,                      /*nb_multiply*/
(binaryfunc)FIFOPlayer_div,                           /*nb_divide*/
0,                                              /*nb_remainder*/
0,                                              /*nb_divmod*/
0,                                              /*nb_power*/
0,                                              /*nb_neg*/
0,                                              /*nb_pos*/
0,                                              /*(unaryfunc)array_abs,*/
0,                                              /*nb_nonzero*/
0,                                              /*nb_invert*/
0,                                              /*nb_lshift*/
0,                                              /*nb_rshift*/
0,                                              /*nb_and*/
0,                                              /*nb_xor*/
0,                                              /*nb_or*/
0,                                              /*nb_coerce*/
0,                                              /*nb_int*/
0,                                              /*nb_long*/
0,                                              /*nb_float*/
0,                                              /*nb_oct*/
0,                                              /*nb_hex*/
(binaryfunc)FIFOPlayer_inplace_add,                   /*inplace_add*/
(binaryfunc)FIFOPlayer_inplace_sub,                   /*inplace_subtract*/
(binaryfunc)FIFOPlayer_inplace_multiply,              /*inplace_multiply*/
(binaryfunc)FIFOPlayer_inplace_div,                   /*inplace_divide*/
0,                                              /*inplace_remainder*/
0,                                              /*inplace_power*/
0,                                              /*inplace_lshift*/
0,                                              /*inplace_rshift*/
0,                                              /*inplace_and*/
0,                                              /*inplace_xor*/
0,                                              /*inplace_or*/
0,                                              /*nb_floor_divide*/
0,                                              /*nb_true_divide*/
0,                                              /*nb_inplace_floor_divide*/
0,                                              /*nb_inplace_true_divide*/
0,                                              /* nb_index */
};

/**************************************************************
Object's type declaration. The type's name should be "XXXType",
where XXX is replaced by the name of the object.
Fields in PyTypeObject that are not used should be 0.
**************************************************************/
PyTypeObject FIFOPlayerType = {
PyObject_HEAD_INIT(NULL)
0,                                              /*ob_size*/
/* How the object will be exposed to the
python interpreter. The name of the C component
of a PyoObject should be "XXX_base", where XXX
is replaced by the name of the object. These
objects are actually never directly created
by the user. They are used inside the object's
Python class to handle multi-channel expansion.*/
"_pyo.FIFOPlayer_base",                      /*tp_name*/
sizeof(FIFOPlayer),                                  /*tp_basicsize*/
0,                                              /*tp_itemsize*/
(destructor)FIFOPlayer_dealloc,                       /*tp_dealloc*/
0,                                              /*tp_print*/
0,                                              /*tp_getattr*/
0,                                              /*tp_setattr*/
0,                                              /*tp_dbare*/
0,                                              /*tp_repr*/
&FIFOPlayer_as_number,                          /*tp_as_number*/
0,                                              /*tp_as_sequence*/
0,                                              /*tp_as_mapping*/
0,                                              /*tp_hash */
0,                                              /*tp_call*/
0,                                              /*tp_str*/
0,                                              /*tp_getattro*/
0,                                              /*tp_setattro*/
0,                                              /*tp_as_buffer*/
Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_CHECKTYPES, /*tp_flags*/
"FIFOPlayer C-implementation. Plays audio data as raw samples from numpy arrays.\n"
"Other objects that support the Python buffer protocol, are valid, too, as long as the element type matches.\n"
"For pyo", /* tp_doc */
(traverseproc)FIFOPlayer_traverse,                    /* tp_traverse */
(inquiry)FIFOPlayer_clear,                            /* tp_clear */
0,                                              /* tp_richdbare */
0,                                              /* tp_weaklistoffset */
0,                                              /* tp_iter */
0,                                              /* tp_iternext */
FIFOPlayer_methods,                                   /* tp_methods */
FIFOPlayer_members,                                   /* tp_members */
0,                                              /* tp_getset */
0,                                              /* tp_base */
0,                                              /* tp_dict */
0,                                              /* tp_descr_get */
0,                                              /* tp_descr_set */
0,                                              /* tp_dictoffset */
0,                                              /* tp_init */
0,                                              /* tp_alloc */
FIFOPlayer_new,                                       /* tp_new */
};
