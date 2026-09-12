# Copyright 2026 LunarG, Inc.
# Copyright 2026 Google LLC
# SPDX-License-Identifier: MIT

"""Write a text-based stub library (.tbd) that "defines" every weak entrypoint symbol the
driver's dispatch tables reference.

The generated entrypoint tables declare every possible entrypoint weak, so that the ones a
driver doesn't implement resolve to NULL. On macOS that means linking with
-undefined dynamic_lookup, which turns each of those ~3000 references into a flat-namespace
bind: at load, dyld looks each one up in every image in the process, which in a large host
process costs hundreds of milliseconds inside dlopen.

Linking this stub library with -weak_library instead makes every reference a two-level weak
import of a library that never exists at run time - a missing weak library resolves all of its
symbols to NULL without any searching. Entrypoints the driver does implement never reach the
stub library: the linker resolves them inside the driver first.
"""

import argparse
import re

DECL = re.compile(r'VKAPI_CALL\s+([A-Za-z0-9_]+)\s*\(')

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', required=True, help='The .tbd to write')
    parser.add_argument('--install-name', default='@rpath/libkk_entrypoint_stubs.dylib')
    parser.add_argument('headers', nargs='+', help='Generated *_entrypoints.h files to take symbols from')
    args = parser.parse_args()

    symbols = set()
    for header in args.headers:
        with open(header) as f:
            symbols.update('_' + name for name in DECL.findall(f.read()))

    with open(args.out, 'w') as f:
        f.write('--- !tapi-tbd\n')
        f.write('tbd-version: 4\n')
        f.write('targets: [ arm64-macos ]\n')
        f.write(f"install-name: '{args.install_name}'\n")
        f.write('exports:\n')
        f.write('  - targets: [ arm64-macos ]\n')
        f.write('    symbols: [ ' + ', '.join(sorted(symbols)) + ' ]\n')
        f.write('...\n')

if __name__ == '__main__':
    main()
