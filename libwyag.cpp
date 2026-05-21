#include "libwyag.h"
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <regex>
#include <sstream>
#include <iomanip>
#include <set>
#include <algorithm> 

// macOS POSIX & Systems Headers
#include <pwd.h>     
#include <grp.h>     
#include <fnmatch.h> 
#include <zlib.h>    
#include <arpa/inet.h>  // Used for byte-order swapping (htonl, ntohl)
#include <sys/stat.h>

// macOS Native Cryptography for SHA-1
#include <CommonCrypto/CommonDigest.h>

// -------------------------------------------------------------------------
// GIT REPOSITORY IMPLEMENTATION
// -------------------------------------------------------------------------

GitRepository::GitRepository(const fs::path& path, bool force) {
    worktree = path;
    gitdir = path / ".git";

    if (!(force || fs::is_directory(gitdir))) {
        throw std::runtime_error("Not a Git repository " + path.string());
    }

    fs::path cf = repo_file(*this, "config");
    
    // Ensure repository format version is compatible (Git uses version 0 for standard repos)
    if (fs::exists(cf)) {
        if (!force) {
            std::ifstream file(cf);
            std::string line;
            std::regex re(R"(repositoryformatversion\s*=\s*(\d+))");
            std::smatch match;
            int vers = -1;

            while (std::getline(file, line)) {
                if (std::regex_search(line, match, re)) {
                    vers = std::stoi(match[1]);
                    break;
                }
            }
            if (vers != 0) {
                throw std::runtime_error("Unsupported repositoryformatversion: " + std::to_string(vers));
            }
        }
    } else if (!force) {
        throw std::runtime_error("Configuration file missing");
    }
}

fs::path repo_path(const GitRepository& repo, const fs::path& path) { return repo.gitdir / path; }

fs::path repo_dir(const GitRepository& repo, const fs::path& path, bool mkdir) {
    fs::path p = repo_path(repo, path);
    if (fs::exists(p)) {
        if (fs::is_directory(p)) return p;
        else throw std::runtime_error("Not a directory: " + p.string());
    }
    if (mkdir) {
        fs::create_directories(p);
        return p;
    }
    return ""; 
}

fs::path repo_file(const GitRepository& repo, const fs::path& path, bool mkdir) {
    if (repo_dir(repo, path.parent_path(), mkdir) != "") return repo_path(repo, path);
    return "";
}

std::string repo_default_config() {
    return "[core]\n\trepositoryformatversion = 0\n\tfilemode = false\n\tbare = false\n";
}

GitRepository repo_create(const fs::path& path) {
    GitRepository repo(path, true);
    if (fs::exists(repo.worktree)) {
        if (!fs::is_directory(repo.worktree)) throw std::runtime_error(path.string() + " is not a directory!");
        if (fs::exists(repo.gitdir) && !fs::is_empty(repo.gitdir)) throw std::runtime_error(path.string() + " is not empty!");
    } else {
        fs::create_directories(repo.worktree);
    }
    
    // Create the foundational directory structure inside .git
    repo_dir(repo, "branches", true);
    repo_dir(repo, "objects", true);
    repo_dir(repo, fs::path("refs") / "tags", true);
    repo_dir(repo, fs::path("refs") / "heads", true);

    std::ofstream desc(repo_file(repo, "description"));
    desc << "Unnamed repository; edit this file 'description' to name the repository.\n";
    desc.close();

    std::ofstream head(repo_file(repo, "HEAD"));
    head << "ref: refs/heads/master\n";  
    head.close();

    std::ofstream conf(repo_file(repo, "config"));
    conf << repo_default_config();
    conf.close();

    return repo;
}

std::unique_ptr<GitRepository> repo_find(fs::path path, bool required) {
    path = fs::absolute(path);
    if (fs::is_directory(path / ".git")) return std::make_unique<GitRepository>(path);
    fs::path parent = path.parent_path();
    if (parent == path) {
        if (required) throw std::runtime_error("No git directory.");
        else return nullptr;
    }
    return repo_find(parent, required);
}

// -------------------------------------------------------------------------
// CRYPTOGRAPHY & COMPRESSION
// -------------------------------------------------------------------------

/**
 * @brief Computes a SHA-1 hash utilizing macOS CommonCrypto.
 */
std::string sha1_hex(const std::string& data) {
    unsigned char hash[CC_SHA1_DIGEST_LENGTH];
    CC_SHA1(data.c_str(), data.size(), hash);
    std::stringstream ss;
    for (int i = 0; i < CC_SHA1_DIGEST_LENGTH; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }
    return ss.str();
}

/**
 * @brief Compresses a string using the zlib DEFLATE algorithm.
 * Required for packing GitObjects into the .git/objects/ database.
 */
std::string zlib_compress(const std::string& str) {
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (deflateInit(&zs, Z_BEST_COMPRESSION) != Z_OK) throw std::runtime_error("deflateInit failed");
    zs.next_in = (Bytef*)str.data();
    zs.avail_in = str.size();
    int ret;
    char outbuffer[32768];
    std::string outstring;
    do {
        zs.next_out = reinterpret_cast<Bytef*>(outbuffer);
        zs.avail_out = sizeof(outbuffer);
        ret = deflate(&zs, Z_FINISH);
        if (outstring.size() < zs.total_out) outstring.append(outbuffer, zs.total_out - outstring.size());
    } while (ret == Z_OK);
    deflateEnd(&zs);
    return outstring;
}

std::string zlib_decompress(const std::string& str) {
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit(&zs) != Z_OK) throw std::runtime_error("inflateInit failed");
    zs.next_in = (Bytef*)str.data();
    zs.avail_in = str.size();
    int ret;
    char outbuffer[32768];
    std::string outstring;
    do {
        zs.next_out = reinterpret_cast<Bytef*>(outbuffer);
        zs.avail_out = sizeof(outbuffer);
        ret = inflate(&zs, 0);
        if (outstring.size() < zs.total_out) outstring.append(outbuffer, zs.total_out - outstring.size());
    } while (ret == Z_OK);
    inflateEnd(&zs);
    return outstring;
}

