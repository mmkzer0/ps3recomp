"""ps3diff: the trace schema shared by the runtime, the tracer and the oracle.

`ps3diff.event/2` extends `rpcs3-gdb-trace.event/1`; see docs/PS3DIFF.md for
the contract and `schema.py` for the executable form of it.
"""

__all__ = ["Ps3DiffError"]

from .schema import Ps3DiffError
