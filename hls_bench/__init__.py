"""Shared helpers for local HLS benchmark runners.

This package contains small, tool-agnostic utilities used by the repository's
LegUp, Bambu/PANDA, and future Dynamatic batch runners.  The modules are kept
independent from any single command-line entry point so each runner can preserve
its existing behavior while emitting common reporting artifacts.
"""