// -------------------------------------------------------------------------
// KVLM PARSING (Commits)
// -------------------------------------------------------------------------
/**
 * @brief Parses Key-Value List with Message format.
 * Commits are structured as key-value pairs (tree, parent, author) followed by
 * a blank line and the commit message.
 */
KVLMData kvlm_parse(const std::string& raw, size_t start) {
    KVLMData result;
    while (start < raw.size()) {
        size_t spc = raw.find(' ', start);
        size_t nl = raw.find('\n', start);
        if (spc == std::string::npos || nl < spc) {
            // No more keys, the rest is the commit message
            if (nl == start) result.message = raw.substr(start + 1);
            else result.message = raw.substr(start); 
            break;
        }
        std::string key = raw.substr(start, spc - start);
        size_t end = start;
        
        // Handle values spanning multiple lines (indentation)
        while (true) {
            end = raw.find('\n', end + 1);
            if (end == std::string::npos || end + 1 >= raw.size() || raw[end + 1] != ' ') break;
        }
        std::string raw_value = raw.substr(spc + 1, end - (spc + 1));
        std::string value;
        for (size_t i = 0; i < raw_value.size(); ++i) {
            if (raw_value[i] == '\n' && i + 1 < raw_value.size() && raw_value[i+1] == ' ') {
                value += '\n';
                ++i; 
            } else {
                value += raw_value[i];
            }
        }
        result.fields[key].push_back(value);
        start = end + 1;
    }
    return result;
}

std::string kvlm_serialize(const KVLMData& kvlm) {
    std::string ret;
    for (const auto& [key, values] : kvlm.fields) {
        for (const auto& v : values) {
            ret += key + " ";
            for (char c : v) {
                ret += c;
                if (c == '\n') ret += " "; // Encode multiline values
            }
            ret += "\n";
        }
    }
    ret += "\n" + kvlm.message;
    return ret;
}

void GitCommit::deserialize(const std::string& data) { kvlm = kvlm_parse(data); }
std::string GitCommit::serialize() { return kvlm_serialize(kvlm); }

// -------------------------------------------------------------------------
// TREE PARSING
// -------------------------------------------------------------------------
void GitTree::deserialize(const std::string& data) {
    size_t pos = 0;
    while (pos < data.size()) {
        size_t spc = data.find(' ', pos);
        if (spc == std::string::npos) break;
        std::string mode = data.substr(pos, spc - pos);
        if (mode.size() == 5) mode = "0" + mode; 
        size_t null_idx = data.find('\0', spc);
        if (null_idx == std::string::npos) break;
        std::string path = data.substr(spc + 1, null_idx - spc - 1);
        std::string raw_sha = data.substr(null_idx + 1, 20); // SHA is stored as raw binary
        
        // Convert binary SHA back to hex string
        std::stringstream ss;
        for (unsigned char c : raw_sha) ss << std::hex << std::setw(2) << std::setfill('0') << (int)c;
        items.push_back({mode, path, ss.str()});
        pos = null_idx + 21;
    }
}

std::string GitTree::serialize() {
    // Trees must be strictly sorted to ensure deterministic hashing
    std::sort(items.begin(), items.end(), [](const GitTreeLeaf& a, const GitTreeLeaf& b) {
        std::string pathA = a.path + (a.mode[0] == '4' ? "/" : "");
        std::string pathB = b.path + (b.mode[0] == '4' ? "/" : "");
        return pathA < pathB;
    });
    std::string ret;
    for (const auto& item : items) {
        ret += item.mode + " " + item.path + '\0';
        for (size_t i = 0; i < 40; i += 2) ret += (char)std::stoi(item.sha.substr(i, 2), nullptr, 16);
    }
    return ret;
}

// -------------------------------------------------------------------------
// INDEX PARSING (The Staging Area)
// -------------------------------------------------------------------------
std::vector<unsigned char> hex_to_bytes(const std::string& hex) {
    std::vector<unsigned char> bytes;
    for (unsigned int i = 0; i < hex.length(); i += 2) {
        bytes.push_back((unsigned char) strtol(hex.substr(i, 2).c_str(), nullptr, 16));
    }
    return bytes;
}

/**
 * @brief Reads the highly optimized binary .git/index file.
 * The index uses Big-Endian byte order. `ntohl` is used to convert
 * Network Byte Order (Big-Endian) to Host Byte Order (Little-Endian on macOS).
 */
GitIndex index_read(const GitRepository& repo) {
    fs::path index_file = repo_file(repo, "index");
    if (!fs::exists(index_file)) return GitIndex();

    std::ifstream file(index_file, std::ios::binary | std::ios::ate);
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<unsigned char> raw(size);
    if (!file.read(reinterpret_cast<char*>(raw.data()), size)) throw std::runtime_error("Failed to read index file");
    if (size < 12) throw std::runtime_error("Index file too small");

    std::string signature(raw.begin(), raw.begin() + 4);
    if (signature != "DIRC") throw std::runtime_error("Invalid index signature");

    uint32_t version, count;
    std::memcpy(&version, raw.data() + 4, 4);
    version = ntohl(version);
    std::memcpy(&count, raw.data() + 8, 4);
    count = ntohl(count);

    GitIndex index(version);
    size_t idx = 12;

    for (uint32_t i = 0; i < count; ++i) {
        GitIndexEntry entry;
        auto read_u32 = [&](size_t offset) {
            uint32_t val;
            std::memcpy(&val, raw.data() + idx + offset, 4);
            return ntohl(val); // Swap to Little-Endian
        };

        entry.ctime_s = read_u32(0);
        entry.ctime_ns = read_u32(4);
        entry.mtime_s = read_u32(8);
        entry.mtime_ns = read_u32(12);
        entry.dev = read_u32(16);
        entry.ino = read_u32(20);
        
        uint32_t mode_word = read_u32(24);
        uint16_t mode = mode_word & 0xFFFF;
        entry.mode_type = mode >> 12;
        entry.mode_perms = mode & 0x01FF;

        entry.uid = read_u32(28);
        entry.gid = read_u32(32);
        entry.fsize = read_u32(36);

        std::stringstream ss;
        for (int j = 0; j < 20; ++j) ss << std::hex << std::setw(2) << std::setfill('0') << (int)raw[idx + 40 + j];
        entry.sha = ss.str();

        uint16_t flags;
        std::memcpy(&flags, raw.data() + idx + 60, 2);
        flags = ntohs(flags);

        entry.flag_assume_valid = (flags & 0x8000) != 0;
        entry.flag_stage = (flags & 0x3000) >> 12;
        uint16_t name_length = flags & 0x0FFF;

        idx += 62;
        if (name_length < 0xFFF) {
            entry.name = std::string(raw.begin() + idx, raw.begin() + idx + name_length);
            idx += name_length + 1; 
        } else {
            size_t null_idx = idx;
            while (null_idx < size && raw[null_idx] != '\0') null_idx++;
            entry.name = std::string(raw.begin() + idx, raw.begin() + null_idx);
            idx = null_idx + 1;
        }

        // Advance to the next 8-byte aligned boundry
        size_t content_offset = idx - 12;
        idx = ((content_offset + 7) & ~7) + 12;
        index.entries.push_back(entry);
    }
    return index;
}

