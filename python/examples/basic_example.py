#!/usr/bin/env python3
"""Basic example of using sqlite-claude-code."""

import sqlite_claude_code


def main():
    # Create an in-memory database with extension loaded
    # The context manager automatically closes the connection when done
    with sqlite_claude_code.connect() as conn:
        print("Extension loaded successfully!")
        print()

        # Query projects
        print("=== Projects ===")
        cursor = conn.execute("SELECT * FROM projects LIMIT 5")
        for row in cursor:
            print(row)
        print()

        # Query sessions
        print("=== Sessions (first 5) ===")
        cursor = conn.execute("SELECT session_id, project_id FROM sessions LIMIT 5")
        for row in cursor:
            print(f"Session: {row[0][:8]}..., Project: {row[1]}")
        print()

        # Query messages
        print("=== Messages (first 10) ===")
        cursor = conn.execute("SELECT type, message_id FROM messages LIMIT 10")
        for row in cursor:
            print(f"Type: {row[0]}, Message ID: {row[1][:16]}...")
        print()

        # Count messages by type
        print("=== Message counts by type ===")
        cursor = conn.execute("""
            SELECT type, COUNT(*) as count
            FROM messages
            GROUP BY type
            ORDER BY count DESC
        """)
        for row in cursor:
            print(f"{row[0]}: {row[1]}")
        print()

        # Query assistant messages with JSON extraction
        print("=== Assistant message models ===")
        cursor = conn.execute("""
            SELECT
                json_extract(json_data, '$.message.model') as model,
                COUNT(*) as count
            FROM messages
            WHERE type = 'assistant'
            GROUP BY model
            ORDER BY count DESC
        """)
        for row in cursor:
            print(f"{row[0]}: {row[1]} messages")


if __name__ == "__main__":
    main()
