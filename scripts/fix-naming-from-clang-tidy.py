#!/usr/bin/env python3
"""
fix-naming-from-clang-tidy.py — Fix identifier naming using clang-tidy output

This script:
1. Runs clang-tidy with readability-identifier-naming check
2. Parses the fix-it suggestions
3. Applies the renames globally across the codebase

Naming conventions applied (per Straylight style guide):
- Types (class/struct/enum): snake_case_t suffix to avoid collision with variables
- Functions/methods: snake_case
- Variables/parameters: snake_case
- Template parameters: CamelCase (preserved)
- Macros: UPPER_CASE (preserved)

Usage:
    ./scripts/fix-naming-from-clang-tidy.py --dry-run                    # Preview
    ./scripts/fix-naming-from-clang-tidy.py --dry-run --save-map map.json # Save map
    ./scripts/fix-naming-from-clang-tidy.py --apply --load-map map.json   # Apply
"""

import argparse
import json
import os
import re
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path
from typing import Optional


# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
#                                                              // configuration
# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

SKIP_DIRS = {
    "third_party", "buck-out", ".git", "result", ".direnv", ".cache",
    "nix-meson-build-support", "straylight",  # other agents working here
}

# identifiers to never rename
PRESERVE_EXACT = frozenset({
    # namespaces
    "nix", "std", "boost", "nlohmann",
    
    # common short names that are fine
    "i", "j", "k", "n", "m", "x", "y", "z",
    
    # template params (single letter)
    "T", "U", "V", "K", "N", "M", "R", "S", "E", "F", "P", "C", "I", "O",
    
    # common words that appear in paths/strings and would cause false positives
    "writer", "reader", "output", "input", "source", "sink", "buffer",
    "store", "cache", "hash", "config", "logger", "handler", "parser",
    "builder", "factory", "manager", "provider", "consumer", "producer",
    "client", "server", "session", "context", "state", "status",
    "error", "result", "value", "key", "data", "info", "path", "name",
    "file", "dir", "root", "base", "node", "item", "entry", "record",
})

# patterns for identifiers to preserve
PRESERVE_PATTERNS = [
    r'^[A-Z][A-Z0-9_]+$',    # ALL_CAPS - macros
    r'^YY_',                  # yacc/bison generated
    r'^NLOHMANN_',            # nlohmann json macros
    r'^[a-z_]+_$',            # trailing underscore (intentional)
    r'^_[a-zA-Z]',            # leading underscore (reserved/internal)
    r'^__',                   # double underscore (compiler reserved)
]

# common template parameter names (keep CamelCase per style guide)
TEMPLATE_PARAMS = frozenset({
    "Args", "Arg", "Attrs", "Base", "Bindings", "Body", "Builder",
    "Callback", "Child", "Children", "Cmp", "Compare", "Container",
    "Content", "Context", "Data", "Derived", "Element", "Elements",
    "Entry", "Error", "Exception", "Expr", "Fn", "Fun", "Func",
    "Handler", "Hash", "Impl", "Info", "Input", "InputIter", "InputIterator",
    "Item", "Items", "Iter", "Iterator", "Key", "Keys", "Kind",
    "Lambda", "List", "Msg", "Name", "Node", "Obj", "Object", "Op",
    "Output", "OutputIter", "OutputIterator", "Pair", "Param", "Params",
    "Parser", "Path", "Paths", "Payload", "Pred", "Predicate", "Ptr",
    "Range", "Reader", "Ref", "Result", "Ret", "Return", "Root",
    "Self", "Sink", "Source", "State", "Store", "Stream", "String",
    "Super", "Target", "This", "Token", "Tokens", "Transform", "Tuple",
    "Type", "Types", "Val", "Value", "Values", "Var", "Variant",
    "Visitor", "Writer",
})

# identifier kinds that should get _t suffix (types)
TYPE_KINDS = frozenset({
    "class", "struct", "enum", "union", "typedef", "type alias",
})


# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
#                                                                    // helpers
# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

