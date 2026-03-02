#!/bin/bash
# Generate version header with build counter

VERSION_FILE="include/version.h"
VERSION_COUNTER_FILE=".build_version"

# Initialize counter if it doesn't exist
if [ ! -f "$VERSION_COUNTER_FILE" ]; then
    echo "0" > "$VERSION_COUNTER_FILE"
fi

# Increment counter
BUILD_NUM=$(($(cat "$VERSION_COUNTER_FILE") + 1))
echo "$BUILD_NUM" > "$VERSION_COUNTER_FILE"

# Generate version header
cat > "$VERSION_FILE" << EOF
#ifndef VERSION_H
#define VERSION_H

#define BUILD_NUMBER $BUILD_NUM
#define BUILD_DATE "$( date '+%Y-%m-%d %H:%M:%S' )"

#endif /* VERSION_H */
EOF

echo "Generated $VERSION_FILE with BUILD_NUMBER=$BUILD_NUM"
