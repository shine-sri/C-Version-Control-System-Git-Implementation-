#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <memory>
#include <map>
#include <cstdint>
#include <optional>
#include <variant>
#include <chrono>

namespace fs = std::filesystem;

// -------------------------------------------------------------------------
// GIT REPOSITORY CLASS
// -------------------------------------------------------------------------
/**
 * @brief Represents a physical Git repository on disk.
 * Encapsulates the worktree (user files) and the gitdir (.git folder).
 */
class GitRepository {
public:
    fs::path worktree;
    fs::path gitdir;

    GitRepository(const fs::path& path, bool force = false);
};

// -------------------------------------------------------------------------
// GIT OBJECTS (Polymorphic Hierarchy)
// -------------------------------------------------------------------------
/**
 * @brief Base class for all Git objects (Blobs, Commits, Trees, Tags).
 * Requires implementation of serialization to byte arrays and formatting.
 */
class GitObject {
public:
    virtual ~GitObject() = default;
    virtual std::string serialize() = 0;
    virtual void deserialize(const std::string& data) = 0;
    virtual std::string format() const = 0;
};

// --- BLOBS ---
// Represents the raw contents of a file.
class GitBlob : public GitObject {
public:
    std::string blobdata;
    GitBlob(const std::string& data = "") { if (!data.empty()) deserialize(data); }
    std::string serialize() override { return blobdata; }
    void deserialize(const std::string& data) override { blobdata = data; }
    std::string format() const override { return "blob"; }
};

// --- COMMITS ---
// Key-Value List with Message (KVLM). Used to store commit metadata.
struct KVLMData {
    std::map<std::string, std::vector<std::string>> fields;
    std::string message;
};

class GitCommit : public GitObject {
public:
    KVLMData kvlm;
    GitCommit(const std::string& data = "") { if (!data.empty()) deserialize(data); }
    std::string serialize() override;
    void deserialize(const std::string& data) override;
    std::string format() const override { return "commit"; }
};

// --- TAGS ---
// Tags are structurally identical to commits, just with a different format tag.
class GitTag : public GitCommit {
public:
    GitTag(const std::string& data = "") : GitCommit(data) {}
    std::string format() const override { return "tag"; }
};

// --- TREES ---
// Represents a directory node in the Merkle DAG.
struct GitTreeLeaf {
    std::string mode;
    std::string path;
    std::string sha; 
};

class GitTree : public GitObject {
public:
    std::vector<GitTreeLeaf> items;
    GitTree(const std::string& data = "") { if (!data.empty()) deserialize(data); }
    std::string serialize() override;
    void deserialize(const std::string& data) override;
    std::string format() const override { return "tree"; }
};

// -------------------------------------------------------------------------
// INDEX & STAGING AREA
// -------------------------------------------------------------------------
/**
 * @brief Represents a single file staged in the .git/index.
 * Contains precise filesystem metadata (timestamps, inodes) to optimize
 * diffing algorithms without needing to re-hash the file.
 */
struct GitIndexEntry {
    uint32_t ctime_s, ctime_ns;
    uint32_t mtime_s, mtime_ns;
    uint32_t dev, ino;
    uint32_t mode_type, mode_perms;
    uint32_t uid, gid, fsize;
    std::string sha; 
    bool flag_assume_valid;
    uint16_t flag_stage;
    std::string name;
};

class GitIndex {
public:
    uint32_t version;
    std::vector<GitIndexEntry> entries;
    GitIndex(uint32_t v = 2) : version(v) {}
};

GitIndex index_read(const GitRepository& repo);
void index_write(const GitRepository& repo, const GitIndex& index);
void rm(const GitRepository& repo, const std::vector<std::string>& paths, bool delete_file = true, bool skip_missing = false);
void add(const GitRepository& repo, const std::vector<std::string>& paths);

// -------------------------------------------------------------------------
// IGNORE SYSTEM
// -------------------------------------------------------------------------
struct IgnoreRule {
    std::string pattern;
    bool exclude; // True if ignored, False if included (starts with '!')
};

