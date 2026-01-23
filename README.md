# patchnar

A NAR (Nix ARchive) stream patcher for Android compatibility. Patches ELF binaries, symlinks, and shell scripts within NAR streams to work with Android's patched glibc.

Based on [patchelf](https://github.com/NixOS/patchelf) and uses it as a library for ELF modifications.

## Features

- **Streaming NAR processing**: Reads NAR from stdin, writes patched NAR to stdout
- **ELF patching**: Modifies interpreters and RPATH for Android glibc compatibility
- **Symlink patching**: Adds installation prefix to `/nix/store/` symlink targets
- **Script patching**: Uses GNU Source-highlight for string-aware shebang and path patching
- **Hash mapping**: Substitutes store path hashes for inter-package reference updates
- **Parallel processing**: TBB parallel_pipeline for concurrent patching with automatic backpressure

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                    TBB parallel_pipeline                        │
│                                                                 │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐      │
│  │ Parse NAR    │ -> │ Patch        │ -> │ Write NAR    │      │
│  │ (serial)     │    │ (parallel)   │    │ (serial)     │      │
│  └──────────────┘    └──────────────┘    └──────────────┘      │
│                                                                 │
│  8 tokens in flight for automatic backpressure                  │
└─────────────────────────────────────────────────────────────────┘
```

## Usage

```console
$ patchnar [OPTIONS] < input.nar > output.nar
```

### Options

| Option | Description |
|--------|-------------|
| `--prefix PATH` | Installation prefix (e.g., `/data/data/com.termux.nix/files/usr`) |
| `--glibc PATH` | Android glibc store path |
| `--old-glibc PATH` | Original glibc store path to replace |
| `--mappings FILE` | Hash mappings file (format: `OLD_PATH NEW_PATH` per line) |
| `--self-mapping MAP` | Self-reference mapping (`OLD_PATH NEW_PATH`) |
| `--add-prefix-to PATH` | Path pattern to prefix in scripts (e.g., `/nix/var/`). Repeatable. |
| `--source-highlight-data-dir DIR` | Path to source-highlight `.lang` files |
| `--debug` | Enable debug output |

### Example

```console
$ nix-store --dump /nix/store/abc123-hello | patchnar \
    --prefix /data/data/com.termux.nix/files/usr \
    --glibc /nix/store/xyz789-glibc-android-2.40 \
    --old-glibc /nix/store/def456-glibc-2.40 \
    --mappings hash-mappings.txt \
    > patched-hello.nar
```

### Environment Variables

| Variable | Description |
|----------|-------------|
| `TBB_NUM_THREADS` | Control thread count for parallel patching |

## Building

Requires:
- C++23 compiler (GCC 14+ or Clang 18+)
- Intel TBB (onetbb >= 2020.0)
- GNU Source-highlight (>= 3.0)
- Boost (headers, for source-highlight)

### Via GNU Autotools

```console
./bootstrap.sh
./configure
make
sudo make install
```

### Via Nix

```console
nix build
```

## How It Works

### ELF Patching

For regular ELF files:
1. Sets interpreter to `$PREFIX/nix/store/.../ld-linux.so`
2. Modifies RPATH to include prefix and substitute glibc paths
3. Applies hash mapping to update inter-package store references

### Symlink Patching

For symlinks pointing to `/nix/store/`:
1. Adds installation prefix to target
2. Applies hash mapping to update store path references

### Script Patching

For shell scripts (detected via shebang or `.sh` extension):
1. Uses GNU Source-highlight to tokenize as shell script
2. Only modifies paths inside string literals (preserves code structure)
3. Adds prefix to `/nix/store/` and other configured paths
4. Applies hash mapping to update store references

## Integration with nix-on-droid

patchnar is designed for [nix-on-droid](https://github.com/nix-community/nix-on-droid) to enable NixOS-style package grafting on Android:

1. Build packages using standard nixpkgs binary cache
2. At install time, patch NAR streams to use Android-compatible glibc
3. Recursive dependency patching via hash mapping ensures all references are updated

## License

GNU General Public License v3.0 or later.

Based on patchelf by Eelco Dolstra.

## See Also

- [patchelf](https://github.com/NixOS/patchelf) - The ELF patching library used internally
- [nix-on-droid](https://github.com/nix-community/nix-on-droid) - Nix package manager on Android
- [GNU Source-highlight](https://www.gnu.org/software/src-highlite/) - Used for script tokenization
