# Ruby Examples

Examples demonstrating the `sqlite-claude_code` gem.

## Setup

```bash
bundle install
```

## Running Examples

### SQLite directly

```bash
bundle exec ruby basic_example.rb
```

### ActiveRecord

```bash
bundle exec ruby activerecord_example.rb
```

## Examples

### basic_example.rb

- Uses SQLite3 gem directly (no ActiveRecord)
- Shows basic queries on all three virtual tables (projects, sessions, messages)
- Demonstrates JSON extraction with `json_extract()`
- Self-contained in a single file

### activerecord_example.rb

- Uses ActiveRecord with the extension
- Demonstrates Single Table Inheritance (STI) with message types
- Shows the JSON query builder for cleaner queries
- Uses model classes from `models/` directory
