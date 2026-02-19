#!/usr/bin/env python3
"""
fix-camel-case.py — Convert camelCase identifiers to snake_case

Uses ast-grep for AST-aware identifier detection, then applies surgical
renaming across the codebase.

Usage:
    ./scripts/fix-camel-case.py --dry-run           # Preview changes
    ./scripts/fix-camel-case.py --apply             # Apply changes
    ./scripts/fix-camel-case.py --file src/foo.cpp  # Single file
"""

import argparse
import json
import os
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path
from typing import NamedTuple


# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
#                                                              // configuration
# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

# identifiers to never rename (external APIs, standard library, etc.)
PRESERVE_EXACT = frozenset({
    # c++ standard library
    "std", "size_t", "int64_t", "uint64_t", "int32_t", "uint32_t",
    "int16_t", "uint16_t", "int8_t", "uint8_t", "ptrdiff_t",
    "nullptr", "nullopt", "npos",
    
    # common type names that are fine
    "string", "vector", "map", "set", "pair", "tuple", "optional",
    "variant", "span", "array", "list", "deque", "queue", "stack",
    
    # nix-specific that we want to preserve (for now)
    "nix", "nixPath", "nixStore",
    
    # boost
    "boost",
    
    # nlohmann json
    "nlohmann", "json",
    
    # template parameter conventions (single uppercase letter)
    "T", "U", "V", "K", "N", "M", "R", "S", "E", "F", "P", "C", "I", "O",
})

# prefixes that indicate external API (don't rename)
PRESERVE_PREFIXES = (
    "std::", "boost::", "nlohmann::", "sqlite3", "curl", "git_",
    "EVP_", "SHA", "MD5", "BIO_", "SSL_", "OPENSSL_",
    "Py", "py",  # python
    "CURL", "curl_",
    "json::",
)

# patterns to preserve (regex)
PRESERVE_PATTERNS = [
    r"^_[A-Z]",          # reserved identifiers like _Pragma
    r"^__",              # compiler reserved
    r"^[A-Z][A-Z0-9_]+$", # ALL_CAPS (macros/constants) - keep as is
    r"^k[A-Z]",          # kConstant style (will handle separately if needed)
]

# directories to skip
SKIP_DIRS = {
    "third_party", "buck-out", ".git", "result", ".direnv", ".cache",
    "nix-meson-build-support", "straylight",  # other agents working here
}


# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
#                                                                 // conversion
# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

def camel_to_snake(name: str) -> str:
    """
    Convert camelCase or PascalCase to snake_case.
    
    Examples:
        camelCase -> camel_case
        PascalCase -> pascal_case
        XMLParser -> xml_parser
        parseJSON -> parse_json
        getHTTPResponse -> get_http_response
        IOError -> io_error
    """
    if not name:
        return name
    
    # handle consecutive uppercase (acronyms)
    # XMLParser -> XML_Parser -> xml_parser
    result = re.sub(r'([A-Z]+)([A-Z][a-z])', r'\1_\2', name)
    
    # handle camelCase
    # camelCase -> camel_Case -> camel_case
    result = re.sub(r'([a-z\d])([A-Z])', r'\1_\2', result)
    
    # handle numbers followed by letters
    result = re.sub(r'([a-zA-Z])(\d)', r'\1_\2', result)
    result = re.sub(r'(\d)([a-zA-Z])', r'\1_\2', result)
    
    return result.lower()


def is_camel_case(name: str) -> bool:
    """Check if identifier is camelCase (has lowercase followed by uppercase)."""
    if not name or len(name) < 2:
        return False
    
    # must have at least one lowercase->uppercase transition
    for i in range(len(name) - 1):
        if name[i].islower() and name[i + 1].isupper():
            return True
    
    return False


def is_pascal_case(name: str) -> bool:
    """Check if identifier is PascalCase (starts uppercase, has transitions)."""
    if not name or len(name) < 2:
        return False
    
    if not name[0].isupper():
        return False
    
    # must have lowercase letters too
    has_lower = any(c.islower() for c in name)
    if not has_lower:
        return False  # ALL_CAPS, not PascalCase
    
    return True


