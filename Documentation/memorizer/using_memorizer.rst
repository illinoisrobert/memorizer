===============
Using Memorizer
===============

.. note::

  This section is a work in progress. The instructions
  and information in it might be incomplete or incorrect.

Memorizer is a tool to record information about access to kernel objects:
specifically, it counts memory accesses from distinct IP addresses in the
kernel source and also the PID that accessed, thereby providing spatial and
temporal dimensions.

The phases of gathering data from a specific experiement are:

* `Initial Conditions`_
* :ref:`Enabling Memorizer<memorizer_enable>`
* Running a `streaming experiment`_ or a `non-streaming experiment`_
* :ref:`Disabling Memorizer<memorizer_disable>`
* Gathering and transferring the data

.. note::
  This document will reference kernel command-line parameters and debugfs files.
  For more information, consult:

  * :ref:`kernel-parameters`
  * :ref:`debugfs-files`

Initial Conditions
==================

You should by now have compiled or acquired a Memorizer Linux kernel.
Ensure that the command-line parameters match the requirements
for your experiment::

  cat /proc/cmdline

See :ref:`kernel-cmdline` for more information about the kernel
command line parameters.

Some of the kernel command-line parameters that effect Memorizer are:

``memorizer_enabled_boot=(yes|no)``
    * ``yes`` - The memorizer data will include traces from early
      in the boot process.
    * ``no`` - The memorizer will not trace kernel allocation or
      accesss until specifically enabled by the user.
  
``mem_log_boot=(yes|no)`` 
  * ``yes`` - If ``memorizer_enabled_boot=yes``, then the Memorizer data
    will include information about memory accesses in the initial
    boot process.
  * ``no`` - The Memorizer will not trace memory accesses until
    specifically enabled by the user.

If the kernel command-line parameters are incorrect, adjust them in
your bootloader configuration (typical: ``/etc/default/grub``) and
reboot.

Memorizer's control and data files live in the debugfs filesystem at
``/sys/kernel/debug/memorizer/*``. If that directory does not
exist, please confirm:

* You are running a Memorizer kernel. Examine the output of ``uname -a``,
  and reboot as required.
* The debugfs filesystem is mounted. Examine the output of
  ``mount | grep debugfs``. If you need to mount the debugfs filesystem, 
  run ``mount -t debugfs debugfs /sys/kernel/debug`` as root.

If you are not interested in any previously-collected Memorizer data::

  echo 0 > /sys/kernel/debug/memorizer/memorizer_enabled
  echo 1 > /sys/kernel/debug/clear_dead_objects

This will disable Memorizer and remove all traces of previous allocations
that have completed their life cycle.

.. _memorizer_enable:
.. _memorizer_disable:

Enabling & Disabling Memorizer
==============================

Memorizer starts to gather data whenever ``memorizer_enabled`` has a
non-zero value.  The control file ``memorizer_enabled`` accepts
several different command values:

* `0` - turn off memorizer
* `1` - enable memorizer for the entire system
* `2` - enable memorizer for the current process, its subsequently
  created child processes, plus all interrupt contexts
* `3` - enable memorizer for the current process and its
  subsequently created child processes
* *pid* - enable memorizer for the indicated process and its
  subsequently created child processes

Note that writing ``0`` to ``memorizer_enabled`` turns off
the memorizer kernel object tracing immediately. Until some
other value is written, Memorizer gathers no data whatsoever.

Processes that were marked as memorizable are still marked; if one
renables memorizer, then those processes will be traced.

Non-streaming experiment
========================

Some process-specific non-streaming experiments run this::

  echo 3 > /sys/kernel/debug/memorizer/memorizer_enabled
  whatever-command-we-are-testing --foo bar
  echo 0 > /sys/kernel/debug/memorizer/memorizer_enabled
  ssh server sh -c "cat > /tmp/kmap" < /sys/kernel/debug/memorizer/kmap 

That code first enables memorizer's data-gathering function with ``echo 3``.
Whatever shell is running the ``echo`` command will be marked as
memorizable. All of that shell's subsequently created child processes 
will be similarly marked upon their creation.

Next the code performs your test. You can run almost any command
at this step.

The script 
disabled Memorizer with ``echo 0``. Otherwise, memorizer would continue
to gather data unrelated to the experiment. For example, the ``cat``
and ``ssh`` processes will both be marked memorizable.

Finally we capture the data and copy it to its final resting place.
You may copy the data out of the `kmap`
file any way you see fit. Here we are copying the data to a
file server.

.. note::

  Do not use ``scp`` to copy data directly from the debugfs filesystem.
  If you use ``scp``, you must buffer your data first, e.g.::

    cp /sys/kernel/debug/memorizer/kmap /tmp/kmap
    scp /tmp/kmap server:/tmp/kmap


Streaming experiment
====================

In contrast to the previous process, Memorizer can also
provide its kmap data in real time. Reading ``kmap_stream``
provides data about kernel objects that have completed their
life cycle. Additionally, reading ``kmap_stream`` will destroy
Memorizer's record of that kernel object, thus freeing
Memorizer memory to use for future kernel object tracing.

To gather data synchronously, one might do this::

  # "cat", "nc", and "ssh" are documented here. Any one of these will
  # trigger the streaming feature.
  cat > /tmp/kmap_data.txt < /sys/kernel/debug/memorizer/kmap_stream 

  #Client: nc <server> <port> < /sys/kernel/debug/memorizer/kmap_stream
  #Server: nc -l -k -p <port> > /tmp/kmap

  #ssh user@server sh -c "cat > /tmp/kmap_data.txt" < /sys/kernel/debug/memorizer/kmap_stream

  # With streaming started, run the experiment:
  sh -c "echo 3 > /sys/kernel/debug/memorizer_enabled && test-program"

This shell script enables the streaming feature by reading
from the file ``kmap_stream``.

The ``cat``, ``nc``, or ``ssh`` command
will run until interrupted because the ``kmap_stream`` file never indicates an
end-of-file condition. Instead, if there is no more data, the ``read()`` syscall
will block waiting for more data to appear.  To end the gathering and streaming of data,
you must interrupt the process reading from ``kmap_stream``.

.. note ::

  For streaming experiments, the ``memorizer_enabled`` mode `3` or `pid`
  works best. If you use mode `1`, then the memorizer data stream
  will include information from the streaming process (e.g. ``ssh``), which
  might overwhelm the streaming medium.

.. note ::

  Memorizer may have information about previously-allocated
  kernel objects.  If so, that data will be streamed immediately,
  regardless of the state of ``memorizer_enabled``.

The next command runs the experiment. Note that the process running `sh` will
be marked as memorizable, along with ``test-program`` and any processes
that ``test-program`` might spawn. Since the shell process is ephemeral
(it exits synchronously with ``test-program``'s exit), there will be
no memorizable processes when the command finishes.


Interpreting Results
====================

.. note ::

  See the :ref:`kmap file format <debugfs-kmap>` secion for more information.
