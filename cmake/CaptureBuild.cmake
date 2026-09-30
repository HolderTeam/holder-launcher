# Snapshot the identity/configuration only after Holder has actually linked.
file(READ "${IDENTITY}" source)
file(READ "${CONFIG}" metadata)
file(SHA256 "${BINARY}" binary_hash)
string(JSON metadata SET "${metadata}" source "${source}")
string(JSON metadata SET "${metadata}" binary_sha256 "\"${binary_hash}\"")
file(WRITE "${OUTPUT}" "${metadata}\n")