def should_convert(name: str) -> bool:
    """Determine if an identifier should be converted to snake_case."""
    if not name:
        return False
    
    # exact matches to preserve
    if name in PRESERVE_EXACT:
        return False
    
    # prefixes to preserve
    for prefix in PRESERVE_PREFIXES:
        if name.startswith(prefix):
            return False
    
    # patterns to preserve
    for pattern in PRESERVE_PATTERNS:
        if re.match(pattern, name):
            return False
    
    # single character or very short - skip
    if len(name) <= 1:
        return False
    
    # already snake_case
    if '_' in name and name.islower():
        return False
    
    # check if it's actually camelCase or PascalCase
    return is_camel_case(name) or is_pascal_case(name)


# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
#                                                                 // ast-grep
# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

class Identifier(NamedTuple):
    name: str
    file: str
    line: int
    column: int
    kind: str  # function, variable, class, etc.


def find_identifiers_with_ast_grep(file_path: str) -> list[Identifier]:
    """Use ast-grep to find all identifiers in a C++ file."""
    identifiers = []
    
    # ast-grep patterns for different identifier types
    patterns = [
        ("function", "function_declarator name: (identifier) @name"),
        ("variable", "declaration declarator: (identifier) @name"),
        ("parameter", "parameter_declaration declarator: (identifier) @name"),
        ("class", "class_specifier name: (type_identifier) @name"),
        ("struct", "struct_specifier name: (type_identifier) @name"),
        ("field", "field_declaration declarator: (field_identifier) @name"),
        ("method", "function_definition declarator: (function_declarator declarator: (field_identifier) @name)"),
    ]
    
    for kind, pattern in patterns:
        try:
            result = subprocess.run(
                ["nix-shell", "-p", "ast-grep", "--run",
                 f"ast-grep --pattern '{pattern}' --json '{file_path}'"],
                capture_output=True,
                text=True,
                timeout=30,
            )
            
            if result.returncode == 0 and result.stdout.strip():
                matches = json.loads(result.stdout)
                for match in matches:
                    name = match.get("text", "")
                    if name and should_convert(name):
                        identifiers.append(Identifier(
                            name=name,
                            file=file_path,
                            line=match.get("range", {}).get("start", {}).get("line", 0),
                            column=match.get("range", {}).get("start", {}).get("column", 0),
                            kind=kind,
                        ))
        except (subprocess.TimeoutExpired, json.JSONDecodeError, Exception) as e:
            print(f"  warning: ast-grep failed for {kind} in {file_path}: {e}", file=sys.stderr)
    
    return identifiers


def find_identifiers_with_regex(file_path: str) -> list[Identifier]:
    """Fallback: find identifiers using regex (less accurate but faster)."""
    identifiers = []
    
    try:
        with open(file_path, 'r', encoding='utf-8', errors='replace') as f:
            content = f.read()
    except Exception as e:
        print(f"  warning: could not read {file_path}: {e}", file=sys.stderr)
        return identifiers
    
    # patterns for different contexts
    patterns = [
        # function declarations: returnType functionName(
        (r'\b([a-z][a-zA-Z0-9]*)\s*\(', 'function'),
        # variable declarations: Type varName; or Type varName =
        (r'\b(?:auto|const|static|inline|virtual|override)?\s*\b[A-Za-z_][A-Za-z0-9_]*(?:<[^>]*>)?\s+([a-z][a-zA-Z0-9]*)\s*[;=\[\(,\)]', 'variable'),
        # class/struct members: type memberName_;
        (r'\b([a-z][a-zA-Z0-9]*_?)\s*;', 'member'),
        # class names: class ClassName {
        (r'\bclass\s+([A-Z][a-zA-Z0-9]*)', 'class'),
        # struct names: struct StructName {
        (r'\bstruct\s+([A-Z][a-zA-Z0-9]*)', 'struct'),
    ]
    
    lines = content.split('\n')
    for line_num, line in enumerate(lines, 1):
        # skip comments
        if line.strip().startswith('//') or line.strip().startswith('/*'):
            continue
        # skip string literals (basic check)
        if '"' in line:
            line = re.sub(r'"[^"]*"', '""', line)
        
        for pattern, kind in patterns:
            for match in re.finditer(pattern, line):
                name = match.group(1)
                if should_convert(name):
                    identifiers.append(Identifier(
                        name=name,
                        file=file_path,
                        line=line_num,
                        column=match.start(1),
                        kind=kind,
                    ))
    
    return identifiers


# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
#                                                                   // renaming
# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