/**
 * @brief Serializes the state of the GitIndex array back into binary.
 * Converts host Little-Endian architectures into Git's required Big-Endian format.
 */
void index_write(const GitRepository& repo, const GitIndex& index) {
    fs::path index_file = repo_file(repo, "index");
    std::ofstream f(index_file, std::ios::binary);

    f.write("DIRC", 4);
    uint32_t v = htonl(index.version);
    f.write(reinterpret_cast<char*>(&v), 4);
    uint32_t count = htonl(index.entries.size());
    f.write(reinterpret_cast<char*>(&count), 4);

    size_t entries_size = 0; 
    
    for (const auto& e : index.entries) {
        auto write_u32 = [&](uint32_t val) {
            uint32_t net_val = htonl(val); // Swap to Big-Endian
            f.write(reinterpret_cast<char*>(&net_val), 4);
        };

        write_u32(e.ctime_s); write_u32(e.ctime_ns);
        write_u32(e.mtime_s); write_u32(e.mtime_ns);
        write_u32(e.dev); write_u32(e.ino);

        uint32_t mode = (e.mode_type << 12) | e.mode_perms;
        write_u32(mode);

        write_u32(e.uid); write_u32(e.gid); write_u32(e.fsize);

        std::vector<unsigned char> sha_bytes = hex_to_bytes(e.sha);
        f.write(reinterpret_cast<char*>(sha_bytes.data()), 20);

        uint16_t flag_assume_valid = e.flag_assume_valid ? 0x8000 : 0;
        uint16_t name_length = e.name.size() >= 0xFFF ? 0xFFF : e.name.size();
        uint16_t flags = htons(flag_assume_valid | (e.flag_stage << 12) | name_length);
        f.write(reinterpret_cast<char*>(&flags), 2);

        f.write(e.name.c_str(), e.name.size());
        f.put('\0');

        entries_size += 62 + e.name.size() + 1;

        // Pad to a multiple of 8 relative to the entries block to ensure memory alignment
        if (entries_size % 8 != 0) {
            size_t pad = 8 - (entries_size % 8);
            for (size_t i = 0; i < pad; ++i) f.put('\0');
            entries_size += pad;
        }
    }
}

// -------------------------------------------------------------------------
// IGNORE SYSTEM
// -------------------------------------------------------------------------
std::optional<IgnoreRule> gitignore_parse1(std::string raw) {
    raw.erase(raw.begin(), std::find_if(raw.begin(), raw.end(), [](unsigned char ch) { return !std::isspace(ch); }));
    raw.erase(std::find_if(raw.rbegin(), raw.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), raw.end());

    if (raw.empty() || raw[0] == '#') return std::nullopt;
    if (raw[0] == '!') return IgnoreRule{raw.substr(1), false};
    if (raw[0] == '\\') return IgnoreRule{raw.substr(1), true};
    return IgnoreRule{raw, true};
}

RuleSet gitignore_parse(const std::vector<std::string>& lines) {
    RuleSet ret;
    for (const auto& line : lines) {
        if (auto parsed = gitignore_parse1(line)) ret.push_back(*parsed);
    }
    return ret;
}

GitIgnore gitignore_read(const GitRepository& repo) {
    GitIgnore ret;
    auto read_file_lines = [](const fs::path& path) {
        std::vector<std::string> lines;
        std::ifstream file(path);
        std::string line;
        while (std::getline(file, line)) lines.push_back(line);
        return lines;
    };

    fs::path repo_file = repo.gitdir / "info" / "exclude";
    if (fs::exists(repo_file)) ret.absolute.push_back(gitignore_parse(read_file_lines(repo_file)));

    std::string config_home;
    if (const char* env_xdg = std::getenv("XDG_CONFIG_HOME")) config_home = env_xdg;
    else if (const char* env_home = std::getenv("HOME")) config_home = std::string(env_home) + "/.config";
    
    if (!config_home.empty()) {
        fs::path global_file = fs::path(config_home) / "git" / "ignore";
        if (fs::exists(global_file)) ret.absolute.push_back(gitignore_parse(read_file_lines(global_file)));
    }

    GitIndex index = index_read(repo);
    std::string suffix = "/.gitignore";

    // Wyag only applies ignore rules from .gitignore files that are staged in the index
    for (const auto& entry : index.entries) {
        // C++17 compliant ends_with replacement
        if (entry.name == ".gitignore" || 
           (entry.name.size() >= suffix.size() && entry.name.compare(entry.name.size() - suffix.size(), suffix.size(), suffix) == 0)) {
            std::string dir_name = fs::path(entry.name).parent_path().string();
            if (dir_name.empty()) dir_name = ".";
            auto obj = object_read(repo, entry.sha);
            if (obj && obj->format() == "blob") {
                GitBlob* blob = dynamic_cast<GitBlob*>(obj.get());
                std::istringstream stream(blob->blobdata);
                std::vector<std::string> lines;
                std::string line;
                while (std::getline(stream, line)) lines.push_back(line);
                ret.scoped[dir_name] = gitignore_parse(lines);
            }
        }
    }
    return ret;
}

