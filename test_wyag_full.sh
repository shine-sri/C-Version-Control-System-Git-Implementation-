#!/bin/bash
set -e

# Setup a clean test environment
TEST_DIR="wyag_ultimate_test"
CHECKOUT_DIR="wyag_checkout_test"
rm -rf "$TEST_DIR" "$CHECKOUT_DIR"
mkdir "$TEST_DIR"
cd "$TEST_DIR"

echo -e "\n==== 1. WYAG INIT ===="
../wyag init

echo -e "\n==== 2. CREATING FILES ===="
echo "Write Yourself a Git in C++" > readme.txt
mkdir src
echo "int main() { return 0; }" > src/main.cpp
echo "*.log" > .gitignore
echo "this should be ignored" > debug.log
ls -la

echo -e "\n==== 3. WYAG HASH-OBJECT ===="
echo "Hashing readme.txt directly into the object database:"
../wyag hash-object -w readme.txt

echo -e "\n==== 4. WYAG CHECK-IGNORE ===="
echo "Testing ignore rules on debug.log and readme.txt:"
../wyag check-ignore debug.log readme.txt || true

echo -e "\n==== 5. WYAG ADD & STATUS ===="
../wyag add readme.txt src/main.cpp .gitignore
../wyag status

echo -e "\n==== 6. WYAG COMMIT (First Commit) ===="
../wyag commit -m "Initial commit: Added base files"

echo -e "\n==== 7. MODIFY, WYAG RM, & WYAG ADD ===="
echo "Adding a new line to readme" >> readme.txt
echo "Removing main.cpp from the index and filesystem"
../wyag rm src/main.cpp
../wyag add readme.txt
../wyag status

echo -e "\n==== 8. WYAG COMMIT (Second Commit) ===="
../wyag commit -m "Second commit: Modified readme, removed src"

echo -e "\n==== 9. WYAG LOG ===="
echo "Generating Graphviz DOT output for the commit history:"
../wyag log HEAD

echo -e "\n==== 10. WYAG TAG & SHOW-REF ===="
echo "Creating an annotated tag 'v1.0' at HEAD:"
../wyag tag -a v1.0 HEAD
../wyag show-ref

echo -e "\n==== 11. WYAG REV-PARSE & CAT-FILE ===="
echo "Resolving HEAD to a full SHA-1 hash and reading the commit object:"
COMMIT_SHA=$(../wyag rev-parse HEAD)
../wyag cat-file commit $COMMIT_SHA

echo -e "\n==== 12. WYAG CHECKOUT ===="
echo "Checking out the repository state to a completely new, empty directory:"
mkdir ../$CHECKOUT_DIR
../wyag checkout HEAD ../$CHECKOUT_DIR
echo "Contents of the checked-out directory:"
ls -la ../$CHECKOUT_DIR

echo -e "\n==== CLEANUP ===="
echo "All systems operational. Tests completed successfully!"
cd ..
rm -rf "$TEST_DIR" "$CHECKOUT_DIR"
