# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Copyright (C) 2026 Red Hat, Inc.

"""
The modules in this directory are standalone report-generation scripts
(run via ``__main__``), not pytest test cases; they only match pytest's
``test_*.py`` discovery pattern by accident.  They import third party
packages that are not part of this project's test requirements, so
collecting them aborts the whole run on a stock system.

Skip collection of a module only when one of its optional third party
imports is genuinely absent.  ``find_spec()`` asks whether the package
is installed without executing anything, so an ImportError raised from
our own code still fails loudly.
"""

from importlib.util import find_spec

# Module -> third party distributions it needs at import time.
_OPTIONAL_IMPORTS = {
    # test_automated imports test_hardware, so it inherits xlrd.
    'test_automated.py': ('yaml', 'xlrd'),
    'test_hardware.py': ('yaml', 'xlrd'),
    'test_results.py': ('yaml', 'bs4', 'htmltag'),
}

collect_ignore = [
    module for module, requires in _OPTIONAL_IMPORTS.items()
    if any(find_spec(name) is None for name in requires)
]
