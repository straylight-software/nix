#!/usr/bin/env python3
"""
Fix private/protected member naming by adding trailing underscore.

This version uses a smarter replacement strategy that avoids collisions:
1. For each file, collect all private member renames
2. Apply renames only when there's no collision risk
3. Skip renames that would collide with common identifiers
"""

import subprocess
import re
import sys
from pathlib import Path
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed

# Root directory
ROOT = Path(__file__).resolve().parent.parent
SRC_DIR = ROOT / "src" / "nix"

# Common identifiers that shouldn't be globally renamed
# These are function names, type names, or other identifiers that
# would cause collisions if globally renamed
DANGEROUS_NAMES = {
    'fmt', 'hash', 'state', 'value', 'data', 'type', 'name', 'path',
    'root', 'size', 'id', 'fun', 'pos', 'env', 'eval', 'store',
    'ctx', 'ptr', 'ref', 'str', 'msg', 'err', 'log', 'out', 'in',
    'res', 'ret', 'buf', 'len', 'idx', 'key', 'val', 'src', 'dst',
    'fd', 'pid', 'uid', 'gid', 'mode', 'flags', 'attr', 'act',
    's', 'n', 'i', 'j', 'k', 'x', 'y', 'p', 'v', 'output',
}

# Regex to parse clang-tidy output
WARNING_PATTERN = re.compile(
    r"^(.+?):(\d+):(\d+): warning: invalid case style for (private|protected) member '(\w+)'"
)


def map_buck_path_to_source(buck_path: str) -> Path | None:
    """Map a buck-out header path back to the actual source file."""
    if "buck-headers" not in buck_path:
        return Path(buck_path) if Path(buck_path).exists() else None
    
    match = re.search(r"buck-headers/(.+)$", buck_path)
    if not match:
        return None
    
    header_rel = match.group(1)
    candidates = [
        ROOT / "src" / header_rel,
    ]
    
    for candidate in candidates:
        if candidate.exists():
            return candidate
    
    return None


def run_clang_tidy_on_file(cpp_file: Path) -> list[tuple[str, str, str, int]]:
    """Run clang-tidy on a single .cpp file and extract member violations."""
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
                old_name = match.group(5)
                
                # Look ahead for the suggested fix
                new_name = None
                for j in range(i + 1, min(i + 5, len(lines))):
                    fix_line = lines[j]
                    if '^' in fix_line:
                        continue
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
    """Collect all private/protected member violations."""
    cpp_files = list(SRC_DIR.glob("**/*.cpp"))
    cpp_files = [f for f in cpp_files if "straylight" not in str(f.relative_to(ROOT))]
    
    print(f"Scanning {len(cpp_files)} .cpp files...", file=sys.stderr)
    
    all_violations = defaultdict(list)
    
    with ThreadPoolExecutor(max_workers=8) as executor:
        futures = {executor.submit(run_clang_tidy_on_file, f): f for f in cpp_files}
        
        for i, future in enumerate(as_completed(futures)):
            if (i + 1) % 20 == 0:
                print(f"  Processed {i + 1}/{len(cpp_files)} files...", file=sys.stderr)
            
            violations = future.result()
            for source_file, old_name, new_name, line_num in violations:
                key = (old_name, new_name, line_num)
                if key not in [(v[0], v[1], v[2]) for v in all_violations[source_file]]:
                    all_violations[source_file].append((old_name, new_name, line_num))
    
    return all_violations


def is_safe_to_rename(file_path: str, old_name: str, new_name: str) -> bool:
    """Check if renaming is safe (won't cause collisions)."""
    # Skip dangerous common names
    if old_name in DANGEROUS_NAMES:
        return False
    
    # Skip single-letter names (too risky)
    if len(old_name) == 1:
        return False
    
    # Skip names that are already underscored (leading underscore)
    if old_name.startswith('_'):
        return False
    
    return True


