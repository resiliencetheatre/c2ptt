#!/bin/sh

FILE="$1"

if [ -z "$FILE" ]; then
    echo "No filename given" >&2
    exit 1
fi

if [ ! -f "$FILE" ]; then
    echo "Not a regular file: $FILE" >&2
    exit 1
fi

echo "Processing file: $FILE"

# Put your real work here.
# Example:
# cat "$FILE"
# some_command "$FILE"

# Handler deletes the file after successful processing
rm -f -- "$FILE"
