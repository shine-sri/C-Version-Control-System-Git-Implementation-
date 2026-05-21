#!/bin/bash
set -e

# Setup a clean test environment
TEST_DIR="wyag_test_repo"
rm -rf "$TEST_DIR"
mkdir "$TEST_DIR"
cd "$TEST_DIR"

echo "==== 1. Initializing real Git repo ===="
git init

echo "==== 2. Creating test files ===="
mkdir src
echo "int main() { return 0; }" > src/main.cpp
echo "ignored_data" > ignored.txt
echo "*.txt" > .gitignore
echo "temp data" > temp.log

echo "==== 3. Staging files with real Git (Creating the binary index) ===="
git add .gitignore src/main.cpp
git commit -m "Initial commit"

echo "==== 4. Modifying working tree to test status ===="
echo "// modified" >> src/main.cpp
echo "new file" > new_untracked.c

echo "==== 5. Testing 'wyag ls-files --verbose' ===="
../wyag ls-files --verbose

echo "==== 6. Testing 'wyag check-ignore' ===="
../wyag check-ignore ignored.txt src/main.cpp || true

echo "==== 7. Testing 'wyag status' ===="
../wyag status

echo "Tests complete. Cleaning up."
cd ..
rm -rf "$TEST_DIR"