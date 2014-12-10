Callgrind collect-openclose
===========================
overview
--------
Callgrind with a simple patch, add option ``--collect-openclose``.

``--collect_openclose`` is to counting Ir between first time opening testcase and last time closing output file.

It is used in online judge system, we use callgrind to measuring algorithm performance (use cpu instruction).

``--collect_openclose`` option can avoid to counting too much cost at VM initialization(ex. jvm), because it has nothing to do with algorithm performance.

detail
------
new options
+++++++++++
- ``--collect_openclose=<yes|no>``

  + open functionality of this patch.

- [TODO] ``--collect-openfile=<filename>``

- [TODO] ``--collect-closefile=<filename>``

building step
+++++++++++++
same as valgrind. 

refer to `build valgrind tool`_::

    ./autogen.sh    
    ./configure --prefix=`pwd`/inst
    make    
    make install
    
The executable is ``inst/bin/valgrind``.

testing
-------
see ``cg_patch_test/testing.sh``.

.. _build valgrind tool: http://valgrind.org/docs/manual/manual-writing-tools.html#manual-writing-tools.gettingstarted
