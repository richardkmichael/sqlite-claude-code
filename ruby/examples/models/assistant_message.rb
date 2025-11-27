# frozen_string_literal: true

class AssistantMessage < Message
  scope :with_thinking, -> {
    where("json_extract(json_data, '$.message.content') LIKE '%\"type\":\"thinking\"%'")
  }

  def model
    data.message&.model
  end

  def content
    msg = data.message&.content
    return msg if msg.is_a?(String)

    msg&.filter_map { |block| block.text if block.type == "text" }&.join("\n")
  end

  def stop_reason
    data.message&.stop_reason
  end

  def input_tokens
    data.message&.usage&.input_tokens
  end

  def output_tokens
    data.message&.usage&.output_tokens
  end

  def thinking?
    data.message&.content&.any? { |block| block.type == "thinking" }
  end

  def thinking_content
    return nil unless data.message&.content.is_a?(Array)

    data.message.content
      .filter_map { |block| block.thinking if block.type == "thinking" }
      .join("\n")
  end
end
