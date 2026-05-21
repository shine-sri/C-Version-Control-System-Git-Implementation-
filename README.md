A lightweight, systems-level Version Control System written in C++17. Reconstructs Git’s object database, Merkle DAG tracking, and binary index parsing from the ground up using strict Object-Oriented Programming (OOP) principles.

## Features

- **Cryptographic Hashing:** Native SHA-1 integration and `zlib` blob compression.
- **Binary Index Parsing:** Optimized `.git/index` staging area parser with strict Endianness (byte-order) management.
- **POSIX Integration:** Nanosecond-precision `status` diffing utilizing `<sys/stat.h>`.
- **Merkle DAG Generation:** Recursive tree building and traversal algorithms to manage repository history.

## Supported Commands

The engine successfully mimics the following core Git sub-commands:
- **Plumbing:** `hash-object`, `cat-file`, `ls-tree`, `ls-files`, `check-ignore`, `rev-parse`, `show-ref`
- **Porcelain:** `init`, `add`, `rm`, `commit`, `status`, `log`, `checkout`, `tag`

## Build & Run (macOS)

### Prerequisites
- C++17 Compatible Compiler (`clang++`)
- `zlib` library (natively available on macOS)
- macOS Native `CommonCrypto` (for SHA-1)

### Compilation
Build the executable using the following command:
```bash
clang++ -std=c++17 main.cpp libwyag.cpp -o wyag -lz
```

Quick Start
```
Bash
# Initialize a new repository
./wyag init

# Create and stage a file
echo "Hello GitCore" > readme.txt
./wyag add readme.txt

# Commit the changes
./wyag commit -m "Initial commit"

# Check the history
./wyag log HEAD
```

Acknowledgements
This C++ adaptation was built following the excellent foundational concepts outlined in the tutorial: Write Yourself a Git by Thibault Polge.
