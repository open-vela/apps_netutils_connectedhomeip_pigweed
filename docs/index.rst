.. _docs-root:
.. highlight:: sh

.. TODO: b/256680603 - Remove query string from issue tracker link.

.. toctree::
  :maxdepth: 1
  :hidden:

  Home <self>
  docs/overview
  docs/get_started/index
  docs/concepts/index
  targets
  Modules <module_guides>
  docs/module_structure
  changelog
  Mailing List <https://groups.google.com/forum/#!forum/pigweed>
  Chat Room <https://discord.gg/M9NSeTA>
  docs/os/index
  docs/size_optimizations
  Code Editor Support <docs/editors>
  third_party_support
  Source Code <https://cs.pigweed.dev/pigweed>
  Code Reviews <https://pigweed-review.googlesource.com>
  Issue Tracker <https://issues.pigweed.dev/issues?q=status:open>
  docs/contributing/index
  docs/infra/index
  Automated Analysis <automated_analysis>
  Build System <build_system>
  SEEDs <seed/0000-index>
  kudzu/docs
  Eng Blog <docs/blog/index>

=======
Pigweed
=======
Pigweed is an open source collection of embedded-targeted libraries--or as we
like to call them, :ref:`modules <docs-glossary-module>`. These modules are
building blocks and infrastructure that enable faster and more reliable
development on small-footprint MMU-less 32-bit microcontrollers like the
STMicroelectronics STM32L452 or the Nordic nRF52832.

.. attention::

   Pigweed is in **early access**; though many modules are shipping in
   production already. If you're interested in using Pigweed, please reach out
   in our `chat room <https://discord.gg/M9NSeTA>`_ or on the `mailing list
   <https://groups.google.com/forum/#!forum/pigweed>`_.

--------------------------
Talk to us at Pigweed Live
--------------------------
.. pigweed-live::

---------------------
What's New In Pigweed
---------------------
.. include:: changelog.rst
   :start-after: .. changelog_highlights_start
   :end-before: .. changelog_highlights_end

See :ref:`docs-changelog-latest` in our changelog for details.

---------------
Getting Started
---------------
Check out our :ref:`docs-get-started` landing page. We've got a guide that
shows you how to use Pigweed in a new, Bazel-based project (the recommended
path), sample code for GN-based projects, a tutorial on getting set up to
contribute to upstream Pigweed, and more.

------------------------
What does Pigweed offer?
------------------------
There are many modules in Pigweed; this section showcases a selection that
produce visual output. For more information about the different Pigweed module
offerings, refer to :ref:`docs-module-guides` section.

``pw_watch`` - Build, flash, run, & test on save
================================================
In the web development space, file system watchers are prevalent. These
watchers trigger a web server reload on source change, making development much
faster. In the embedded space, file system watchers are less prevalent;
however, they are no less useful! The Pigweed watcher module makes it easy to
instantly compile, flash, and run tests upon save. Combined with the GN-based
build which expresses the full dependency tree, only the exact tests affected
by a file change are run on saves.

The demo below shows :ref:`module-pw_watch` building for a STMicroelectronics
STM32F429I-DISC1 development board, flashing the board with the affected test,
and verifying the test runs as expected. Once this is set up, you can attach
multiple devices to run tests in a distributed manner to reduce the time it
takes to run tests.

.. image:: https://storage.googleapis.com/pigweed-media/pw_watch_on_device_demo.gif
  :width: 800
  :alt: pw watch running on-device tests

``pw_presubmit`` - Vacuum lint on every commit
==============================================
Presubmit checks are essential tools, but they take work to set up, and
projects don’t always get around to it. The :ref:`module-pw_presubmit` module
provides tools for setting up high quality presubmit checks for any project. We
use this framework to run Pigweed’s presubmit on our workstations and in our
automated building tools.

The ``pw_presubmit`` module includes ``pw format``, a tool that provides a
unified interface for automatically formatting code in a variety of languages.
With ``pw format``, you can format C, C++, Python, GN, and Go code according to
configurations defined by your project. ``pw format`` leverages existing tools
like ``clang-format``, and it’s simple to add support for new languages.

.. image:: https://storage.googleapis.com/pigweed-media/pw_presubmit_demo.gif
  :width: 800
  :alt: pw presubmit demo

