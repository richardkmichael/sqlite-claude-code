# frozen_string_literal: true

require "bundler/setup"
require "sqlite-claude_code"

# Create database and load extension
SqliteClaudeCode.connect do |db|
  puts "Extension loaded successfully!\n\n"

  # Query projects
  puts "=== Projects ==="
  db.execute("SELECT * FROM projects LIMIT 5").each do |row|
    puts row.inspect
  end
  puts

  # Query sessions
  puts "=== Sessions (first 5) ==="
  db.execute("SELECT session_id, project_id FROM sessions LIMIT 5").each do |row|
    session_id, project_id = row
    puts "Session: #{session_id[0..7]}..., Project: #{project_id}"
  end
  puts

  # Query messages
  puts "=== Messages (first 10) ==="
  db.execute("SELECT type, message_id FROM messages LIMIT 10").each do |row|
    type, message_id = row
    puts "Type: #{type}, Message ID: #{message_id[0..15]}..."
  end
  puts

  # Count messages by type
  puts "=== Message counts by type ==="
  db.execute(<<~SQL).each do |row|
    SELECT type, COUNT(*) as count
    FROM messages
    GROUP BY type
    ORDER BY count DESC
  SQL
    type, count = row
    puts "#{type}: #{count}"
  end
  puts

  # Query assistant messages with JSON extraction
  puts "=== Assistant message models ==="
  db.execute(<<~SQL).each do |row|
    SELECT
      json_extract(json_data, '$.message.model') as model,
      COUNT(*) as count
    FROM messages
    WHERE type = 'assistant'
    GROUP BY model
    ORDER BY count DESC
  SQL
    model, count = row
    puts "#{model}: #{count} messages"
  end
  puts

  # Token usage by model
  puts "=== Token usage by model ==="
  db.execute(<<~SQL).each do |row|
    SELECT
      json_extract(json_data, '$.message.model') as model,
      SUM(json_extract(json_data, '$.message.usage.output_tokens')) as total_tokens
    FROM messages
    WHERE type = 'assistant'
    GROUP BY model
    ORDER BY total_tokens DESC
  SQL
    model, total_tokens = row
    puts "#{model}: #{total_tokens} tokens"
  end
end