def build_rename_map(identifiers: list[Identifier]) -> dict[str, str]:
    """Build a map of old_name -> new_name for all identifiers."""
    rename_map = {}
    
    for ident in identifiers:
        if ident.name not in rename_map:
            new_name = camel_to_snake(ident.name)
            if new_name != ident.name:
                rename_map[ident.name] = new_name
    
    return rename_map


def apply_renames_to_file(file_path: str, rename_map: dict[str, str], dry_run: bool) -> int:
    """Apply renames to a single file. Returns number of changes."""
    try:
        with open(file_path, 'r', encoding='utf-8', errors='replace') as f:
            original = f.read()
    except Exception as e:
        print(f"  error: could not read {file_path}: {e}", file=sys.stderr)
        return 0
    
    modified = original
    changes = 0
    
    # sort by length descending to avoid partial replacements
    sorted_renames = sorted(rename_map.items(), key=lambda x: len(x[0]), reverse=True)
    
    for old_name, new_name in sorted_renames:
        # use word boundaries to avoid partial matches
        pattern = r'\b' + re.escape(old_name) + r'\b'
        
        # count matches (excluding strings and comments - basic)
        matches = len(re.findall(pattern, modified))
        if matches > 0:
            modified = re.sub(pattern, new_name, modified)
            changes += matches
    
    if changes > 0 and not dry_run:
        try:
            with open(file_path, 'w', encoding='utf-8') as f:
                f.write(modified)
        except Exception as e:
            print(f"  error: could not write {file_path}: {e}", file=sys.stderr)
            return 0
    
    return changes


# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
#                                                                       // main
# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

def find_cpp_files(root: str) -> list[str]:
    """Find all C++ source and header files."""
    files = []
    
    for dirpath, dirnames, filenames in os.walk(root):
        # skip excluded directories
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        
        for filename in filenames:
            if filename.endswith(('.cpp', '.h', '.hpp', '.cc', '.cxx', '.hxx')):
                files.append(os.path.join(dirpath, filename))
    
    return sorted(files)


def main():
    parser = argparse.ArgumentParser(
        description='Convert camelCase identifiers to snake_case',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument('--dry-run', action='store_true',
                        help='Preview changes without applying')
    parser.add_argument('--apply', action='store_true',
                        help='Apply changes to files')
    parser.add_argument('--file', type=str,
                        help='Process a single file')
    parser.add_argument('--dir', type=str, default='src',
                        help='Root directory to process (default: src)')
    parser.add_argument('--use-ast', action='store_true',
                        help='Use ast-grep for more accurate detection (slower)')
    parser.add_argument('--verbose', '-v', action='store_true',
                        help='Show detailed output')
    
    args = parser.parse_args()
    
    if not args.dry_run and not args.apply:
        print("error: must specify --dry-run or --apply", file=sys.stderr)
        sys.exit(1)
    
    # find files
    if args.file:
        files = [args.file]
    else:
        files = find_cpp_files(args.dir)
    
    print(f"Processing {len(files)} files...")
    
    # collect all identifiers
    all_identifiers = []
    for i, file_path in enumerate(files):
        if args.verbose or (i + 1) % 50 == 0:
            print(f"  [{i+1}/{len(files)}] Scanning {file_path}")
        
        if args.use_ast:
            identifiers = find_identifiers_with_ast_grep(file_path)
        else:
            identifiers = find_identifiers_with_regex(file_path)
        
        all_identifiers.extend(identifiers)
    
    # build rename map
    rename_map = build_rename_map(all_identifiers)
    
    print(f"\nFound {len(rename_map)} unique identifiers to rename:")
    
    # show sample renames
    sample_size = min(50, len(rename_map))
    for old, new in list(rename_map.items())[:sample_size]:
        print(f"  {old} -> {new}")
    
    if len(rename_map) > sample_size:
        print(f"  ... and {len(rename_map) - sample_size} more")
    
    if args.dry_run:
        print("\n[DRY RUN] No changes applied.")
        return
    
    # apply renames
    print(f"\nApplying renames...")
    total_changes = 0
    
    for i, file_path in enumerate(files):
        changes = apply_renames_to_file(file_path, rename_map, dry_run=False)
        if changes > 0:
            if args.verbose:
                print(f"  [{i+1}/{len(files)}] {file_path}: {changes} changes")
            total_changes += changes
    
    print(f"\nTotal: {total_changes} replacements across {len(files)} files")


if __name__ == '__main__':
    main()
