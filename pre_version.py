"""
PlatformIO pre-script: inject git-describe as APP_VERSION_STRING.
Runs before every build of the waveshare_28b environment.
"""
Import("env")
import subprocess

try:
    v = subprocess.check_output(
        ["git", "describe", "--tags", "--always", "--dirty"],
        stderr=subprocess.DEVNULL,
    ).decode().strip()
except Exception:
    v = "unknown"

env.Append(CPPDEFINES=[("APP_VERSION_STRING", '\\"' + v + '\\"')])
