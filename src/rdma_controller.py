import os
import asyncio
import sys
import subprocess

try:
    import aiohttp
except ImportError:
    print("Module 'aiohttp' is not found. Installing...")
    subprocess.check_call([sys.executable, "-m", "pip", "install", "aiohttp"])
    import aiohttp


# TODO: aiohttp server
