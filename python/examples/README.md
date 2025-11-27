# Python Examples

Examples demonstrating the `sqlite-claude-code` package.

## Setup

The examples use the built package from the sibling `../package` directory.

```bash
# First, build the package
cd ../package
uv build

# Then, install and run examples
cd ../examples
uv sync
uv run python basic_example.py
```

## Development Workflow

### After Changes to C Extension or Python Code

```bash
# Rebuild the package
cd ../package
uv build

# Reinstall in examples
cd ../examples
uv sync --reinstall-package sqlite-claude-code
uv run python basic_example.py
```

## How It Works

- `pyproject.toml` specifies `sqlite-claude-code` as a dependency
- `[tool.uv.sources]` tells uv to use the local package at `../package`
- `uv build` in the package directory compiles the C extension and creates a wheel
- `uv sync` in the examples directory installs the built wheel
- The wheel contains the pre-compiled extension for your platform
