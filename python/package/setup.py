"""Setup script that compiles the SQLite extension during build."""

import platform
import subprocess
import sys
from pathlib import Path

from setuptools import setup
from setuptools.command.build_py import build_py
from setuptools.command.editable_wheel import editable_wheel


class BuildWithExtension(build_py):
    """Build command that compiles the SQLite extension."""

    def run(self):
        """Run build and compile extension."""
        self.compile_extension()
        super().run()

    def compile_extension(self):
        """Compile the C extension into the source directory."""
        package_dir = Path(__file__).parent
        c_src_dir = package_dir / "c_src"
        target_dir = package_dir / "src" / "sqlite_claude_code"

        # Source files
        src_files = [
            c_src_dir / "init.c",
            c_src_dir / "projects.c",
            c_src_dir / "sessions.c",
            c_src_dir / "messages.c",
            c_src_dir / "common.c",
            c_src_dir / "vendor" / "cJSON.c",
        ]

        # Check that source files exist
        for src_file in src_files:
            if not src_file.exists():
                print(f"Error: Source file not found: {src_file}", file=sys.stderr)
                sys.exit(1)

        # Ensure target directory exists
        target_dir.mkdir(parents=True, exist_ok=True)

        # Platform-specific compilation
        system = platform.system()

        print("=" * 60)
        print(f"Compiling SQLite extension for {system}...")
        print("=" * 60)

        try:
            if system == "Darwin":  # macOS
                output_file = target_dir / "claude_code.dylib"
                self._compile_macos(src_files, output_file, c_src_dir)
            elif system == "Linux":
                output_file = target_dir / "claude_code.so"
                self._compile_linux(src_files, output_file, c_src_dir)
            elif system == "Windows":
                output_file = target_dir / "claude_code.dll"
                self._compile_windows(src_files, output_file, c_src_dir)
            else:
                print(f"Error: Unsupported platform: {system}", file=sys.stderr)
                sys.exit(1)

            print(f"✓ Successfully compiled: {output_file}")
            print("=" * 60)

        except subprocess.CalledProcessError as e:
            print(f"Error: Compilation failed: {e}", file=sys.stderr)
            sys.exit(1)

    def _compile_macos(self, src_files, output_file, c_src_dir):
        """Compile on macOS."""
        # Try to find SQLite via Homebrew first
        try:
            brew_prefix = subprocess.check_output(
                ["brew", "--prefix", "sqlite3"],
                stderr=subprocess.DEVNULL,
                text=True
            ).strip()
            sqlite_includes = ["-I", f"{brew_prefix}/include"]
            sqlite_libs = ["-L", f"{brew_prefix}/lib"]
            print(f"Using Homebrew SQLite from: {brew_prefix}")
        except (subprocess.CalledProcessError, FileNotFoundError):
            sqlite_includes = []
            sqlite_libs = []
            print("Using system SQLite")

        cmd = [
            "clang",
            "-dynamiclib",
            "-o", str(output_file),
            "-fPIC",
            "-Wall", "-Wextra", "-Werror",
            f"-I{c_src_dir}",
            f"-I{c_src_dir / 'vendor'}",
        ] + sqlite_includes + sqlite_libs + [
            "-lsqlite3",
        ] + [str(f) for f in src_files]

        subprocess.run(cmd, check=True)

    def _compile_linux(self, src_files, output_file, c_src_dir):
        """Compile on Linux."""
        cmd = [
            "gcc",
            "-shared",
            "-o", str(output_file),
            "-fPIC",
            "-Wall", "-Wextra", "-Werror",
            f"-I{c_src_dir}",
            f"-I{c_src_dir / 'vendor'}",
            "-lsqlite3",
        ] + [str(f) for f in src_files]

        subprocess.run(cmd, check=True)

    def _compile_windows(self, src_files, output_file, c_src_dir):
        """Compile on Windows."""
        raise NotImplementedError(
            "Windows compilation not yet implemented. "
            "Please compile manually or use WSL/MinGW."
        )


class EditableWithExtension(editable_wheel):
    """Editable install that ensures extension is compiled."""

    def run(self):
        """Run editable install and compile extension."""
        self.run_command('build_py')
        super().run()


if __name__ == "__main__":
    setup(
        cmdclass={
            "build_py": BuildWithExtension,
            "editable_wheel": EditableWithExtension,
        }
    )