def strip_ansi(text: str) -> str:
    """Remove ANSI escape codes from text."""
    return re.sub(r'\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])', '', text)


def should_rename(name: str) -> bool:
    """Check if an identifier should be renamed."""
    if name in PRESERVE_EXACT:
        return False
    if name in TEMPLATE_PARAMS:
        return False
    for pattern in PRESERVE_PATTERNS:
        if re.match(pattern, name):
            return False
    # skip very short names
    if len(name) <= 2:
        return False
    return True


# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
#                                                           // clang-tidy parsing
# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

def run_clang_tidy(file_path: str) -> str:
    """Run clang-tidy on a file and return output."""
    try:
        result = subprocess.run(
            ["clang-tidy", 
             "--checks=-*,readability-identifier-naming",
             file_path],
            capture_output=True,
            text=True,
            timeout=120,
        )
        return result.stdout + result.stderr
    except subprocess.TimeoutExpired:
        return ""
    except Exception:
        return ""


def parse_naming_fixes(output: str) -> dict[str, str]:
    """
    Parse clang-tidy output to extract old_name -> new_name mappings.
    
    Handles output format:
    path:line:col: error: invalid case style for class 'CanonPath' [...]
       49 | class CanonPath {
          |       ^~~~~~~~~
          |       canon_path
    """
    rename_map = {}
    output = strip_ansi(output)
    lines = output.split('\n')
    
    i = 0
    while i < len(lines):
        line = lines[i]
        
        # look for naming violation - capture the kind and name
        match = re.search(
            r"invalid case style for (\w+(?:\s+\w+)?) '([^']+)'.*\[readability-identifier-naming",
            line
        )
        
        if match:
            kind = match.group(1).lower()
            old_name = match.group(2)
            
            # look ahead for the fix-it suggestion
            for j in range(i + 1, min(i + 8, len(lines))):
                suggestion_line = lines[j].strip()
                
                if not suggestion_line:
                    continue
                
                # handle lines with pipe character (clang output format)
                if '|' in suggestion_line:
                    parts = suggestion_line.split('|')
                    if len(parts) >= 2:
                        after_pipe = parts[-1].strip()
                        # skip marker lines
                        if after_pipe.startswith(('^', '~')):
                            continue
                        # check for valid snake_case suggestion
                        if re.match(r'^[a-z_][a-z0-9_]*$', after_pipe):
                            if should_rename(old_name) and after_pipe != old_name:
                                # add _t suffix for types
                                if kind in TYPE_KINDS and not after_pipe.endswith('_t'):
                                    after_pipe = after_pipe + '_t'
                                rename_map[old_name] = after_pipe
                            break
                    continue
                
                # stop if we hit another error/warning
                if ':' in suggestion_line and ('error' in suggestion_line or 'warning' in suggestion_line):
                    break
                
                # direct suggestion line
                if re.match(r'^[a-z_][a-z0-9_]*$', suggestion_line):
                    if should_rename(old_name) and suggestion_line != old_name:
                        if kind in TYPE_KINDS and not suggestion_line.endswith('_t'):
                            suggestion_line = suggestion_line + '_t'
                        rename_map[old_name] = suggestion_line
                    break
        
        i += 1
    
    return rename_map


def analyze_file(file_path: str) -> tuple[str, dict[str, str]]:
    """Analyze a single file with clang-tidy (for parallel execution)."""
    output = run_clang_tidy(file_path)
    return file_path, parse_naming_fixes(output)


# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
#                                                                   // renaming
# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

