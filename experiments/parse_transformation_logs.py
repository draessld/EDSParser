#!/usr/bin/env python3
"""
Parse transformation logs from EDS/l-EDS transformations and extract performance statistics.

Usage:
    python parse_transformation_logs.py <dataset_dir> [--output OUTPUT.csv]

Example:
    python parse_transformation_logs.py datasets/SARS_cov2 --output sars_cov2_performance.csv
"""

import argparse
import re
from pathlib import Path
from typing import Dict, List, Optional
import csv


def parse_performance_line(line: str) -> Optional[Dict[str, float]]:
    """
    Parse performance line from log file.
    Format: [Performance] Runtime: 0.01s | Peak Memory: 5.1 MB

    Returns dict with 'runtime_seconds' and 'memory_mb' or None if not matched.
    """
    pattern = r'\[Performance\]\s+Runtime:\s+([\d.]+)s\s+\|\s+Peak Memory:\s+([\d.]+)\s+MB'
    match = re.search(pattern, line)
    if match:
        return {
            'runtime_seconds': float(match.group(1)),
            'memory_mb': float(match.group(2))
        }
    return None


def parse_context_length(content: str) -> Optional[int]:
    """Extract context length from l-EDS transformation log."""
    pattern = r'Context length:\s+(\d+)'
    match = re.search(pattern, content)
    return int(match.group(1)) if match else None


def parse_threads(content: str) -> int:
    """Extract thread count from log. Default to 1 if not found."""
    pattern = r'Threads:\s+(\d+)'
    match = re.search(pattern, content)
    return int(match.group(1)) if match else 1


def parse_transformation_type(content: str) -> str:
    """Determine transformation type from log content."""
    if 'MSA → EDS transformation' in content:
        return 'MSA→EDS'
    elif 'EDS → l-EDS transformation' in content:
        return 'EDS→l-EDS'
    elif 'MSA → l-EDS transformation' in content:
        return 'MSA→l-EDS'
    elif 'VCF → EDS transformation' in content:
        return 'VCF→EDS'
    elif 'VCF → l-EDS transformation' in content:
        return 'VCF→l-EDS'
    return 'Unknown'


def parse_log_file(log_path: Path) -> Optional[Dict]:
    """
    Parse a single log file and extract all relevant information.

    Returns dict with:
        - variant_name: str
        - transformation_type: str
        - context_length: int or None
        - runtime_seconds: float
        - memory_mb: float
        - threads: int
    """
    try:
        content = log_path.read_text()

        # Extract performance metrics
        perf_data = None
        for line in content.split('\n'):
            perf_data = parse_performance_line(line)
            if perf_data:
                break

        if not perf_data:
            print(f"Warning: No performance data found in {log_path}")
            return None

        # Extract variant name from filename
        # Remove .eds.log or .leds.log suffix
        variant_name = log_path.stem
        if variant_name.endswith('.eds'):
            variant_name = variant_name[:-4]
        elif variant_name.endswith('.leds'):
            variant_name = variant_name[:-5]

        return {
            'variant_name': variant_name,
            'transformation_type': parse_transformation_type(content),
            'context_length': parse_context_length(content),
            'runtime_seconds': perf_data['runtime_seconds'],
            'memory_mb': perf_data['memory_mb'],
            'threads': parse_threads(content),
            'log_file': str(log_path.relative_to(log_path.parents[2]))  # Relative to dataset dir parent
        }

    except Exception as e:
        print(f"Error parsing {log_path}: {e}")
        return None


def find_log_files(dataset_dir: Path) -> List[Path]:
    """
    Find all .eds.log and .leds.log files in the dataset directory.

    Looks in:
        - eds/*.eds.log
        - <l>_leds/*.leds.log (e.g., 3_leds, 5_leds, 10_leds, etc.)
    """
    log_files = []

    # Find EDS logs
    eds_dir = dataset_dir / 'eds'
    if eds_dir.exists():
        log_files.extend(eds_dir.glob('*.eds.log'))

    # Find l-EDS logs (all *_leds directories)
    for leds_dir in dataset_dir.glob('*_leds'):
        if leds_dir.is_dir():
            log_files.extend(leds_dir.glob('*.leds.log'))

    return sorted(log_files)


