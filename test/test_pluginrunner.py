# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Copyright (C) 2011-2023 Red Hat, Inc.

import importlib.util
import os
import socket
import struct
import sys
import threading
import time
import types
import unittest

# Import python_binding/lsm/_pluginrunner.py (and the modules it needs)
# directly, bypassing lsm/__init__.py which pulls in the compiled _clib C
# extension.  Mirrors the approach in test_transport.py.
_lsm_src_dir = os.path.join(os.path.dirname(__file__), '..', 'python_binding',
                            'lsm')


def _load(name, filename):
    path = os.path.join(_lsm_src_dir, filename)
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


_lsm_pkg = types.ModuleType("lsm")
_lsm_pkg.__path__ = [_lsm_src_dir]
sys.modules.setdefault("lsm", _lsm_pkg)

_common = _load("lsm._common", "_common.py")
_load("lsm._data", "_data.py")
_transport = _load("lsm._transport", "_transport.py")

# _pluginrunner.py does "from lsm import LsmError, error, ErrorNumber", so the
# stub package must expose those names.
_lsm_pkg.LsmError = _common.LsmError
_lsm_pkg.error = _common.error
_lsm_pkg.ErrorNumber = _common.ErrorNumber

# It also does "from lsm.lsmcli import cmd_line_wrapper"; provide a stub so we
# don't drag in the CLI (and the C extension) it lives beside.
_lsmcli_stub = types.ModuleType("lsm.lsmcli")
_lsmcli_stub.cmd_line_wrapper = lambda *args, **kwargs: None
sys.modules["lsm.lsmcli"] = _lsmcli_stub
_lsm_pkg.lsmcli = _lsmcli_stub

_pluginrunner = _load("lsm._pluginrunner", "_pluginrunner.py")

PluginRunner = _pluginrunner.PluginRunner
TransPort = _transport.TransPort


class FakePlugin(object):
    """Minimal plug-in exposing just the methods these tests drive.  All
    methods accept **kwargs because PluginRunner calls them with the request's
    params dict and return JSON-serializable results."""

    def plugin_register(self, **kwargs):
        return None

    def plugin_unregister(self, **kwargs):
        return None

    def systems(self, **kwargs):
        return []


class TestPluginRunner(unittest.TestCase):
    def setUp(self):
        # Small deadline so the pre-auth case fires quickly.
        self._saved_timeout = _pluginrunner.REGISTRATION_TIMEOUT
        _pluginrunner.REGISTRATION_TIMEOUT = 0.3

    def tearDown(self):
        _pluginrunner.REGISTRATION_TIMEOUT = self._saved_timeout

    @staticmethod
    def _start_runner(plugin_sock):
        runner = PluginRunner(FakePlugin, ["fake_plugin",
                                           str(plugin_sock.fileno())])
        thread = threading.Thread(target=runner.run)
        thread.daemon = True
        thread.start()
        return thread

    def test_pre_auth_timeout_exits(self):
        """A client that connects but never sends plugin_register must not
        wedge the worker; run() has to exit once the pre-auth deadline
        fires."""
        plugin_sock, client_sock = socket.socketpair(socket.AF_UNIX,
                                                      socket.SOCK_STREAM)
        try:
            thread = self._start_runner(plugin_sock)
            # Send nothing at all from the client end.
            thread.join(timeout=3)
            self.assertFalse(
                thread.is_alive(),
                "plug-in did not exit after the pre-auth read timed out")
        finally:
            plugin_sock.close()
            client_sock.close()

    def test_post_register_leniency(self):
        """After plugin_register the timeout must be cleared so an idle gap
        longer than the pre-auth timeout does not kill an established
        session."""
        plugin_sock, client_sock = socket.socketpair(socket.AF_UNIX,
                                                     socket.SOCK_STREAM)
        # lsmd sets SO_RCVTIMEO on the socket before fork() as defence in
        # depth, so the worker starts with a kernel timeout it did not set
        # itself.  Reproduce that here: clearing the Python-level timeout
        # alone leaves this behind, and the idle gap below then fails the
        # read with BlockingIOError instead of simply waiting.
        timeout = _pluginrunner.REGISTRATION_TIMEOUT
        plugin_sock.setsockopt(
            socket.SOL_SOCKET, socket.SO_RCVTIMEO,
            struct.pack("@ll", int(timeout), int(timeout % 1 * 1000000)))
        # Guard against a hang if the worker wrongly exits mid-session.
        client_sock.settimeout(5)
        try:
            thread = self._start_runner(plugin_sock)
            client = TransPort(client_sock)

            client.rpc('plugin_register', {'uri': 'sim://',
                                           'plain_text_password': None,
                                           'timeout_ms': 1000,
                                           'flags': 0})

            # Idle longer than the (now-cleared) pre-auth timeout.
            time.sleep(_pluginrunner.REGISTRATION_TIMEOUT * 2)

            result = client.rpc('systems', {'search_key': None,
                                            'search_value': None,
                                            'flags': 0})
            self.assertEqual(result, [],
                             "established session was killed during idle gap")

            client.rpc('plugin_unregister', {'flags': 0})
            thread.join(timeout=3)
            self.assertFalse(thread.is_alive(),
                             "plug-in did not exit after plugin_unregister")
        finally:
            plugin_sock.close()
            client_sock.close()


if __name__ == "__main__":
    unittest.main()
