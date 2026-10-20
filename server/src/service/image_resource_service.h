#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <stdexcept>
#include <vector>
#include <nlohmann/json_fwd.hpp>

namespace dice::image_resources {
struct Conflict : std::runtime_error { using std::runtime_error::runtime_error; };

struct Document {
    std::string id, label, type, content;
    // text / json / yaml. Code files are never rewritten as plain text.
    std::string format = "text";
    bool writable = true;
    std::string owner;
    std::filesystem::path baseDirectory;
};
struct Change { Document before; std::string after; };
using Scan = std::function<std::vector<Document>()>;
// Compare-before-write; a changed document must fail, never overwrite a newer edit.
using Save = std::function<void(const Change&)>;
using Mapper = std::function<std::optional<std::string>(const std::string&)>;
struct ImageReference { std::size_t begin = 0, end = 0; std::string value; bool cq = false; };
std::vector<ImageReference> references(const std::string& text);
std::string rewriteText(const std::string& text, const Mapper& mapper);
std::string rewriteDocument(const Document& document, const Mapper& mapper);
std::vector<std::string> documentReferences(const Document& document);
std::string utf8(const std::filesystem::path& path);
std::filesystem::path fromUtf8(const std::string& value);
std::string escapeCode(const std::string& value);
std::string unescapeCode(std::string value);
std::string imageCode(const std::string& relativePath);
std::string mimeFor(std::string_view bytes, const std::string& filename);
bool imageExtension(const std::filesystem::path& path);

class Service {
public:
    explicit Service(std::filesystem::path dataRoot = "data");
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;
    void setDocuments(Scan scan, Save save, std::function<void()> changed = {});
    // Listing reads metadata only. It never reads image contents or emits image URLs.
    nlohmann::json list(std::string directory, int page = 1, int size = 20,
        std::string query = {}, std::string sort = "name", bool descending = false, bool trash = false);
    nlohmann::json upload(const std::string& directory, const std::string& filename,
        std::string_view bytes, bool overwrite = false);
    // A known owner imports all its images under this directory. No global hash pool.
    nlohmann::json importFile(const std::filesystem::path& sourceRoot, const std::filesystem::path& source,
        const std::string& destination, const std::string& oldReference = {});
    nlohmann::json importOwner(const std::filesystem::path& sourceRoot, const std::filesystem::path& directory,
        const std::string& category, const std::string& owner);
    void mkdir(const std::string& directory);
    nlohmann::json operate(const std::string& action, const std::vector<std::string>& paths,
        const std::string& target = {}, bool overwrite = false);
    nlohmann::json usages(const std::string& path);
    nlohmann::json scanReferences();
    std::optional<std::filesystem::path> resolve(const std::string& reference) const;
    bool known(const std::string& reference) const;
    std::optional<std::filesystem::path> resolveInDocument(const Document&, const std::string& reference) const;
    std::string canonicalReference(const std::string& reference) const;
    nlohmann::json describe(const std::string& path);
    std::filesystem::path checkedPath(const std::string& relative, bool allowRoot = false) const;
    std::filesystem::path root() const;
    std::filesystem::path dataRoot() const;
    // Private, authenticated reads use these paths; public sharing is opt-in per file.
    std::string share(const std::string& path);
    std::optional<std::filesystem::path> sharedPath(const std::string& token) const;
    void addAlias(const std::string& oldReference, const std::string& path);
    nlohmann::json migrateExisting(bool force = false);
    void startMigration(bool force = false);
    void waitForBackground();
    nlohmann::json migrationStatus() const;
    void backupCatalog(const std::filesystem::path& target, const std::vector<std::string>& included = {}) const;
    void refresh();
    nlohmann::json recoverOperations();
    // Mutations serialize separately from metadata reads / sending images.
    std::unique_lock<std::recursive_mutex> lockMutation();
    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};

void setDefault(std::shared_ptr<Service> service);
std::shared_ptr<Service> current();
// Centralized old/new URL and path lookup, preserving old behavior when not initialized.
std::string resolvePath(const std::string& reference);
std::filesystem::path cacheDirectory(const std::string& kind);
// Merge a selected-resource catalog into a snapshot of the current catalog.
// The staged file changes; the running/existing instance's catalog is untouched.
void mergeCatalogForRestore(const std::filesystem::path& staged, const std::filesystem::path& existing);

} // namespace dice::image_resources
