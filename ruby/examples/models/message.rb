# frozen_string_literal: true

require "ostruct"
require "json"
require_relative "../lib/json_query_builder"

# Base message class using Single Table Inheritance.
# The `type` column discriminates between message types.
class Message < ActiveRecord::Base
  self.primary_key = "rowid"
  self.inheritance_column = "type"

  # Map database type values to Ruby class names
  STI_TYPE_MAP = {
    "assistant" => "AssistantMessage",
    "user" => "UserMessage",
    "system" => "SystemMessage",
    "summary" => "SummaryMessage",
    "file-history-snapshot" => "FileHistorySnapshotMessage",
    "queue-operation" => "QueueOperationMessage"
  }.freeze

  # Extend all relations with json() method
  default_scope { extending(JsonQueryExtension) }

  class << self
    def sti_class_for(type_name)
      class_name = STI_TYPE_MAP[type_name]
      return Message unless class_name

      class_name.constantize
    rescue NameError
      Message
    end

    def sti_name
      STI_TYPE_MAP.key(name) || name
    end

    # JSON query builder entry point
    #
    # Examples:
    #   Model.json('$.message.model').eq('claude-sonnet')
    #   Model.json('message.model').eq('claude-sonnet')  # $. prefix optional
    #   Model.json('$.usage.output_tokens').gte(1000).lte(5000)
    #   Model.json('$.content').like('%search%')
    #
    # Works anywhere in chain:
    #   Model.where(session_id: 'x').json('$.model').eq('y').limit(10)
    #
    def json(path)
      JsonQueryBuilder.new(all, path)
    end
  end

  # Parse json_data into a nested OpenStruct for dot notation access
  def data
    @data ||= JSON.parse(json_data, object_class: OpenStruct)
  end

  def timestamp
    data.timestamp
  end

  def uuid
    data.uuid
  end
end

# Load subclasses
require_relative "assistant_message"
require_relative "user_message"
require_relative "system_message"
require_relative "summary_message"
require_relative "file_history_snapshot_message"
require_relative "queue_operation_message"