using RuleSet = std::vector<IgnoreRule>;

class GitIgnore {
public:
    std::vector<RuleSet> absolute;             // Global/System rules
    std::map<std::string, RuleSet> scoped;     // Directory-specific rules (.gitignore)
};

std::optional<IgnoreRule> gitignore_parse1(std::string raw);
RuleSet gitignore_parse(const std::vector<std::string>& lines);
GitIgnore gitignore_read(const GitRepository& repo);
std::optional<bool> check_ignore1(const RuleSet& rules, const std::string& path);
std::optional<bool> check_ignore_scoped(const std::map<std::string, RuleSet>& rules, const std::string& path);
bool check_ignore_absolute(const std::vector<RuleSet>& rules, const std::string& path);
bool check_ignore(const GitIgnore& rules, const std::string& path);

// -------------------------------------------------------------------------
// UTILITIES & HELPERS
// -------------------------------------------------------------------------
KVLMData kvlm_parse(const std::string& raw, size_t start = 0);
std::string kvlm_serialize(const KVLMData& kvlm);
std::string sha1_hex(const std::string& data);
std::string zlib_compress(const std::string& data);
std::string zlib_decompress(const std::string& data);
std::unique_ptr<GitObject> object_read(const GitRepository& repo, const std::string& sha);
std::string object_write(GitObject& obj, const GitRepository* repo = nullptr);
std::string object_hash(std::istream& fd, const std::string& fmt, const GitRepository* repo = nullptr);
std::vector<std::string> object_resolve(const GitRepository& repo, std::string name);
std::string object_find(const GitRepository& repo, const std::string& name, const std::string& fmt = "", bool follow = true);
std::string ref_resolve(const GitRepository& repo, const std::string& ref);
std::map<std::string, std::string> ref_list(const GitRepository& repo, fs::path path = "");
void show_ref(const GitRepository& repo, const std::map<std::string, std::string>& refs, bool with_hash = true, std::string prefix = "");
void tag_create(const GitRepository& repo, const std::string& name, const std::string& ref, bool create_tag_object = false);
void ref_create(const GitRepository& repo, const std::string& ref_name, const std::string& sha);
fs::path repo_path(const GitRepository& repo, const fs::path& path);
fs::path repo_dir(const GitRepository& repo, const fs::path& path, bool mkdir = false);
fs::path repo_file(const GitRepository& repo, const fs::path& path, bool mkdir = false);
std::string repo_default_config();
GitRepository repo_create(const fs::path& path);
std::unique_ptr<GitRepository> repo_find(fs::path path = ".", bool required = true);
std::string branch_get_active(const GitRepository& repo);
std::map<std::string, std::string> tree_to_dict(const GitRepository& repo, const std::string& ref, const std::string& prefix = "");
std::string tree_from_index(const GitRepository& repo, const GitIndex& index);
std::string commit_create(const GitRepository& repo, const std::string& tree, const std::string& parent, const std::string& author, const std::string& message);
std::string gitconfig_user_get();

// -------------------------------------------------------------------------
// COMMAND DECLARATIONS
// -------------------------------------------------------------------------
void cmd_init(const std::vector<std::string>& args);
void cmd_hash_object(const std::vector<std::string>& args);
void cmd_cat_file(const std::vector<std::string>& args);
void cmd_log(const std::vector<std::string>& args);
void cmd_ls_tree(const std::vector<std::string>& args);
void cmd_checkout(const std::vector<std::string>& args);
void cmd_show_ref(const std::vector<std::string>& args);
void cmd_tag(const std::vector<std::string>& args);
void cmd_rev_parse(const std::vector<std::string>& args);
void cmd_ls_files(const std::vector<std::string>& args);
void cmd_check_ignore(const std::vector<std::string>& args);
void cmd_status(const std::vector<std::string>& args);
void cmd_commit(const std::vector<std::string>& args);
void cmd_add(const std::vector<std::string>& args);
void cmd_rm(const std::vector<std::string>& args);