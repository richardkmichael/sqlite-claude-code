"""SQLite extension for querying Claude Code conversation history."""

from contextlib import contextmanager
import os
import platform
import sqlite3
from pathlib import Path

__version__ = "0.1.0"


class SqliteClaudeCodeError(Exception):
    """Error loading SQLite Claude Code extension."""
    pass


@contextmanager
def connect(database=':memory:'):
    """
    Create a SQLite database connection with the Claude Code extension loaded.

    This is a context manager that automatically closes the connection when done.

    Args:
        database: Path to database file or ':memory:' for in-memory database (default)

    Yields:
        sqlite3.Connection: Database connection with extension loaded

    Raises:
        SqliteClaudeCodeError: If the extension cannot be loaded

    Example:
        >>> import sqlite_claude_code
        >>> with sqlite_claude_code.connect() as conn:
        ...     sessions = conn.execute("SELECT COUNT(*) FROM sessions").fetchone()[0]
        ...     print(f"Found {sessions} sessions")
    """
    conn = sqlite3.connect(database)
    try:
        load(conn)
        yield conn
    finally:
        conn.close()


def load(connection):
    """
    Load the Claude Code SQLite extension into a database connection.

    Args:
        connection: A sqlite3.Connection object

    Raises:
        SqliteClaudeCodeError: If the extension cannot be loaded

    Example:
        >>> import sqlite3
        >>> import sqlite_claude_code
        >>> conn = sqlite3.connect(':memory:')
        >>> conn.enable_load_extension(True)
        >>> sqlite_claude_code.load(conn)
        >>> conn.enable_load_extension(False)
    """
    # Enable extension loading
    was_enabled = connection.enable_load_extension(True)

    try:
        extension_path = _find_extension()
        connection.load_extension(extension_path)
    finally:
        if not was_enabled:
            connection.enable_load_extension(False)


def _find_extension():
    """Find the compiled SQLite extension for the current platform."""
    # Get the directory where this module is installed
    module_dir = Path(__file__).parent

    # Platform-specific extension names
    # Note: Extension is named "claude_code" (not sqlite_claude_code) to match
    # the entry point sqlite3_claudecode_init in init.c
    system = platform.system()
    if system == "Darwin":
        ext_names = ["claude_code.dylib", "claude_code.so"]
    elif system == "Linux":
        ext_names = ["claude_code.so"]
    elif system == "Windows":
        ext_names = ["claude_code.dll"]
    else:
        ext_names = ["claude_code.so", "claude_code.dylib"]

    # Try to find the extension
    for ext_name in ext_names:
        ext_path = module_dir / ext_name
        if ext_path.exists():
            return str(ext_path)

    raise SqliteClaudeCodeError(
        f"Could not find compiled extension in {module_dir}. "
        f"Looked for: {', '.join(ext_names)}"
    )


__all__ = ["connect", "load", "SqliteClaudeCodeError", "__version__"]
