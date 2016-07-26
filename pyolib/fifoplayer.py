from pyolib._core import *
import Queue


class FIFOPlayer(PyoObject):
    # Objects generating vectors of samples must inherit from PyoObject.
    # Always provide a __doc__ string with the syntax below.
    """
    Directly feed a FIFO buffer with audio samples (from NumPy arrays).

    You can call put(x) to add to the buffer, which is implemented as a Python
    stdlib queue. FIFOPlayer will accept any objects supporting the Python
    Buffer protocol as long as the type of the elements is matching either
    float (32Bit) or double (64Bit) depending on the pyo variant
    (import pyo or pyo64). The number of elements in the array does not matter.
    FIFOPlayer is thread-save.

    Parentclass: PyoObject

    Parameters:

    maxsize : int
        The maximum number of objects in the queue until the next call to
        put(x) blocks. The size of each array does not matter. (Default 100)

    copy_free : Boolean
        If set to True (Default:False), the FIFOPlayer attemps to avoid memcpy
        and just sets the pointer to point into the memory of the array in the
        queue. This is slightly faster (however, getting an object from the
        queue is the bigger overhead). Even if copy_free is True, sometimes
        memcpy cannot be avoided if the size of an array is *not* a multiple
        of the buffer size of the server. Remaining samples have to be copied.
        Beware, `mul` and `add` (and any other) postprocessing will be ignored!

    Methods:

    put(x) : Feed x into the queue. The dtype of x must be correct but the
             size does not matter. put() actually adds a reference.
             Therefore, you should not change the contents of x, after putting
             it into the FIFOPlayer.
             If you use `import pyo` (the 32Bit version), make sure to
             use dtype=numpy.float32 or the numpy methond `.astype(np.float32)`
             on the numpy array.
             If the FIFOPlayer has multiple streams, you may use the keyword
             stream=i to put into the specific stream (this feature is
             experimental).

    Examples:

    >>> import pyo  # or pyo64 but then use np.float64 as the type of `a`
    >>> import threading  # for the sound generation thread
    >>>
    >>> s = pyo.Server()
    >>> s.boot()
    >>> fifo = pyo.FIFOPlayer().out()
    >>>
    >>> def proc(fifo, stopevent):
    >>>     import numpy as np
    >>>     freq = 500
    >>>     inc = 10
    >>>     while not stopevent.wait(0):
    >>>         a = np.cos(np.linspace(0,2*np.pi*freq,num=256*100))
    >>>         fifo.put(a.astype(np.float32))
    >>>         freq += inc
    >>>         if freq > 700:
    >>>             inc = -10
    >>>         elif freq < 500:
    >>>             inc = 10
    >>>
    >>> stopevent = threading.Event()
    >>> producer = threading.Thread(name="Compute audio signal", target=proc, args=[fifo, stopevent])
    >>> producer.start()
    >>>
    >>> s.gui(locals())
    >>> # Remember, the producer may be blocked at fifo.put, because the queue is full,
    >>> # so we set the stopevent and additionally, we have to remove one item from
    >>> # the queue, so that the last fifo.put() call returns.
    >>> stopevent.set()
    >>> fifo._base_objs[0]._queue.get_nowait()
    >>>


    """
    # Do not forget "mul" and "add" attributes.
    def __init__(self, maxsize=100, mul=1, add=0, always_memcpy=True):
        PyoObject.__init__(self)
        self._mul = mul
        self._add = add
        # Converts every arguments to lists (for multi-channel expansion).
        mul, add, always_memcpy, lmax = convertArgsToLists(mul, add, always_memcpy)
        # self._base_objs contains the list of XXX_base objects. Use "wrap" function to prevent "out of range" errors.
        self._base_objs = [FIFOPlayer_base(wrap(mul,i), wrap(add,i), wrap(always_memcpy,i)) for i in range(lmax)]
        for bo in self._base_objs:
            # One queue for each channel
            bo._queue = Queue.Queue(maxsize=maxsize, block=block, timeout=timeout)

    def put(self, x, stream=0,  block=True, timeout=None):
        self._base_objs[stream].put(x, block, timeout)


