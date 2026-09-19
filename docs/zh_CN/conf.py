#
# Chinese Language RTD & Sphinx config file
#
# Uses ../conf_common.py for most non-language-specific settings.
#

import datetime
import os
import sys

try:
    from conf_common import *  # noqa: F401,F403
except ImportError:
    sys.path.insert(0, os.path.abspath("../"))
    from conf_common import *  # noqa: F401,F403

project = "MiniHub"
copyright = f"2026 - {datetime.datetime.now(tz=datetime.timezone.utc).year}, MiniHub Contributors"
pdf_title = "MiniHub 编程指南"
language = "zh_CN"
