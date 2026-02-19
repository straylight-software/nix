#!/usr/bin/env python3
"""
Fix private/protected member naming by adding trailing underscore.

This script:
1. Runs clang-tidy on all .cpp files to collect private/protected member violations
2. Maps buck-out header paths back to source paths
3. Creates a rename map for each file
4. Applies the renames to the actual source files
"""

import subprocess
import re
import sys
import os
from pathlib import Path
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed

# Root directory
ROOT = Path(__file__).resolve().parent.parent
SRC_DIR = ROOT / "src" / "nix"

# Regex to parse clang-tidy output
# buck-out/.../buck-headers/nix/store/build/goal.h:70:9: warning: invalid case style for private member 'waitees' [readability-identifier-naming]
WARNING_PATTERN = re.compile(
    r"^(.+?):(\d+):(\d+): warning: invalid case style for (private|protected) member '(\w+)'"
)

# Regex to extract suggested fix from the next lines
FIX_PATTERN = re.compile(r"^\s+\^\s*$|^\s+(\w+_?)$")


def map_buck_path_to_source(buck_path: str) -> Path | None:
    """Map a buck-out header path back to the actual source file."""
    # buck-out/v2/gen/root/b42aeba648b8c415/src/nix/store/__store__/buck-headers/nix/store/build/goal.h
    # -> src/nix/store/build/goal.h
    
    if "buck-headers" not in buck_path:
        # Direct source file
        return Path(buck_path) if Path(buck_path).exists() else None
    
    # Extract the path after buck-headers
    match = re.search(r"buck-headers/(.+)$", buck_path)
    if not match:
        return None
    
    header_rel = match.group(1)  # e.g., "nix/store/build/goal.h"
    
    # Try common source locations
    candidates = [
        ROOT / "src" / header_rel,  # src/nix/store/build/goal.h
        ROOT / "src" / "nix" / header_rel.replace("nix/", "", 1),  # alternative
    ]
    
    for candidate in candidates:
        if candidate.exists():
            return candidate
    
    return None


def run_clang_tidy_on_file(cpp_file: Path) -> list[tuple[str, str, str, int]]:
    """Run clang-tidy on a single .cpp file and extract member violations.
    
    Returns list of (source_file, old_name, new_name, line_number) tuples.
    """
    violations = []
    
    try:
        result = subprocess.run(
            [
                "clang-tidy",
                "--config-file=/tmp/member-naming.yaml",
                "-p", str(ROOT),
                str(cpp_file)
            ],
            capture_output=True,
            text=True,
            timeout=120,
            cwd=ROOT
        )
        
        output = result.stdout + result.stderr
        lines = output.split('\n')
        
        i = 0
        while i < len(lines):
            match = WARNING_PATTERN.match(lines[i])
            if match:
                buck_path = match.group(1)
                line_num = int(match.group(2))
                access = match.group(4)  # private or protected
                old_name = match.group(5)
                
                # Look ahead for the suggested fix
                # Format is:
                #   70 |   Goals waitees;
                #      |         ^~~~~~~
                #      |         waitees_
                new_name = None
                for j in range(i + 1, min(i + 5, len(lines))):
                    fix_line = lines[j]
                    # Skip lines with ^~~~ (the caret pointing line)
                    if '^' in fix_line:
                        continue
                    # The fix line looks like: "      |         waitees_"
                    # Extract the last word after the |
                    fix_match = re.search(r'\|\s+(\w+_?)\s*$', fix_line)
                    if fix_match:
                        new_name = fix_match.group(1)
                        break
                
                if new_name and new_name != old_name:
                    source_file = map_buck_path_to_source(buck_path)
                    if source_file:
                        violations.append((str(source_file), old_name, new_name, line_num))
            i += 1
            
    except subprocess.TimeoutExpired:
        print(f"Timeout on {cpp_file}", file=sys.stderr)
    except Exception as e:
        print(f"Error on {cpp_file}: {e}", file=sys.stderr)
    
    return violations


def collect_all_violations() -> dict[str, list[tuple[str, str, int]]]:
    """Collect all private/protected member violations.
    
    Returns dict mapping source file -> list of (old_name, new_name, line_number).
    """
    # Find all .cpp files
    cpp_files = list(SRC_DIR.glob("**/*.cpp"))
    
    # Filter out src/straylight/ directory (relative to SRC_DIR, not parent repo)
    # Use relative_to to get path within the project
    cpp_files = [f for f in cpp_files if "straylight" not in str(f.relative_to(ROOT))]
    
    print(f"Scanning {len(cpp_files)} .cpp files...", file=sys.stderr)
    
    all_violations = defaultdict(list)
    
    # Process in parallel
    with ThreadPoolExecutor(max_workers=8) as executor:
        futures = {executor.submit(run_clang_tidy_on_file, f): f for f in cpp_files}
        
        for i, future in enumerate(as_completed(futures)):
            cpp_file = futures[future]
            if (i + 1) % 20 == 0:
                print(f"  Processed {i + 1}/{len(cpp_files)} files...", file=sys.stderr)
            
            violations = future.result()
            for source_file, old_name, new_name, line_num in violations:
                # Deduplicate by (file, old_name, new_name)
                key = (old_name, new_name, line_num)
                if key not in [(v[0], v[1], v[2]) for v in all_violations[source_file]]:
                    all_violations[source_file].append((old_name, new_name, line_num))
    
    return all_violations


def apply_renames(violations: dict[str, list[tuple[str, str, int]]]) -> int:
    """Apply renames to source files.
    
    Returns number of files modified.
    """
    files_modified = 0
    
    for source_file, renames in sorted(violations.items()):
        path = Path(source_file)
        if not path.exists():
            continue
            
        content = path.read_text()
        original = content
        
        for old_name, new_name, line_num in renames:
            # Only rename if it's a whole word and not already renamed
            # Use word boundaries to avoid partial matches
            pattern = rf'\b{re.escape(old_name)}\b'
            content = re.sub(pattern, new_name, content)
        
        if content != original:
            path.write_text(content)
            files_modified += 1
            print(f"Modified: {source_file} ({len(renames)} renames)")
    
    return files_modified


def main():
    import argparse
    parser = argparse.ArgumentParser(description="Fix private/protected member naming")
    parser.add_argument("--dry-run", action="store_true", help="Just show what would be changed")
    parser.add_argument("--single-file", help="Process single .cpp file for testing")
    args = parser.parse_args()
    
    if args.single_file:
        violations = run_clang_tidy_on_file(Path(args.single_file))
        for source_file, old_name, new_name, line_num in violations:
            print(f"{source_file}:{line_num}: {old_name} -> {new_name}")
        return
    
    violations = collect_all_violations()
    
    # Print summary
    total_renames = sum(len(v) for v in violations.values())
    print(f"\nFound {total_renames} private/protected member violations in {len(violations)} files", file=sys.stderr)
    
    if args.dry_run:
        for source_file, renames in sorted(violations.items()):
            for old_name, new_name, line_num in renames:
                print(f"{source_file}:{line_num}: {old_name} -> {new_name}")
        return
    
    # Apply renames
    files_modified = apply_renames(violations)
    print(f"\nModified {files_modified} files", file=sys.stderr)


if __name__ == "__main__":
    main()
