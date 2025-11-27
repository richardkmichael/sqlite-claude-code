# frozen_string_literal: true

class SystemMessage < Message
  def subtype
    data.subtype
  end

  def content
    data.content
  end
end
