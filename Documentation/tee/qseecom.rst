.. SPDX-License-Identifier: GPL-2.0-only

===========================
Qualcomm QSEECOM TEE support
===========================

This driver exposes the legacy, command-buffer QSEE application interface
through the Linux TEE subsystem. It is distinct from the object-based
smcinvoke interface. A platform may need both interfaces for different
firmware applications.

Set CONFIG_TEE_QSEECOM to y or m together with QCOM_QSEECOM and
QCOM_MDT_LOADER to opt in to this interface. Once SCM detects QSEECOM, it
registers the separate qcom_qseecom_tee platform device even on machines
outside the older in-kernel client allowlist. That allowlist still applies
to the existing qcom_qseecom clients.

With CONFIG_TEE_QSEECOM=n, the existing SCM path retains its original lock
scope, warnings for incomplete or blocked calls, and return handling. The
new listener and application load/shutdown helpers are disabled. Both y and
m enable these SCM extensions so that a module can use the exported helpers.

The driver provides a client device, /dev/teeX, and a privileged supplicant
device, /dev/teeprivX. Identify the implementation through TEE_IOC_VERSION
and TEE_IMPL_ID_QSEECOM rather than assuming a fixed device number. Opening
both devices requires CAP_SYS_ADMIN. This is a coarse privilege boundary;
it does not validate opaque TA commands or isolate their secure-storage effects.

Origin and ABI status
=====================

The QSEECOM TEE core originates from Dawid Wróbel's external prototype at
https://github.com/wrobelda/linux/tree/qcom-qseecom-tee, revision
d9e76eac637c8cdd967388bcf4bf46068efb2d3e. The source for that revision is
https://github.com/wrobelda/linux/blob/d9e76eac637c8cdd967388bcf4bf46068efb2d3e/drivers/tee/qseecom/core.c.
The core adaptations for this kernel base add linux/mm.h and replace
kzalloc_obj() with kzalloc(sizeof(...), GFP_KERNEL). Both device opens also
require CAP_SYS_ADMIN locally. The original copyright is retained; SCM gating
and board integration are local changes.

TEE_IMPL_ID_QSEECOM=5 and the application-name session convention belong to
that external proposed ABI. The value is not a Linux upstream assignment.
Upstream QTEE's implementation ID 4 identifies the separate object-based
interface; it does not assign an ID to this QSEECOM prototype.

The TEE close_context callback field and its dispatch before final context
release are backported from Linux commit
0cbaf65c91db0e40a577e8919979dac1963cfcc0, authored by Amirreza Zarrabi:
https://github.com/torvalds/linux/commit/0cbaf65c91db0e40a577e8919979dac1963cfcc0.
Only the callback support needed by this driver is carried here.

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

Hardware ownership
==================

The legacy SENSORS_FPC1020_SPI driver is an opt-in diagnostic transport. It
directly powers and resets the sensor and performs SPI transfers during
probe. It must only bind when Linux owns the bus, pin configuration, supply
and reset lines. A trusted application that owns the same hardware cannot
safely share it with that driver without an established ownership protocol.

The liuqin device tree retains its base GPIO reservations, including GPIOs
36--39, and does not enable the experimental SPI10 fingerprint node, its
LDO9 supply, or its reset/IRQ pinctrl entries. This leaves the raw diagnostic
driver unconnected to the release board description. Enabling sensor control
for the OEM userspace path requires separate ownership evidence and device
acceptance before board wiring is added.

Leaving the node disabled prevents this diagnostic driver from probing via
the release device tree. The cause of the historical GENI corruption reports
has not been proven, and successful boots or fingerprint operations do not
by themselves resolve it. Existing device-tree and runtime observations of
sensor properties and supply do not establish HLOS ownership of SE10's
secure hardware.

OEM GPIO control
================

SENSORS_FPC1264_TEE is the optional OEM control path. It exposes the existing
/dev/fpc1020 open/close and poll interface using a platform device with a
vdd-supply, active-low reset-gpios and a sensor interrupt. It does not claim a
SPI controller or pins, issue SPI transfers, read a hardware ID, change SE10
ownership or request SPI clocks. Matching and sensor communication remain in
the trusted application. The diagnostic SPI driver is not its product module.

This split follows the resource model of Xiaomi's published liuqin-t-oss
platform driver and board description:

* https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/eb49e10028e27fa98856cb5e458c0f0ec5b4554e/drivers/input/fingerprint/fpc_ta/fpc1020_tee.c
* https://github.com/MiCode/kernel_devicetree/blob/1c507816ba8aa6117dcec41f577ef7a66d2091f7/qcom/liuqin-sm8475.dtsi

The vendor board uses reset GPIO41 and IRQ GPIO40. Its L9C voltage is 3.3 V;
that value is not adopted here. The accepted local liuqin supply constraint
remains 2960000--3008000 microvolts. These sources establish a separate GPIO
resource model, not the internals of the resident trusted application's SPI
ownership protocol. The product DTS remains unchanged.

A temporary, separately applied GPIO description was exercised with this
control driver on the previously accepted stable liuqin kernel and ramdisk.
The booted tree retained both reserved GPIO ranges and kept the Linux SPI10
controller disabled. Actual GNOME unlock, unlock after fprintd restart and
other-finger rejection succeeded; the user also confirmed automatic rotation.
Several same-finger attempts returned a real non-match, so this is functional
evidence rather than a reliability or latency claim. This observation does
not accept the complete revised kernel/product-root combination. Paired
product artifacts, cold boot and suspend/resume remain unverified.