def replace_outside_strings(line: str, old: str, new: str) -> tuple[str, int]:
    """
    Replace identifier in line, avoiding string literals and comments.
    Returns (modified_line, change_count).
    """
    # skip preprocessor directives that might contain paths
    stripped = line.lstrip()
    if stripped.startswith('#'):
        return line, 0
    
    # find all string literals and comments to exclude
    exclusions = []
    
    # find string literals "..."
    for m in re.finditer(r'"(?:[^"\\]|\\.)*"', line):
        exclusions.append((m.start(), m.end()))
    
    # find char literals '...'
    for m in re.finditer(r"'(?:[^'\\]|\\.)*'", line):
        exclusions.append((m.start(), m.end()))
    
    # find // comments
    comment_match = re.search(r'//.*$', line)
    if comment_match:
        exclusions.append((comment_match.start(), comment_match.end()))
    
    def in_exclusion(pos: int) -> bool:
        return any(start <= pos < end for start, end in exclusions)
    
    # find and replace
    pattern = re.compile(r'\b' + re.escape(old) + r'\b')
    result = []
    last_end = 0
    count = 0
    
    for m in pattern.finditer(line):
        if not in_exclusion(m.start()):
            result.append(line[last_end:m.start()])
            result.append(new)
            count += 1
        else:
            result.append(line[last_end:m.end()])
        last_end = m.end()
    
    result.append(line[last_end:])
    return ''.join(result), count


def apply_renames_to_file(file_path: str, rename_map: dict[str, str], dry_run: bool) -> int:
    """Apply renames to a single file. Returns number of changes."""
    try:
        with open(file_path, 'r', encoding='utf-8', errors='replace') as f:
            lines = f.readlines()
    except Exception as e:
        print(f"  error: could not read {file_path}: {e}", file=sys.stderr)
        return 0
    
    # sort by length descending to avoid partial replacements
    sorted_renames = sorted(rename_map.items(), key=lambda x: len(x[0]), reverse=True)
    
    modified_lines = []
    total_changes = 0
    
    for line in lines:
        modified_line = line
        for old_name, new_name in sorted_renames:
            modified_line, count = replace_outside_strings(modified_line, old_name, new_name)
            total_changes += count
        modified_lines.append(modified_line)
    
    if total_changes > 0 and not dry_run:
        try:
            with open(file_path, 'w', encoding='utf-8') as f:
                f.writelines(modified_lines)
        except Exception as e:
            print(f"  error: could not write {file_path}: {e}", file=sys.stderr)
            return 0
    
    return total_changes


def apply_renames_globally(files: list[str], rename_map: dict[str, str], 
                           dry_run: bool, verbose: bool) -> int:
    """Apply renames across all files."""
    total_changes = 0
    files_changed = 0
    
    for i, file_path in enumerate(files):
        changes = apply_renames_to_file(file_path, rename_map, dry_run)
        if changes > 0:
            files_changed += 1
            if verbose:
                print(f"  [{i+1}/{len(files)}] {file_path}: {changes} changes")
            total_changes += changes
    
    return total_changes


# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
#                                                              // file discovery
# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

def find_cpp_files(root: str) -> list[str]:
    """Find all C++ source and header files."""
    files = []
    
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        
        for filename in filenames:
            if filename.endswith(('.cpp', '.h', '.hpp', '.cc', '.cxx', '.hxx')):
                files.append(os.path.join(dirpath, filename))
    
    return sorted(files)


def find_files_in_compile_commands() -> list[str]:
    """Find files from compile_commands.json."""
    try:
        with open('compile_commands.json', 'r') as f:
            commands = json.load(f)
        
        files = []
        for entry in commands:
            file_path = entry.get('file', '')
            if file_path.endswith('.cpp'):
                if not any(skip in file_path for skip in SKIP_DIRS):
                    files.append(file_path)
        
        return sorted(set(files))
    except Exception:
        return []


# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
#                                                                       // main
# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

