.. SPDX-License-Identifier: GPL-2.0-only

===========================
Qualcomm QSEECOM TEE support
===========================

This driver exposes the legacy, command-buffer QSEE application interface
through the Linux TEE subsystem. It is distinct from the object-based
smcinvoke interface. A platform may need both interfaces for different
firmware applications.

Enable CONFIG_TEE_QSEECOM together with QCOM_QSEECOM and QCOM_MDT_LOADER.
The driver provides a client device, /dev/teeX, and a privileged supplicant
device, /dev/teeprivX. Identify the implementation through TEE_IOC_VERSION
and TEE_IMPL_ID_QSEECOM rather than assuming a fixed device number.

Client sessions
===============

QSEE applications have string names, not GlobalPlatform UUIDs. Opening a
client session requires a zero UUID and one input memref containing a
NUL-terminated application name. The driver does not advertise
TEE_GEN_CAP_GP. The name is resolved against resident applications or the
registry of applications loaded by this driver.

Invocations carry an input command memref and an output response memref.
Additional value/memref pairs can describe addresses to patch into the
command. Application-specific commands and response contents are opaque to
the kernel. Shared TEE memory is copied into kernel-controlled qcom_tzmem
staging buffers before a secure call; secure physical addresses are not
exposed as userspace mappings.

Supplicant sessions
===================

The privileged device requires CAP_SYS_ADMIN. It supports loading an
application by a name resolved through request_firmware(), and registering
listener services with shared request buffers. Listener work is received
with TEE_IOC_SUPPL_RECV and answered with TEE_IOC_SUPPL_SEND. Request
generations and receiver ownership associate each answer with its request.
Secure calls are serialized and listener waits have a bounded timeout.

Loading a named firmware application and registering a listener are separate
operations. Userspace must provide the listeners required by its trusted
applications, including their storage protocol where applicable. A failure
to withdraw a listener while TrustZone may still access its buffer retains
the allocation rather than releasing memory reachable by the secure world.

Access and integration
======================

Restrict both device nodes to trusted userspace. The client interface cannot
validate application commands, credentials or their meaning. Opening a
client device is therefore permission to invoke the applications exposed by
this transport, not an application-specific authorization boundary.

Fingerprint enrolment and matching still require the trusted applications'
normal authorization chain. This driver neither generates authentication
tokens nor performs biometric matching. Firmware payloads, credentials and
biometric databases are supplied and managed separately from kernel source.

ABI status
==========

This driver follows an unmerged upstream RFC (Dawid Wróbel,
https://github.com/wrobelda/linux/tree/qcom-qseecom-tee). Its user-visible
ABI is therefore provisional:

- ``TEE_IMPL_ID_QSEECOM`` is 5, the value the RFC takes after
  ``TEE_IMPL_ID_QTEE`` (4) in v6.18. Value 4 is not defined in this tree.
  Clients must match on the ``impl_id`` reported by ``TEE_IOC_VERSION`` and
  must be updated together with the kernel if upstream assigns another value.
- Applications are named by a memref holding a NUL-terminated string with a
  zero UUID, and the privileged device loads by name and registers listeners
  through ``TEE_IOC_OPEN_SESSION``. Neither convention is part of the upstream
  TEE ABI yet.
- Supplicant teardown relies on the ``close_context`` driver operation, which
  is a backport of the v6.18 TEE core change and carries no ABI of its own.