std::optional<bool> check_ignore1(const RuleSet& rules, const std::string& path) {
    std::optional<bool> result = std::nullopt;
    for (const auto& rule : rules) {
        if (fnmatch(rule.pattern.c_str(), path.c_str(), 0) == 0) result = rule.exclude;
    }
    return result;
}

std::optional<bool> check_ignore_scoped(const std::map<std::string, RuleSet>& rules, const std::string& path) {
    fs::path parent = fs::path(path).parent_path();
    while (true) {
        std::string parent_str = parent.empty() ? "." : parent.string();
        if (rules.count(parent_str)) {
            if (auto result = check_ignore1(rules.at(parent_str), path)) return result;
        }
        if (parent.empty()) break;
        parent = parent.parent_path();
    }
    return std::nullopt;
}

bool check_ignore_absolute(const std::vector<RuleSet>& rules, const std::string& path) {
    for (const auto& ruleset : rules) {
        if (auto result = check_ignore1(ruleset, path)) return *result;
    }
    return false;
}

bool check_ignore(const GitIgnore& rules, const std::string& path) {
    if (fs::path(path).is_absolute()) throw std::runtime_error("Path must be relative to repo root");
    if (auto result = check_ignore_scoped(rules.scoped, path)) return *result;
    return check_ignore_absolute(rules.absolute, path);
}

// -------------------------------------------------------------------------
// REFERENCES & TAGS
// -------------------------------------------------------------------------
std::string ref_resolve(const GitRepository& repo, const std::string& ref) {
    fs::path path = repo_file(repo, ref);
    if (!fs::exists(path) || !fs::is_regular_file(path)) return "";
    std::ifstream file(path);
    std::string data;
    std::getline(file, data);
    
    // If the file starts with ref:, it's an indirect reference (like HEAD -> refs/heads/master)
    if (data.rfind("ref: ", 0) == 0) return ref_resolve(repo, data.substr(5));
    return data;
}

std::map<std::string, std::string> ref_list(const GitRepository& repo, fs::path path) {
    if (path.empty()) path = repo_dir(repo, "refs");
    std::map<std::string, std::string> ret; 
    if (!fs::exists(path)) return ret;
    for (const auto& entry : fs::directory_iterator(path)) {
        std::string name = entry.path().filename().string();
        if (entry.is_directory()) {
            auto sub_refs = ref_list(repo, entry.path());
            for (const auto& [k, v] : sub_refs) ret[name + "/" + k] = v;
        } else {
            ret[name] = ref_resolve(repo, fs::relative(entry.path(), repo.gitdir).string());
        }
    }
    return ret;
}

void ref_create(const GitRepository& repo, const std::string& ref_name, const std::string& sha) {
    fs::path path = repo_file(repo, fs::path("refs") / ref_name, true);
    std::ofstream fp(path);
    fp << sha << "\n";
}

void tag_create(const GitRepository& repo, const std::string& name, const std::string& ref, bool create_tag_object) {
    std::string sha = object_find(repo, ref);
    if (create_tag_object) {
        // Annotated Tag: Create a full object
        GitTag tag;
        tag.kvlm.fields["object"].push_back(sha);
        tag.kvlm.fields["type"].push_back("commit"); 
        tag.kvlm.fields["tag"].push_back(name);
        tag.kvlm.fields["tagger"].push_back("Wyag <wyag@example.com>");
        tag.kvlm.message = "A tag generated by wyag!";
        std::string tag_sha = object_write(tag, &repo);
        ref_create(repo, "tags/" + name, tag_sha);
    } else {
        // Lightweight Tag: Just create a reference
        ref_create(repo, "tags/" + name, sha);
    }
}

// -------------------------------------------------------------------------
// OBJECT READING, WRITING & RESOLUTION
// -------------------------------------------------------------------------
std::unique_ptr<GitObject> object_read(const GitRepository& repo, const std::string& sha) {
    fs::path path = repo_file(repo, fs::path("objects") / sha.substr(0, 2) / sha.substr(2));
    if (!fs::exists(path)) return nullptr;
    std::ifstream file(path, std::ios::binary);
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string raw = zlib_decompress(buffer.str());
    size_t space_idx = raw.find(' ');
    std::string fmt = raw.substr(0, space_idx);
    size_t null_idx = raw.find('\0', space_idx);
    int size = std::stoi(raw.substr(space_idx + 1, null_idx - space_idx - 1));
    if (size != raw.size() - null_idx - 1) throw std::runtime_error("Malformed object " + sha);
    std::string data = raw.substr(null_idx + 1);
    
    if (fmt == "blob") return std::make_unique<GitBlob>(data);
    else if (fmt == "commit") return std::make_unique<GitCommit>(data);
    else if (fmt == "tree") return std::make_unique<GitTree>(data);
    else if (fmt == "tag") return std::make_unique<GitTag>(data);
    else throw std::runtime_error("Unknown type " + fmt); 
}

std::string object_write(GitObject& obj, const GitRepository* repo) {
    std::string data = obj.serialize();
    std::string result = obj.format() + " " + std::to_string(data.size()) + '\0' + data;
    std::string sha = sha1_hex(result);
    if (repo) {
        fs::path path = repo_file(*repo, fs::path("objects") / sha.substr(0, 2) / sha.substr(2), true);
        if (!fs::exists(path)) {
            std::ofstream file(path, std::ios::binary);
            file << zlib_compress(result);
        }
    }
    return sha;
}

std::vector<std::string> object_resolve(const GitRepository& repo, std::string name) {
    std::vector<std::string> candidates;
    if (name.empty()) return candidates;
    if (name == "HEAD") {
        std::string resolved = ref_resolve(repo, "HEAD");
        if (!resolved.empty()) candidates.push_back(resolved);
        return candidates;
    }
    // Partial hash resolution
    std::regex hashRE("^[0-9A-Fa-f]{4,40}$");
    if (std::regex_match(name, hashRE)) {
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        std::string prefix = name.substr(0, 2);
        fs::path path = repo_dir(repo, fs::path("objects") / prefix, false);
        if (fs::exists(path)) {
            std::string rem = name.substr(2);
            for (const auto& entry : fs::directory_iterator(path)) {
                std::string f = entry.path().filename().string();
                if (f.rfind(rem, 0) == 0) candidates.push_back(prefix + f);
            }
        }
    }
    std::string as_tag = ref_resolve(repo, "refs/tags/" + name);
    if (!as_tag.empty()) candidates.push_back(as_tag);
    std::string as_branch = ref_resolve(repo, "refs/heads/" + name);
    if (!as_branch.empty()) candidates.push_back(as_branch);
    return candidates;
}

