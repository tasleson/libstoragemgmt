/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Copyright (C) 2011-2023 Red Hat, Inc.
 *
 * Author: Tony Asleson <tasleson@redhat.com>
 */

#ifndef LSM_IPC_TIMEOUT_H
#define LSM_IPC_TIMEOUT_H

/* Receive timeout (seconds) enforced on a plug-in's IPC socket while the
 * client has not yet completed plugin_register. Shared by the daemon
 * (defense-in-depth SO_RCVTIMEO set before fork()) and the C plug-in runner;
 * kept consistent with INITIAL_RECV_TIMEOUT in
 * python_binding/lsm/_pluginrunner.py. Cleared (0) once the client
 * registers, so slow post-registration operations are never interrupted. */
#define LSM_PLUGIN_INITIAL_RECV_TIMEOUT_SECONDS 30

#endif
