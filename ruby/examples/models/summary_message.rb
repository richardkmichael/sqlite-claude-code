# frozen_string_literal: true

class SummaryMessage < Message
  def content
    data.summary
  end
end
