# frozen_string_literal: true

# Builder for JSON field queries using SQLite json_extract
#
# Usage:
#   Model.json('$.path.to.field').eq(value)
#   Model.json('$.path').gte(100).lte(500)
#   Model.json('$.path').like('%pattern%')
#   Model.json('$.path').in(['a', 'b', 'c'])
#   Model.json('$.path').null
#   Model.json('$.path').not_null
#
# Works anywhere in a chain:
#   Model.where(session_id: 'x').json('$.path').eq(value).limit(10)
#
class JsonQueryBuilder
  attr_reader :relation, :path

  def initialize(relation, path)
    @relation = relation
    @path = path.start_with?("$.") ? path : "$.#{path}"
  end

  def eq(value)
    chain("#{expr} = ?", value)
  end

  def not_eq(value)
    chain("#{expr} != ?", value)
  end

  def gt(value)
    chain("#{expr} > ?", value)
  end

  def gte(value)
    chain("#{expr} >= ?", value)
  end

  def lt(value)
    chain("#{expr} < ?", value)
  end

  def lte(value)
    chain("#{expr} <= ?", value)
  end

  def between(min, max)
    chain("#{expr} BETWEEN ? AND ?", min, max)
  end

  def like(pattern)
    chain("#{expr} LIKE ?", pattern)
  end

  def not_like(pattern)
    chain("#{expr} NOT LIKE ?", pattern)
  end

  def in(values)
    chain("#{expr} IN (?)", values)
  end

  def not_in(values)
    chain("#{expr} NOT IN (?)", values)
  end

  def null
    chain("#{expr} IS NULL")
  end

  def not_null
    chain("#{expr} IS NOT NULL")
  end

  # Start a new JSON path query
  def json(new_path)
    JsonQueryBuilder.new(relation, new_path)
  end

  # Return to regular ActiveRecord relation for standard where clauses
  def where(...)
    relation.where(...).extending(JsonQueryExtension)
  end

  # Delegate everything else to relation, extending result to keep json() available
  def method_missing(method, ...)
    result = relation.public_send(method, ...)
    if result.is_a?(ActiveRecord::Relation)
      result.extending(JsonQueryExtension)
    else
      result
    end
  end

  def respond_to_missing?(method, include_private = false)
    relation.respond_to?(method, include_private)
  end

  private

  def expr
    "json_extract(json_data, '#{path}')"
  end

  def chain(condition, *args)
    new_relation = args.empty? ? relation.where(condition) : relation.where(condition, *args)
    JsonQueryBuilder.new(new_relation, path)
  end
end

# Extension module to add json() to ActiveRecord::Relation
module JsonQueryExtension
  def json(path)
    JsonQueryBuilder.new(self, path)
  end
end