``pw_env_setup`` - Cross platform embedded compiler setup
=========================================================
A classic problem in the embedded space is reducing the **time from git clone
to having a binary executing on a device**. An entire suite of tools is needed
for building non-trivial production embedded projects, and need to be
installed. For example:

- A C++ compiler for your target device, and also for your host
- A build system or three; for example, GN, Ninja, CMake, Bazel
- A code formatting program like clang-format
- A debugger like OpenOCD to flash and debug your embedded device
- A known Python version with known modules installed for scripting
- A Go compiler for the Go-based command line tools
- ... and so on

In the server space, container solutions like Docker or Podman solve this;
however, container solutions are a mixed bag for embedded systems development
where one frequently needs access to native system resources like USB devices,
or must operate on Windows.

:ref:`module-pw_env_setup` is our compromise solution for this problem that
works on Mac, Windows, and Linux. It leverages the Chrome Infrastructure
Packaging Deployment system (CIPD) to bootstrap a Python installation, which in
turn inflates a virtual environment. The tooling is installed into your
workspace, and makes no changes to your system. This tooling is designed to be
reused by any project.

.. image:: https://storage.googleapis.com/pigweed-media/pw_env_setup_demo.gif
   :width: 800
   :alt: pw environment setup demo

``pw_unit_test`` - Embedded testing for MCUs
============================================
Unit testing is important, and Pigweed offers a portable library that’s broadly
compatible with `Google Test <https://github.com/google/googletest>`_. Unlike
Google Test, :ref:`module-pw_unit_test` is built on top of embedded friendly
primitives; for example, it does not use dynamic memory allocation.
Additionally, it is easy to port to new target platforms by implementing the
`test event handler interface <https://cs.pigweed.dev/pigweed/+/main:pw_unit_test/public/pw_unit_test/event_handler.h>`_.

Like other modules in Pigweed, ``pw_unit_test`` is designed for use in
established codebases with their own build system, without the rest of Pigweed
or the Pigweed integrated GN build. However, when combined with Pigweed's
build, the result is a flexible and powerful setup that enables easily
developing code on your desktop (with tests), then running the same tests
on-device.

.. image:: https://storage.googleapis.com/pigweed-media/pw_status_test.png
   :width: 800
   :alt: pw_status test run natively on host

And more!
=========
Here is a selection of interesting modules:

.. grid:: 3

   .. grid-item-card:: :octicon:`cpu` pw_cpu_exception
      :link: module-pw_cpu_exception_cortex_m
      :link-type: ref

      An interface for entering CPU exception handlers. Includes robust low
      level hardware fault handling for ARM Cortex-M; the handler even has unit
      tests written in assembly to verify nested-hardware-fault handling!

   .. grid-item-card:: :octicon:`code-square` pw_polyfill
      :link: module-pw_polyfill
      :link-type: ref

      This module makes it easier to work with different C++ standards in one
      codebase.

   .. grid-item-card:: :octicon:`container` pw_tokenizer
      :link: module-pw_tokenizer
      :link-type: ref

      Replace string literals from log statements with 32-bit tokens, to reduce
      flash use, reduce logging bandwidth, and save formatting cycles from log
      statements at runtime.

.. grid:: 3

   .. grid-item-card:: :octicon:`database` pw_kvs
      :link: module-pw_kvs
      :link-type: ref

      A key-value-store implementation for flash-backed persistent storage with
      integrated wear levelling. This is a lightweight alternative to a file
      system for embedded devices.

   .. grid-item-card:: :octicon:`paper-airplane` pw_protobuf
      :link: module-pw_protobuf
      :link-type: ref

      An early preview of our wire-format-oriented protocol buffer
      implementation. This protobuf compiler makes a different set of
      implementation tradeoffs than the most popular protocol buffer library in
      this space, nanopb.

   .. grid-item-card:: :octicon:`device-desktop` pw_console
      :link: module-pw_console
      :link-type: ref

      Interactive Python repl and log viewer designed to be a a complete
      solution for interacting with hardware devices using :ref:`module-pw_rpc`
      over a :ref:`module-pw_hdlc` transport.


See the :ref:`docs-module-guides` for the complete list of modules and their
documentation.
