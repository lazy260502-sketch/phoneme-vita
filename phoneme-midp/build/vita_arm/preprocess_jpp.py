#!/usr/bin/env python3
"""
Simple preprocessor for phoneME .jpp files.
Handles #ifndef, #ifdef, #else, #endif directives.
By default, ENABLE_CDC, ENABLE_CHAMELEON, ENABLE_I3_TEST, ENABLE_GCI are undefined.
"""

import os
import sys
import re

# Defines for our Vita build
# We're building CLDC (not CDC), so ENABLE_CDC is NOT defined
# We're NOT using Chameleon, so ENABLE_CHAMELEON is NOT defined
# We're NOT running I3 tests, so ENABLE_I3_TEST is NOT defined
# We're NOT using GCI, so ENABLE_GCI is NOT defined
DEFINES = set()

def preprocess_jpp(input_path, output_path):
    """Preprocess a .jpp file and write the result to a .java file."""
    with open(input_path, 'r') as f:
        lines = f.readlines()
    
    output_lines = []
    # Stack of (condition_true, has_been_true, is_else)
    # condition_true: whether the current block's condition is true
    # has_been_true: whether any branch in this #if/#ifdef/#ifndef has been taken
    # is_else: whether we're in the #else branch
    stack = []
    
    for line in lines:
        stripped = line.strip()
        
        # Handle #ifndef
        if stripped.startswith('#ifndef'):
            macro = stripped.split()[1]
            condition = macro not in DEFINES
            stack.append([condition, condition, False])
            continue
        
        # Handle #ifdef
        if stripped.startswith('#ifdef'):
            macro = stripped.split()[1]
            condition = macro in DEFINES
            stack.append([condition, condition, False])
            continue
        
        # Handle #if
        if stripped.startswith('#if ') or stripped.startswith('#if\t'):
            # For simplicity, treat unknown #if as false
            stack.append([False, False, False])
            continue
        
        # Handle #elif
        if stripped.startswith('#elif'):
            if stack:
                stack[-1][2] = True  # Mark that we've seen #else/#elif
                # For simplicity, treat as false
                stack[-1][0] = False
            continue
        
        # Handle #else
        if stripped.startswith('#else'):
            if stack:
                parent_true = stack[-1][1]
                # #else is active if no previous branch was true
                stack[-1][0] = not stack[-1][1]
                stack[-1][2] = True
                # Update has_been_true
                if stack[-1][0]:
                    stack[-1][1] = True
            continue
        
        # Handle #endif
        if stripped.startswith('#endif'):
            if stack:
                stack.pop()
            continue
        
        # Regular line - check if we should include it
        if stack:
            # Check if the current block is active
            active = stack[-1][0]
            if not active:
                continue
        
        output_lines.append(line)
    
    with open(output_path, 'w') as f:
        f.writelines(output_lines)

def main():
    src_dir = '/home/zyb/vitasdk/samples/j2me/phoneme-midp/src'
    out_dir = '/home/zyb/vitasdk/samples/j2me/phoneme-midp/build/vita_arm/all_sources'
    
    os.makedirs(out_dir, exist_ok=True)
    
    # Find all .jpp files
    jpp_files = []
    for root, dirs, files in os.walk(src_dir):
        for f in files:
            if f.endswith('.jpp'):
                jpp_files.append(os.path.join(root, f))
    
    for jpp_path in jpp_files:
        # Compute relative path
        rel_path = os.path.relpath(jpp_path, src_dir)
        # Change extension from .jpp to .java
        java_rel = rel_path[:-4] + '.java'
        java_path = os.path.join(out_dir, java_rel)
        
        # Create output directory
        os.makedirs(os.path.dirname(java_path), exist_ok=True)
        
        preprocess_jpp(jpp_path, java_path)
        print(f"Preprocessed: {rel_path} -> {java_rel}")
    
    print(f"\nTotal: {len(jpp_files)} files preprocessed")

if __name__ == '__main__':
    main()