std::string object_find(const GitRepository& repo, const std::string& name, const std::string& fmt, bool follow) {
    auto sha_list = object_resolve(repo, name);
    if (sha_list.empty()) throw std::runtime_error("No such reference " + name);
    if (sha_list.size() > 1) throw std::runtime_error("Ambiguous reference " + name);
    std::string sha = sha_list[0];
    if (fmt.empty()) return sha;
    
    // Follow object references (e.g. Tag -> Commit -> Tree)
    while (true) {
        auto obj = object_read(repo, sha);
        if (!obj) return "";
        if (obj->format() == fmt) return sha;
        if (!follow) return "";
        if (obj->format() == "tag") sha = dynamic_cast<GitTag*>(obj.get())->kvlm.fields.at("object")[0];
        else if (obj->format() == "commit" && fmt == "tree") sha = dynamic_cast<GitCommit*>(obj.get())->kvlm.fields.at("tree")[0];
        else return "";
    }
}

std::string object_hash(std::istream& fd, const std::string& fmt, const GitRepository* repo) {
    std::stringstream buffer;
    buffer << fd.rdbuf();
    std::string data = buffer.str();
    if (fmt == "blob") { GitBlob obj(data); return object_write(obj, repo); }
    else if (fmt == "commit") { GitCommit obj(data); return object_write(obj, repo); }
    else if (fmt == "tree") { GitTree obj(data); return object_write(obj, repo); }
    else if (fmt == "tag") { GitTag obj(data); return object_write(obj, repo); }
    throw std::runtime_error("Unknown type " + fmt);
}

// -------------------------------------------------------------------------
// STATUS & COMMIT HELPERS
// -------------------------------------------------------------------------

std::string branch_get_active(const GitRepository& repo) {
    fs::path head_file = repo_file(repo, "HEAD");
    std::ifstream file(head_file);
    std::string head;
    std::getline(file, head);
    if (head.find("ref: refs/heads/") == 0) return head.substr(16);
    return "";
}

/**
 * @brief Performs a Depth-First Search (DFS) to flatten a recursive GitTree 
 * into a map of full file paths -> blob SHAs.
 */
std::map<std::string, std::string> tree_to_dict(const GitRepository& repo, const std::string& ref, const std::string& prefix) {
    std::map<std::string, std::string> ret;
    std::string tree_sha;
    try { tree_sha = object_find(repo, ref, "tree"); } catch (...) { return ret; }

    auto obj = object_read(repo, tree_sha);
    GitTree* tree = dynamic_cast<GitTree*>(obj.get());

    for (const auto& leaf : tree->items) {
        std::string full_path = prefix.empty() ? leaf.path : prefix + "/" + leaf.path;
        bool is_subtree = (leaf.mode.substr(0, 2) == "04"); // 040000 denotes a directory
        if (is_subtree) {
            auto sub_dict = tree_to_dict(repo, leaf.sha, full_path);
            ret.insert(sub_dict.begin(), sub_dict.end());
        } else {
            ret[full_path] = leaf.sha;
        }
    }
    return ret;
}

void cmd_status_branch(const GitRepository& repo) {
    std::string branch = branch_get_active(repo);
    if (!branch.empty()) std::cout << "On branch " << branch << "\n";
    else std::cout << "HEAD detached at " << object_find(repo, "HEAD") << "\n";
}

void cmd_status_head_index(const GitRepository& repo, const GitIndex& index) {
    std::cout << "Changes to be committed:\n";
    auto head = tree_to_dict(repo, "HEAD");
    for (const auto& entry : index.entries) {
        if (head.count(entry.name)) {
            if (head[entry.name] != entry.sha) std::cout << "  modified: " << entry.name << "\n";
            head.erase(entry.name); // Checked, remove from map
        } else {
            std::cout << "  added:    " << entry.name << "\n";
        }
    }
    // Any files left in the HEAD map that weren't in the index must have been deleted
    for (const auto& [name, sha] : head) std::cout << "  deleted:  " << name << "\n";
}

void cmd_status_index_worktree(const GitRepository& repo, const GitIndex& index) {
    std::cout << "Changes not staged for commit:\n";
    GitIgnore ignore = gitignore_read(repo);
    std::vector<std::string> all_files;

    for (auto it = fs::recursive_directory_iterator(repo.worktree); it != fs::recursive_directory_iterator(); ++it) {
        if (it->is_directory() && it->path() == repo.gitdir) {
            it.disable_recursion_pending(); // Skip .git directory traversal
            continue;
        }
        if (it->is_regular_file()) all_files.push_back(fs::relative(it->path(), repo.worktree).string());
    }

    for (const auto& entry : index.entries) {
        fs::path full_path = repo.worktree / entry.name;
        if (!fs::exists(full_path)) {
            std::cout << "  deleted:  " << entry.name << "\n";
        } else {
            struct stat st;
            stat(full_path.c_str(), &st);

            // High-precision nanosecond time comparison
            uint64_t ctime_ns = entry.ctime_s * 1000000000ULL + entry.ctime_ns;
            uint64_t mtime_ns = entry.mtime_s * 1000000000ULL + entry.mtime_ns;
            uint64_t stat_ctime_ns = st.st_ctimespec.tv_sec * 1000000000ULL + st.st_ctimespec.tv_nsec;
            uint64_t stat_mtime_ns = st.st_mtimespec.tv_sec * 1000000000ULL + st.st_mtimespec.tv_nsec;

            if (stat_ctime_ns != ctime_ns || stat_mtime_ns != mtime_ns) {
                std::ifstream fd(full_path, std::ios::binary);
                std::string new_sha = object_hash(fd, "blob", nullptr);
                if (entry.sha != new_sha) std::cout << "  modified: " << entry.name << "\n";
            }
        }
        auto it = std::find(all_files.begin(), all_files.end(), entry.name);
        if (it != all_files.end()) all_files.erase(it);
    }

    std::cout << "\nUntracked files:\n";
    for (const auto& f : all_files) {
        if (!check_ignore(ignore, f)) std::cout << "  " << f << "\n";
    }
}

