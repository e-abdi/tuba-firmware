#!/bin/bash
# Generate build timestamp info
# Run this before building to create a fresh build_info.h

BUILD_INFO_FILE="include/build_info.h"

# Get current date and time
BUILD_DATE=$(date '+%Y-%m-%d')
BUILD_TIME=$(date '+%H:%M:%S')
BUILD_TIMESTAMP=$(date '+%s')  # Unix timestamp (seconds since epoch)

# Create the header file
cat > "$BUILD_INFO_FILE" << EOF
/* Auto-generated build info - do not edit */
#ifndef BUILD_INFO_H
#define BUILD_INFO_H

#define BUILD_DATE "$BUILD_DATE"
#define BUILD_TIME "$BUILD_TIME"
#define BUILD_TIMESTAMP $BUILD_TIMESTAMP

#endif /* BUILD_INFO_H */
EOF

echo "Generated $BUILD_INFO_FILE:"
cat "$BUILD_INFO_FILE"
