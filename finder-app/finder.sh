#!/bin/sh

filesdir=$1
searchstr=$2

# Check if either variable is empty
if [ -z "$filesdir" ] || [ -z "$searchstr" ]; then
    echo "ERROR: Not all arguments were provided"
    echo "Example: finder.sh /tmp/aesd/assignment1 linux"
    exit 1
fi

# Check if provided is a directory
if [ ! -d "$filesdir" ]; then
    echo "ERROR: $filesdir is not a directory"
    exit 1
fi

# Count total number of files in the directory and subdirectories
num_files=$(find "$filesdir" -type f | wc -l)

# Count total number of matching lines across all files
num_matching_lines=$(grep -r -F "$searchstr" "$filesdir"| wc -l)

echo "The number of files are $num_files and the number of matching lines are $num_matching_lines"

