require_relative "test_helper"

class ExtensionTest < TestHelper
  def test_projects_table_exists
    result = @db.execute("SELECT name FROM sqlite_master WHERE type='table' AND name='projects'")
    assert_equal 1, result.length
  end

  def test_sessions_table_exists
    result = @db.execute("SELECT name FROM sqlite_master WHERE type='table' AND name='sessions'")
    assert_equal 1, result.length
  end

  def test_messages_table_exists
    result = @db.execute("SELECT name FROM sqlite_master WHERE type='table' AND name='messages'")
    assert_equal 1, result.length
  end
end
