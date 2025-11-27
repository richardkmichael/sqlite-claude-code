"""Tests for the SQLite Claude Code extension."""

import sqlite3
import pytest
import sqlite_claude_code


def test_load_extension():
    """Test that the extension loads successfully."""
    conn = sqlite3.connect(':memory:')
    conn.enable_load_extension(True)

    # Should not raise an exception
    sqlite_claude_code.load(conn)

    conn.close()


def test_projects_table_exists():
    """Test that the projects virtual table is created."""
    conn = sqlite3.connect(':memory:')
    conn.enable_load_extension(True)
    sqlite_claude_code.load(conn)

    # Check if projects table exists
    cursor = conn.execute(
        "SELECT name FROM sqlite_master WHERE type='table' AND name='projects'"
    )
    result = cursor.fetchall()

    assert len(result) == 1
    assert result[0][0] == 'projects'

    conn.close()


def test_sessions_table_exists():
    """Test that the sessions virtual table is created."""
    conn = sqlite3.connect(':memory:')
    conn.enable_load_extension(True)
    sqlite_claude_code.load(conn)

    # Check if sessions table exists
    cursor = conn.execute(
        "SELECT name FROM sqlite_master WHERE type='table' AND name='sessions'"
    )
    result = cursor.fetchall()

    assert len(result) == 1
    assert result[0][0] == 'sessions'

    conn.close()


def test_messages_table_exists():
    """Test that the messages virtual table is created."""
    conn = sqlite3.connect(':memory:')
    conn.enable_load_extension(True)
    sqlite_claude_code.load(conn)

    # Check if messages table exists
    cursor = conn.execute(
        "SELECT name FROM sqlite_master WHERE type='table' AND name='messages'"
    )
    result = cursor.fetchall()

    assert len(result) == 1
    assert result[0][0] == 'messages'

    conn.close()


def test_version():
    """Test that version is defined."""
    assert hasattr(sqlite_claude_code, '__version__')
    assert isinstance(sqlite_claude_code.__version__, str)
