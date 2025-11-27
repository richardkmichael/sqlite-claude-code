# frozen_string_literal: true

class UserMessage < Message
  def content
    msg = data.message&.content
    return msg if msg.is_a?(String)

    msg&.filter_map { |block| block.text if block.type == "text" }&.join("\n")
  end
end
