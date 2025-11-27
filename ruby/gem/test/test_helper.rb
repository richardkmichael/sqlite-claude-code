require "sqlite-claude_code"
require "minitest/autorun"

class TestHelper < Minitest::Test
  def setup
    @db = SQLite3::Database.new(':memory:')
    SqliteClaudeCode.load(@db)
  end

  def teardown
    @db.close
  end
end