// -------------------------------------------------------------------------
// CLI COMMAND ROUTING
// -------------------------------------------------------------------------

void cmd_init(const std::vector<std::string>& args) {
    fs::path repo_path_str = args.empty() ? "." : args[0];
    try {
        repo_create(repo_path_str);
        std::cout << "Initialized empty Git repository in " << fs::absolute(repo_path_str / ".git") << "\n";
    } catch (const std::exception& e) { std::cerr << "Fatal: " << e.what() << "\n"; }
}

void cmd_hash_object(const std::vector<std::string>& args) {
    bool write = false; std::string type = "blob"; std::string file_path = "";
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "-w") write = true;
        else if (args[i] == "-t" && i + 1 < args.size()) type = args[++i];
        else file_path = args[i];
    }
    if (file_path.empty()) throw std::runtime_error("Usage: wyag hash-object [-w] [-t TYPE] FILE");
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) throw std::runtime_error("Could not open file " + file_path);
    std::unique_ptr<GitRepository> repo = write ? repo_find() : nullptr;
    std::cout << object_hash(file, type, repo.get()) << "\n";
}

void cmd_cat_file(const std::vector<std::string>& args) {
    if (args.size() < 2) throw std::runtime_error("Usage: wyag cat-file TYPE OBJECT");
    auto repo = repo_find();
    auto obj = object_read(*repo, object_find(*repo, args[1], args[0]));
    std::cout << obj->serialize(); 
}

void log_graphviz(const GitRepository& repo, const std::string& sha, std::set<std::string>& seen) {
    if (seen.count(sha)) return;
    seen.insert(sha);
    auto obj = object_read(repo, sha);
    if (!obj || obj->format() != "commit") return;
    GitCommit* commit = dynamic_cast<GitCommit*>(obj.get());
    std::string msg = commit->kvlm.message.substr(0, commit->kvlm.message.find('\n'));
    std::string escaped_msg;
    for (char c : msg) escaped_msg += (c == '"') ? "\\\"" : std::string(1, c);
    std::cout << "  c_" << sha << " [label=\"" << sha.substr(0, 7) << ": " << escaped_msg << "\"]\n";
    if (commit->kvlm.fields.count("parent")) {
        for (const std::string& p : commit->kvlm.fields.at("parent")) {
            std::cout << "  c_" << sha << " -> c_" << p << ";\n";
            log_graphviz(repo, p, seen);
        }
    }
}

void cmd_log(const std::vector<std::string>& args) {
    std::string commit_sha = args.empty() ? "HEAD" : args[0]; 
    auto repo = repo_find();
    
    // Resolve HEAD or branch name to the actual SHA-1 hash
    commit_sha = object_find(*repo, commit_sha, "commit");
    
    std::cout << "digraph wyaglog {\n  node[shape=rect]\n";
    std::set<std::string> seen;
    log_graphviz(*repo, commit_sha, seen);
    std::cout << "}\n";
}

void ls_tree(const GitRepository& repo, const std::string& sha, const std::string& prefix = "") {
    auto obj = object_read(repo, sha);
    if (!obj || obj->format() != "tree") throw std::runtime_error("Not a tree: " + sha);
    GitTree* tree = dynamic_cast<GitTree*>(obj.get());
    for (const auto& item : tree->items) {
        std::string type = (item.mode[0] == '4') ? "tree" : "blob";
        std::string full_path = prefix.empty() ? item.path : prefix + "/" + item.path;
        std::cout << std::setfill('0') << std::setw(6) << item.mode << " " << type << " " << item.sha << "\t" << full_path << "\n";
    }
}
void cmd_ls_tree(const std::vector<std::string>& args) {
    if (args.empty()) throw std::runtime_error("Usage: wyag ls-tree <TREE_SHA>");
    auto repo = repo_find(); ls_tree(*repo, args[0]); 
}

void tree_checkout(const GitRepository& repo, GitTree* tree, const fs::path& path) {
    for (const auto& item : tree->items) {
        auto obj = object_read(repo, item.sha);
        fs::path dest = path / item.path;
        if (obj->format() == "tree") {
            fs::create_directories(dest);
            tree_checkout(repo, dynamic_cast<GitTree*>(obj.get()), dest);
        } else if (obj->format() == "blob") {
            std::ofstream file(dest, std::ios::binary);
            file << dynamic_cast<GitBlob*>(obj.get())->blobdata;
        }
    }
}

void cmd_checkout(const std::vector<std::string>& args) {
    if (args.size() < 2) throw std::runtime_error("Usage: wyag checkout <COMMIT_SHA> <EMPTY_DIR>");
    auto repo = repo_find();
    fs::path checkout_path = args[1];
    
    // Resolve "HEAD", a tag name, or a branch name to its actual SHA-1 hash FIRST
    std::string obj_sha = object_find(*repo, args[0], ""); 
    auto obj = object_read(*repo, obj_sha);
    
    if (obj && obj->format() == "commit") {
        std::string tree_sha = dynamic_cast<GitCommit*>(obj.get())->kvlm.fields.at("tree")[0];
        obj = object_read(*repo, tree_sha);
    }
    
    if (!obj || obj->format() != "tree") throw std::runtime_error("Could not find tree.");
    
    if (fs::exists(checkout_path)) {
        if (!fs::is_directory(checkout_path) || !fs::is_empty(checkout_path)) throw std::runtime_error("Dir must be empty!");
    } else {
        fs::create_directories(checkout_path);
    }
    
    tree_checkout(*repo, dynamic_cast<GitTree*>(obj.get()), checkout_path);
    std::cout << "Checked out to " << checkout_path << "\n";
}

void show_ref(const GitRepository& repo, const std::map<std::string, std::string>& refs, bool with_hash, std::string prefix) {
    if (!prefix.empty()) prefix += "/";
    for (const auto& [k, v] : refs) {
        if (with_hash) std::cout << v << " " << prefix << k << "\n";
        else std::cout << prefix << k << "\n";
    }
}
void cmd_show_ref(const std::vector<std::string>& args) {
    auto repo = repo_find();
    show_ref(*repo, ref_list(*repo), true, "refs");
}

