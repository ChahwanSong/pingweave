# Collect files matching the specified prefixes.
shopt -s nullglob
files=(/dev/shm/{tcp,udp,roce,ib}_*)

# List the files that will be removed.
echo "Files to remove:"
printf "  %s\n" "${files[@]}"

for file in "${files[@]}"; do
    if [ -f "$file" ]; then
        if rm -f "$file"; then
            echo "Removed: $file"
        else
            echo "Error: Failed to remove $file"
        fi
    else
        echo "Skipping (not a regular file): $file"
    fi
done