def apply_targeted_rename(file_path: Path, old_name: str, new_name: str, line_num: int) -> bool:
    """Apply a single rename only around the specific line context."""
    content = file_path.read_text()
    lines = content.split('\n')
    
    # Find the line containing the member declaration
    if line_num > len(lines):
        return False
    
    # Find class/struct context
    class_start = None
    class_end = None
    brace_count = 0
    in_private = False
    
    # Search backwards for class/struct start
    for i in range(line_num - 1, -1, -1):
        line = lines[i]
        if re.match(r'^\s*(class|struct)\s+\w+', line):
            class_start = i
            break
    
    if class_start is None:
        return False
    
    # Find class end and track private section
    for i in range(class_start, len(lines)):
        line = lines[i]
        brace_count += line.count('{') - line.count('}')
        
        if re.match(r'^\s*private\s*:', line):
            in_private = True
        elif re.match(r'^\s*public\s*:', line) or re.match(r'^\s*protected\s*:', line):
            in_private = False
        
        if brace_count == 0 and i > class_start:
            class_end = i
            break
    
    if class_end is None:
        return False
    
    # Only rename within the class scope
    modified = False
    for i in range(class_start, class_end + 1):
        # Use word boundary replacement
        pattern = rf'\b{re.escape(old_name)}\b'
        new_line = re.sub(pattern, new_name, lines[i])
        if new_line != lines[i]:
            lines[i] = new_line
            modified = True
    
    if modified:
        file_path.write_text('\n'.join(lines))
    
    return modified


def apply_renames(violations: dict[str, list[tuple[str, str, int]]]) -> tuple[int, int]:
    """Apply renames to source files.
    
    Returns (files_modified, renames_skipped).
    """
    files_modified = 0
    renames_skipped = 0
    
    for source_file, renames in sorted(violations.items()):
        path = Path(source_file)
        if not path.exists():
            continue
        
        # Filter to safe renames only
        safe_renames = []
        for old_name, new_name, line_num in renames:
            if is_safe_to_rename(source_file, old_name, new_name):
                safe_renames.append((old_name, new_name, line_num))
            else:
                renames_skipped += 1
        
        if not safe_renames:
            continue
        
        content = path.read_text()
        original = content
        
        for old_name, new_name, line_num in safe_renames:
            # Use word boundary replacement
            pattern = rf'\b{re.escape(old_name)}\b'
            content = re.sub(pattern, new_name, content)
        
        if content != original:
            path.write_text(content)
            files_modified += 1
            print(f"Modified: {source_file} ({len(safe_renames)} renames, {len(renames) - len(safe_renames)} skipped)")
    
    return files_modified, renames_skipped


def main():
    import argparse
    parser = argparse.ArgumentParser(description="Fix private/protected member naming")
    parser.add_argument("--dry-run", action="store_true", help="Just show what would be changed")
    parser.add_argument("--show-skipped", action="store_true", help="Show skipped (dangerous) renames")
    parser.add_argument("--single-file", help="Process single .cpp file for testing")
    args = parser.parse_args()
    
    if args.single_file:
        violations = run_clang_tidy_on_file(Path(args.single_file))
        for source_file, old_name, new_name, line_num in violations:
            safe = is_safe_to_rename(source_file, old_name, new_name)
            status = "SAFE" if safe else "SKIP"
            print(f"[{status}] {source_file}:{line_num}: {old_name} -> {new_name}")
        return
    
    violations = collect_all_violations()
    
    # Count safe vs skipped
    safe_count = 0
    skip_count = 0
    for source_file, renames in violations.items():
        for old_name, new_name, line_num in renames:
            if is_safe_to_rename(source_file, old_name, new_name):
                safe_count += 1
            else:
                skip_count += 1
    
    print(f"\nFound {safe_count + skip_count} private/protected member violations in {len(violations)} files", file=sys.stderr)
    print(f"  Safe to rename: {safe_count}", file=sys.stderr)
    print(f"  Skipped (dangerous): {skip_count}", file=sys.stderr)
    
    if args.dry_run:
        for source_file, renames in sorted(violations.items()):
            for old_name, new_name, line_num in renames:
                safe = is_safe_to_rename(source_file, old_name, new_name)
                if safe or args.show_skipped:
                    status = "" if safe else "[SKIP] "
                    print(f"{status}{source_file}:{line_num}: {old_name} -> {new_name}")
        return
    
    # Apply renames
    files_modified, renames_skipped = apply_renames(violations)
    print(f"\nModified {files_modified} files, skipped {renames_skipped} dangerous renames", file=sys.stderr)


if __name__ == "__main__":
    main()
