===============================
Quick Start for Ubuntu on Mkosi
===============================


This guide is a step-by-step set of instructions
for creating a Qemu-based Memorizer installation
using Mkosi_.

.. _Mkosi: https://github.com/systemd/mkosi


Environment
===========

We prepare an environment for downloading, building, and booting
a Memorizer Kernel into a Qemu VM. This document presumes you are running a Debian-based
Linux distro (such as Ubuntu) on your host.

These instructions presume that you are building using your
non-privileged host account. Any line not preceded by ``sudo``
should be run without super-user privilege.

Mkosi
-----

Install mkosi. You can install with ``git``, ``pip``, or ``apt``. 
For these instructions, it is important that you install version 14. Here
is one way to install mkosi::

  pip install git+https://github.com/systemd/mkosi.git@v14
  mkosi --version
  ~/.local/bin/mkosi --version
  /usr/local/bin/mkosi --version

``mkosi`` will likely be in one of the three indicated locations.

.. _`using the project's instructions`: https://github.com/systemd/mkosi?tab=readme-ov-file#installation

Qemu
----

Install Qemu::

  sudo apt install qemu-system-x86_64
  sudo adduser $USER kvm
  exit

You need to log out and log back in to apply the group id change.

Kernel Build Packages
---------------------

Using your host package manager, install the following packages::

  sudo apt update && sudo apt -y upgrade
  sudo apt install net-tools openssh-server build-essential

Note: build-essential is supposed to be all you 
need, or you can explicitly specify a list of packages. The following
list usually is sufficient::

  sudo apt install libncurses-dev gawk flex bison libssl-dev dkms \
  libelf-dev libudev-dev libpci-dev libiberty-dev autoconf llvm git \
  ncurses-dev libssl-dev autoconf-archive gnu-standards autoconf-doc \
  libtool gettext binutils-doc bison-doc debtags menu debian-keyring \
  flex-doc g++-multilib g++-12-multilib gcc-12-doc gawk-doc gcc-multilib \
  gcc-doc gcc-12-multilib gcc-12-locales glibc-doc doc-base tagcoll \
  lib32stdc++6-12-dbg libx32stdc++6-12-dbg gettext-doc autopoint \
  libasprintf-dev libgettextpo-dev``

Download Memorizer Source
=========================

The Memorizer source is available from ITI's Linux mirror, either https://code.iti.illinois.edu/ring0/memorizer or https://github.com/iti/memorizer.

In this example, we download Memorizer 25, which is based on Linux 6.6.30, from github. You can adjust version numbers as required.

::

  git clone git@github.com:iti/memorizer
  cd memorizer
  git checkout v6.6.30-memorizer-25



Build the Kernel
================

Start here, skipping the previous steps, if you have already done a
memorizer install once


Configure the kernel
--------------------

Apply the default configuration, followed by the updates from the memorizer configuration::

  make O=p defconfig
  make O=p memorizer.config

.. note::

  The ``O=p`` option directs ``make`` to deposit all of the built
  files into a subdirectory called ``p``. This is where the
  ``mkosi.conf`` files expect to find the Linux kernel image.

Confirm that CONFIG_FRAME_WARN is at least 2048::

  grep CONFIG_FRAME_WARN p/.config
  ./scripts/config --file p/.config --set-val CONFIG_FRAME_WARN 2048

If you are compiling the kernel on Ubuntu, you may receive the
following error that interrupts the building process::

      No rule to make target 'debian/canonical-certs.pem

If so, disable the conflicting security certificates::

      scripts/config --file p/.config --disable SYSTEM_TRUSTED_KEYS
      scripts/config --file p/.config --disable SYSTEM_REVOCATION_KEYS

Building the kernel
-------------------

``make`` may attempt to complete the
configuration process. If so,
accept default answers when prompted.

Adding ``–j`` makes it compile faster. In this case, ``make`` will use
twice as many concurrent processes as their are processing units in the system.

::

  make -j$(($(nproc) * 2)) O=p

Boot the Kernel
===============

We first use ``mkosi`` to build a root filesystem, and then to 
run the Memorizer kernel under Qemu.

Build the rootfs::

  cd scripts/memorizer/VM
  sudo mkosi build

Run memorizer::

  mkosi qemu

.. note::
  You may need to select *View -> serial0* to see the console output.

Congratulations! You should now have a running Memorizer kernel. 
See :doc:`using_memorizer` for the next steps.
