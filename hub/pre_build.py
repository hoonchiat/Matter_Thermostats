"""
Redirect the IDF Component Manager cache to C:\\ecp so that the CHIP SDK
paths inside esp-matter stay under Windows MAX_PATH (260 chars).
"""
import os
Import("env")  # noqa: F821 — injected by PlatformIO
os.environ.setdefault("IDF_COMPONENT_CACHE_PATH", "C:\\ecp")