void cmd_tag(const std::vector<std::string>& args) {
    auto repo = repo_find();
    bool create_tag_object = false; std::string name = ""; std::string object = "HEAD";
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "-a") create_tag_object = true;
        else if (name.empty()) name = args[i];
        else object = args[i];
    }
    if (!name.empty()) tag_create(*repo, name, object, create_tag_object);
    else show_ref(*repo, ref_list(*repo, repo_dir(*repo, fs::path("refs") / "tags")), false, "");
}

void cmd_rev_parse(const std::vector<std::string>& args) {
    std::string type = ""; std::string name = "";
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--wyag-type" && i + 1 < args.size()) type = args[++i];
        else name = args[i];
    }
    if (name.empty()) throw std::runtime_error("Usage: wyag rev-parse [--wyag-type TYPE] NAME");
    auto repo = repo_find();
    std::cout << object_find(*repo, name, type, true) << "\n";
}

void cmd_ls_files(const std::vector<std::string>& args) {
    bool verbose = false;
    for (const auto& arg : args) if (arg == "--verbose") verbose = true;
    auto repo = repo_find();
    GitIndex index = index_read(*repo);
    if (verbose) std::cout << "Index file format v" << index.version << ", containing " << index.entries.size() << " entries.\n";
    for (const auto& e : index.entries) {
        std::cout << e.name << "\n";
        if (verbose) {
            std::string etype = (e.mode_type == 0b1000) ? "regular file" : (e.mode_type == 0b1010) ? "symlink" : "git link";
            std::cout << "  " << etype << " with perms: " << std::oct << e.mode_perms << std::dec << "\n  on blob: " << e.sha << "\n";
            std::time_t ctime = e.ctime_s, mtime = e.mtime_s;
            char cb[64], mb[64];
            std::strftime(cb, sizeof(cb), "%Y-%m-%d %H:%M:%S", std::localtime(&ctime));
            std::strftime(mb, sizeof(mb), "%Y-%m-%d %H:%M:%S", std::localtime(&mtime));
            std::cout << "  created: " << cb << "." << e.ctime_ns << ", modified: " << mb << "." << e.mtime_ns << "\n";
            std::cout << "  device: " << e.dev << ", inode: " << e.ino << "\n";
            struct passwd* pw = getpwuid(e.uid); struct group* gr = getgrgid(e.gid);
            std::cout << "  user: " << (pw ? pw->pw_name : "unknown") << " (" << e.uid << ")  group: " << (gr ? gr->gr_name : "unknown") << " (" << e.gid << ")\n";
            std::cout << "  flags: stage=" << e.flag_stage << " assume_valid=" << e.flag_assume_valid << "\n";
        }
    }
}

void cmd_check_ignore(const std::vector<std::string>& args) {
    if (args.empty()) throw std::runtime_error("Usage: wyag check-ignore <path>...");
    auto repo = repo_find();
    GitIgnore rules = gitignore_read(*repo);
    for (const auto& path : args) {
        if (check_ignore(rules, path)) std::cout << path << "\n";
    }
}

void cmd_status(const std::vector<std::string>& args) {
    auto repo = repo_find();
    GitIndex index = index_read(*repo);
    cmd_status_branch(*repo);
    cmd_status_head_index(*repo, index);
    std::cout << "\n";
    cmd_status_index_worktree(*repo, index);
}

// -------------------------------------------------------------------------
// STAGE & COMMIT ROUTING
// -------------------------------------------------------------------------

/**
 * @brief Removes tracked paths from the index and optionally the physical filesystem.
 */
void rm(const GitRepository& repo, const std::vector<std::string>& paths, bool delete_file, bool skip_missing) {
    GitIndex index = index_read(repo);
    
    // Normalize the worktree path to prevent /./ directory resolution errors
    fs::path worktree = fs::weakly_canonical(fs::absolute(repo.worktree));

    std::set<std::string> abspaths;
    for (const auto& path : paths) {
        fs::path p = fs::weakly_canonical(fs::absolute(path));
        if (p.string().find(worktree.string()) != 0) {
            throw std::runtime_error("Cannot remove paths outside of worktree: " + path);
        }
        abspaths.insert(p.string());
    }

    std::vector<GitIndexEntry> kept_entries;
    std::vector<std::string> remove_paths;

    // Filter index for paths that are targeted for removal
    for (const auto& e : index.entries) {
        std::string full_path = (worktree / e.name).string();
        if (abspaths.count(full_path)) {
            remove_paths.push_back(full_path);
            abspaths.erase(full_path);
        } else {
            kept_entries.push_back(e);
        }
    }

    if (!abspaths.empty() && !skip_missing) throw std::runtime_error("Cannot remove paths not in the index");

    if (delete_file) {
        for (const auto& path : remove_paths) fs::remove(path);
    }

    index.entries = kept_entries;
    index_write(repo, index);
}

void cmd_rm(const std::vector<std::string>& args) {
    if (args.empty()) throw std::runtime_error("Usage: wyag rm <path>...");
    auto repo = repo_find();
    rm(*repo, args);
}

/**
 * @brief Stages paths to the index. Hashes contents into blobs and updates index metadata.
 */
