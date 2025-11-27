# sqlite-claude_code

SQLite extension for querying Claude Code conversation history.

```ruby
gem 'sqlite-claude_code'
```

## Quick start

```ruby
require 'sqlite-claude_code'

# Easiest: use connect() with a block (auto-closes)
SqliteClaudeCode.connect do |db|
  sessions = db.execute("SELECT COUNT(*) FROM sessions").first.first
  puts "Found #{sessions} Claude Code sessions"
end
```

## Usage

### Implicit stand alone database

The `connect()` method creates a database connection with the extension loaded:

```ruby
require 'sqlite-claude_code'

# With a block (automatically closes the connection)
SqliteClaudeCode.connect do |db|
  db.execute("SELECT * FROM projects LIMIT 5")
  db.execute("SELECT * FROM sessions LIMIT 5")
  db.execute("SELECT * FROM messages LIMIT 10")
end

# Without a block (you must close the connection)
db = SqliteClaudeCode.connect
db.execute("SELECT * FROM sessions")
db.close
```

### Loading into existing database

If you already have a database connection, use `load()`:

```ruby
require 'sqlite-claude_code'

db = SQLite3::Database.new(':memory:')
SqliteClaudeCode.load(db)

# Query virtual tables
db.execute("SELECT * FROM projects LIMIT 5")
db.close
```

### With ActiveRecord

```ruby
require 'sqlite-claude_code'
require 'active_record'

ActiveRecord::Base.establish_connection(adapter: "sqlite3", database: ":memory:")
SqliteClaudeCode.load(ActiveRecord::Base.connection.raw_connection)

# Define models and query
class Message < ActiveRecord::Base
  self.primary_key = 'rowid'
end

Message.all
```

## Virtual tables

- `projects` - Project directories in `~/.claude/projects/`
- `sessions` - JSONL session files within projects
- `messages` - Parsed messages from session JSONL files

## License

BSD-3-Clause
