# frozen_string_literal: true

require "bundler/setup"
require "sqlite-claude_code"
require "active_record"

# Setup ActiveRecord with in-memory database
ActiveRecord::Base.establish_connection(
  adapter: "sqlite3",
  database: ":memory:",
  pool: 5,
  timeout: 5000
)

# Load the Claude Code extension
SqliteClaudeCode.load(ActiveRecord::Base.connection.raw_connection)

# Load models
require_relative "models/message"

# STI automatically returns the correct subclass based on `type` column
puts "=== All messages (first 5) ==="
Message.limit(5).each do |msg|
  puts "#{msg.class.name}: #{msg.uuid}"
end

puts "\n=== Assistant messages ==="
AssistantMessage.limit(3).each do |msg|
  puts "Model: #{msg.model}"
  puts "Tokens: #{msg.input_tokens} in / #{msg.output_tokens} out"
  puts "Stop reason: #{msg.stop_reason}"
  puts "Thinking: #{msg.thinking?}"
  puts "Content preview: #{msg.content&.slice(0, 100)}..."
  puts "---"
end

puts "\n=== JSON query builder examples ==="

# Equality
puts "Sonnet messages: #{AssistantMessage.json('$.message.model').eq('claude-sonnet-4-5-20250929').count}"

# Comparisons
puts "Large responses (>=1000 tokens): #{AssistantMessage.json('$.message.usage.output_tokens').gte(1000).count}"
puts "Small responses (<=500 tokens): #{AssistantMessage.json('$.message.usage.output_tokens').lte(500).count}"

# Between
puts "Medium responses (500-2000): #{AssistantMessage.json('$.message.usage.output_tokens').gte(500).lte(2000).count}"

# Chaining different JSON paths
sonnet_large = AssistantMessage
  .json('$.message.model').eq('claude-sonnet-4-5-20250929')
  .json('$.message.usage.output_tokens').gte(5000)
puts "Sonnet with >=5000 tokens: #{sonnet_large.count}"

# Mixed chaining: where() and json() together
first_session = AssistantMessage.limit(1).pluck(:session_id).first
mixed = AssistantMessage
  .where(session_id: first_session)
  .json('$.message.usage.output_tokens').gte(100)
puts "Messages in session #{first_session[0..7]}... with >=100 tokens: #{mixed.count}"

# LIKE for text search (on subtype which is always a string)
puts "Init commands: #{SystemMessage.json('$.subtype').like('%init%').count}"

# Scope still works
puts "Messages with thinking: #{AssistantMessage.with_thinking.count}"

puts "\n=== System messages by subtype ==="
SystemMessage.json('$.subtype').eq("local_command").limit(2).each do |msg|
  puts "Content: #{msg.content&.slice(0, 80)}..."
end

puts "\n=== Token usage by model ==="
AssistantMessage
  .select("json_extract(json_data, '$.message.model') as model_name")
  .select("SUM(json_extract(json_data, '$.message.usage.output_tokens')) as total_tokens")
  .group("model_name")
  .order("total_tokens DESC")
  .each do |row|
    puts "#{row[:model_name]}: #{row[:total_tokens]} tokens"
  end
