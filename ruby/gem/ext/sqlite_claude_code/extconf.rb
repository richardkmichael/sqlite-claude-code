require "mkmf"

# Require SQLite3
abort "sqlite3 is missing" unless have_library("sqlite3")
abort "sqlite3.h is missing" unless have_header("sqlite3.h")

# Platform-specific SQLite detection
case RbConfig::CONFIG["host_os"]
when /darwin/
  # macOS: Try Homebrew first, then pkg-config
  brew_prefix = `brew --prefix sqlite3 2>/dev/null`.strip
  if !brew_prefix.empty?
    $INCFLAGS << " -I#{brew_prefix}/include"
    $LDFLAGS << " -L#{brew_prefix}/lib"
  end
  # Suppress SQLite header warnings
  $CFLAGS.gsub!(/-I(\S+sqlite3\S*)/, '-isystem \1')
when /linux/
  pkg_config("sqlite3")
when /mswin|mingw/
  # Windows: Use environment variables if set
  $CFLAGS << " #{ENV['SQLITE_CFLAGS']}" if ENV["SQLITE_CFLAGS"]
  $LDFLAGS << " #{ENV['SQLITE_LIBS']}" if ENV["SQLITE_LIBS"]
end

# Code quality standards from CLAUDE.md
$CFLAGS << " -Wall -Wextra -Werror -fPIC"
$CFLAGS << " -isystem $(srcdir)/src/vendor"  # Suppress vendor warnings

# Set VPATH to include src subdirectories
$VPATH = %w[$(srcdir)/src $(srcdir)/src/vendor]

# List source files (without directory paths - VPATH will find them)
$srcs = %w[
  init.c
  projects.c
  sessions.c
  messages.c
  common.c
  cJSON.c
]

create_makefile("sqlite_claude_code/claude_code")