void add(const GitRepository& repo, const std::vector<std::string>& paths) {
    // Clear out previous versions of the file if they already exist in the index
    rm(repo, paths, false, true);

    fs::path worktree = fs::weakly_canonical(fs::absolute(repo.worktree));
    std::vector<std::pair<std::string, std::string>> clean_paths;

    for (const auto& path : paths) {
        fs::path p = fs::weakly_canonical(fs::absolute(path));
        if (!(p.string().find(worktree.string()) == 0 && fs::is_regular_file(p))) {
            throw std::runtime_error("Not a file, or outside the worktree: " + path);
        }
        clean_paths.push_back({p.string(), fs::relative(p, worktree).string()});
    }

    GitIndex index = index_read(repo);

    for (const auto& [abspath, relpath] : clean_paths) {
        // Hash and store the object
        std::ifstream fd(abspath, std::ios::binary);
        std::string sha = object_hash(fd, "blob", &repo);

        struct stat st;
        stat(abspath.c_str(), &st);

        GitIndexEntry entry;
        entry.ctime_s = st.st_ctimespec.tv_sec;
        entry.ctime_ns = st.st_ctimespec.tv_nsec;
        entry.mtime_s = st.st_mtimespec.tv_sec;
        entry.mtime_ns = st.st_mtimespec.tv_nsec;
        entry.dev = st.st_dev;
        entry.ino = st.st_ino;
        entry.mode_type = 0b1000; 
        entry.mode_perms = 0644; 
        entry.uid = st.st_uid;
        entry.gid = st.st_gid;
        entry.fsize = st.st_size;
        entry.sha = sha;
        entry.flag_assume_valid = false;
        entry.flag_stage = 0;
        entry.name = relpath;

        index.entries.push_back(entry);
    }

    index_write(repo, index);
}

void cmd_add(const std::vector<std::string>& args) {
    if (args.empty()) throw std::runtime_error("Usage: wyag add <path>...");
    auto repo = repo_find();
    add(*repo, args);
}

/**
 * @brief Converts the flat file structure of the GitIndex into a recursive Merkle Tree.
 * Leverages std::variant to store either single files (GitIndexEntry) or nested sub-trees.
 */
std::string tree_from_index(const GitRepository& repo, const GitIndex& index) {
    using NodeVariant = std::variant<GitIndexEntry, std::pair<std::string, std::string>>;
    std::map<std::string, std::vector<NodeVariant>> contents;
    contents[""] = {}; 

    // Step 1: Map all nested paths
    for (const auto& entry : index.entries) {
        fs::path p(entry.name);
        std::string dirname = p.parent_path().string();
        
        std::string key = dirname;
        while (!key.empty()) {
            if (contents.find(key) == contents.end()) contents[key] = {};
            key = fs::path(key).parent_path().string();
        }
        contents[dirname].push_back(entry);
    }

    // Step 2: Sort paths descending by length to process deepest branches first (Bottom-Up execution)
    std::vector<std::string> sorted_paths;
    for (const auto& pair : contents) sorted_paths.push_back(pair.first);
    std::sort(sorted_paths.begin(), sorted_paths.end(), [](const std::string& a, const std::string& b) {
        return a.length() > b.length();
    });

    std::string root_sha;

    // Step 3: Traverse paths and construct recursive Tree structures
    for (const auto& path : sorted_paths) {
        GitTree tree;
        for (const auto& entry_var : contents[path]) {
            if (std::holds_alternative<GitIndexEntry>(entry_var)) {
                // Node is a File/Blob
                auto entry = std::get<GitIndexEntry>(entry_var);
                char mode_buf[16];
                snprintf(mode_buf, sizeof(mode_buf), "%02o%04o", entry.mode_type, entry.mode_perms);
                tree.items.push_back({mode_buf, fs::path(entry.name).filename().string(), entry.sha});
            } else {
                // Node is a Sub-Tree computed in a previous iteration
                auto subtree = std::get<std::pair<std::string, std::string>>(entry_var);
                tree.items.push_back({"040000", subtree.first, subtree.second});
            }
        }
        
        root_sha = object_write(tree, &repo);
        
        // Push computed tree up to its parent directory node
        if (!path.empty()) {
            std::string parent = fs::path(path).parent_path().string();
            std::string base = fs::path(path).filename().string();
            contents[parent].push_back(std::make_pair(base, root_sha));
        }
    }
    return root_sha;
}

std::string gitconfig_user_get() {
    const char* user = std::getenv("USER");
    std::string name = user ? user : "Wyag User";
    return name + " <" + name + "@example.com>";
}

/**
 * @brief Constructs a GitCommit object pointing to the newly generated Tree.
 */
std::string commit_create(const GitRepository& repo, const std::string& tree, const std::string& parent, const std::string& author, const std::string& message) {
    GitCommit commit;
    commit.kvlm.fields["tree"].push_back(tree);
    if (!parent.empty()) commit.kvlm.fields["parent"].push_back(parent);

    // Capture standard ISO 8601 formatting for timezone metadata
    auto now = std::chrono::system_clock::now();
    std::time_t time = std::chrono::system_clock::to_time_t(now);
    char tz_buf[16];
    std::strftime(tz_buf, sizeof(tz_buf), "%z", std::localtime(&time));

    std::string full_author = author + " " + std::to_string(time) + " " + tz_buf;
    
    commit.kvlm.fields["author"].push_back(full_author);
    commit.kvlm.fields["committer"].push_back(full_author);
    
    // Trim trailing whitespace from the message
    std::string clean_msg = message;
    clean_msg.erase(clean_msg.find_last_not_of(" \n\r\t") + 1);
    commit.kvlm.message = clean_msg + "\n";

    return object_write(commit, &repo);
}

void cmd_commit(const std::vector<std::string>& args) {
    std::string message = "";
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "-m" && i + 1 < args.size()) message = args[++i];
    }
    if (message.empty()) throw std::runtime_error("Usage: wyag commit -m <message>");

    auto repo = repo_find();
    GitIndex index = index_read(*repo);
    std::string tree = tree_from_index(*repo, index);
    
    std::string parent;
    try { parent = object_find(*repo, "HEAD"); } catch (...) { parent = ""; }

    std::string commit_sha = commit_create(*repo, tree, parent, gitconfig_user_get(), message);

    // Update branch references (Move HEAD to the newly created commit)
    std::string active_branch = branch_get_active(*repo);
    if (!active_branch.empty()) {
        fs::path ref_path = repo_file(*repo, fs::path("refs/heads") / active_branch, true);
        std::ofstream fd(ref_path);
        fd << commit_sha << "\n";
    } else {
        fs::path head_path = repo_file(*repo, "HEAD");
        std::ofstream fd(head_path);
        fd << commit_sha << "\n";
    }
    
    std::cout << "[" << (active_branch.empty() ? "detached HEAD" : active_branch) << " " << commit_sha.substr(0, 7) << "] " << message << "\n";
}