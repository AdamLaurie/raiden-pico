# Add a postprocess step to convert the binary file to a C header file.
# It is necessary to separate this into a separate cmake file because cmake
# only supports running scripts during configure time. As such, we run this
# through the cmake during build time in order to be cross-platform.

# Read the binary file into a string and convert it to hex
file(READ ${input} PAYLOAD_BINARY_FILE_HEX HEX)

# Convert the hex string to a char array
string(REGEX REPLACE "(..)" "\\\\x\\1" PAYLOAD_BINARY_FILE_CONTENT "${PAYLOAD_BINARY_FILE_HEX}")

# Generate a header file with the binary content
file(WRITE ${output} "static const char payload[] = \"${PAYLOAD_BINARY_FILE_CONTENT}\";")