def main():
    parser = argparse.ArgumentParser(
        description='Fix identifier naming using clang-tidy suggestions',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument('--dry-run', action='store_true',
                        help='Preview changes without applying')
    parser.add_argument('--apply', action='store_true',
                        help='Apply changes to files')
    parser.add_argument('--file', type=str,
                        help='Process a single file')
    parser.add_argument('--dir', type=str, default='src/nix',
                        help='Root directory to process (default: src/nix)')
    parser.add_argument('--verbose', '-v', action='store_true',
                        help='Show detailed output')
    parser.add_argument('--limit', type=int, default=0,
                        help='Limit number of files to analyze (0 = no limit)')
    parser.add_argument('--jobs', '-j', type=int, default=8,
                        help='Number of parallel jobs (default: 8)')
    parser.add_argument('--save-map', type=str,
                        help='Save rename map to JSON file')
    parser.add_argument('--load-map', type=str,
                        help='Load rename map from JSON file (skip analysis)')
    
    args = parser.parse_args()
    
    if not args.dry_run and not args.apply and not args.save_map:
        print("error: must specify --dry-run, --apply, or --save-map", file=sys.stderr)
        sys.exit(1)
    
    # load existing rename map if specified
    if args.load_map:
        print(f"Loading rename map from {args.load_map}...")
        with open(args.load_map, 'r') as f:
            all_renames = json.load(f)
        print(f"Loaded {len(all_renames)} renames")
        
        all_files = find_cpp_files(args.dir)
        
        if args.dry_run:
            # show what would change
            sample = list(all_renames.items())[:30]
            print("\nSample renames:")
            for old, new in sample:
                print(f"  {old} -> {new}")
            if len(all_renames) > 30:
                print(f"  ... and {len(all_renames) - 30} more")
            print("\n[DRY RUN] No changes applied.")
            return
        
        print(f"\nApplying renames to {len(all_files)} files...")
        total = apply_renames_globally(all_files, all_renames, dry_run=False, verbose=args.verbose)
        print(f"\nTotal: {total} replacements")
        return
    
    # find source files for analysis
    if args.file:
        source_files = [args.file]
    else:
        source_files = find_files_in_compile_commands()
        if not source_files:
            source_files = find_cpp_files(args.dir)
        source_files = [f for f in source_files if f.endswith('.cpp')]
    
    if args.limit > 0:
        source_files = source_files[:args.limit]
    
    print(f"Phase 1: Analyzing {len(source_files)} source files ({args.jobs} parallel jobs)...")
    
    # parallel analysis
    all_renames = {}
    completed = 0
    
    with ProcessPoolExecutor(max_workers=args.jobs) as executor:
        futures = {executor.submit(analyze_file, f): f for f in source_files}
        
        for future in as_completed(futures):
            completed += 1
            if args.verbose or completed % 20 == 0:
                print(f"  [{completed}/{len(source_files)}] analyzed")
            
            try:
                _, renames = future.result()
                for old, new in renames.items():
                    if old not in all_renames:
                        all_renames[old] = new
            except Exception as e:
                if args.verbose:
                    print(f"  error: {e}", file=sys.stderr)
    
    print(f"\nFound {len(all_renames)} unique identifiers to rename")
    
    if not all_renames:
        print("No renames needed!")
        return
    
    # show samples
    sample = list(all_renames.items())[:30]
    print("\nSample renames:")
    for old, new in sample:
        print(f"  {old} -> {new}")
    if len(all_renames) > 30:
        print(f"  ... and {len(all_renames) - 30} more")
    
    # categorize
    types_renamed = sum(1 for v in all_renames.values() if v.endswith('_t'))
    funcs_renamed = len(all_renames) - types_renamed
    print(f"\nBreakdown: {types_renamed} types (->_t), {funcs_renamed} functions/variables")
    
    # save map
    if args.save_map:
        with open(args.save_map, 'w') as f:
            json.dump(all_renames, f, indent=2, sort_keys=True)
        print(f"\nSaved rename map to {args.save_map}")
    
    if args.dry_run:
        print("\n[DRY RUN] No changes applied.")
        return
    
    # apply
    print(f"\nPhase 2: Applying renames...")
    all_files = find_cpp_files(args.dir)
    total = apply_renames_globally(all_files, all_renames, dry_run=False, verbose=args.verbose)
    print(f"\nTotal: {total} replacements across {len(all_files)} files")


if __name__ == '__main__':
    main()