def main():
    parser = argparse.ArgumentParser(
        description='Parse EDS/l-EDS transformation logs and extract performance statistics',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Parse logs and print to console
  python parse_transformation_logs.py datasets/SARS_cov2

  # Parse logs and save to CSV
  python parse_transformation_logs.py datasets/SARS_cov2 --output sars_cov2_perf.csv

  # Parse with custom column separator
  python parse_transformation_logs.py datasets/SARS_cov2 --output stats.csv --delimiter ";"
        """
    )

    parser.add_argument('dataset_dir', type=Path,
                        help='Dataset directory containing eds/ and *_leds/ subdirectories')
    parser.add_argument('-o', '--output', type=Path,
                        help='Output CSV file (default: print to console)')
    parser.add_argument('-d', '--delimiter', default=',',
                        help='CSV delimiter (default: comma)')
    parser.add_argument('-v', '--verbose', action='store_true',
                        help='Print verbose output')

    args = parser.parse_args()

    # Validate dataset directory
    if not args.dataset_dir.exists():
        print(f"Error: Dataset directory '{args.dataset_dir}' does not exist")
        return 1

    if not args.dataset_dir.is_dir():
        print(f"Error: '{args.dataset_dir}' is not a directory")
        return 1

    # Find all log files
    log_files = find_log_files(args.dataset_dir)

    if not log_files:
        print(f"No log files found in {args.dataset_dir}")
        print(f"Expected structure:")
        print(f"  {args.dataset_dir}/eds/*.eds.log")
        print(f"  {args.dataset_dir}/*_leds/*.leds.log")
        return 1

    if args.verbose:
        print(f"Found {len(log_files)} log files")

    # Parse all log files
    results = []
    for log_file in log_files:
        if args.verbose:
            print(f"Parsing {log_file}...")

        data = parse_log_file(log_file)
        if data:
            results.append(data)

    if not results:
        print("No data extracted from log files")
        return 1

    # Sort results by variant name and context length
    results.sort(key=lambda x: (x['variant_name'], x['context_length'] or 0))

    # Define CSV columns
    fieldnames = [
        'variant_name',
        'transformation_type',
        'context_length',
        'runtime_seconds',
        'memory_mb',
        'threads',
        'log_file'
    ]

    # Output results
    if args.output:
        # Write to CSV file
        with open(args.output, 'w', newline='') as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames, delimiter=args.delimiter)
            writer.writeheader()
            writer.writerows(results)

        print(f"Successfully parsed {len(results)} log files")
        print(f"Results written to: {args.output}")

        # Print summary statistics
        print(f"\nSummary:")
        print(f"  Total transformations: {len(results)}")

        by_type = {}
        for r in results:
            t = r['transformation_type']
            by_type[t] = by_type.get(t, 0) + 1

        for trans_type, count in sorted(by_type.items()):
            print(f"  {trans_type}: {count}")

    else:
        # Print to console (pretty format)
        print(f"\n{'Variant':<20} {'Type':<12} {'L':<5} {'Runtime(s)':<12} {'Memory(MB)':<12} {'Threads':<8}")
        print("-" * 80)

        for r in results:
            ctx_len = str(r['context_length']) if r['context_length'] is not None else '-'
            print(f"{r['variant_name']:<20} {r['transformation_type']:<12} {ctx_len:<5} "
                  f"{r['runtime_seconds']:<12.3f} {r['memory_mb']:<12.1f} {r['threads']:<8}")

        print(f"\nTotal: {len(results)} transformations")

    return 0


if __name__ == '__main__':
    exit(main())
