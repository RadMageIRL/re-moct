// MarkSet.h - the browser's marked-file set (convert-core slice).
//
// A path-keyed set of marked files: keyed on the absolute path, NOT a row
// index, so a mark survives a re-sort, a directory refresh, and leaving and
// re-entering a directory (the marks persist across navigation within a
// session). Header-only + UIManager-free so the membership contract is
// unit-tested headlessly. The convert-marked action builds its pair list from
// list().
#pragma once

#include <set>
#include <string>
#include <vector>

class MarkSet {
public:
    // Toggle membership; returns true if the path is NOW marked (was added),
    // false if it was removed.
    bool toggle(const std::string& path) {
        auto it = paths_.find(path);
        if (it == paths_.end()) { paths_.insert(path); return true; }
        paths_.erase(it);
        return false;
    }
    void add(const std::string& path)    { paths_.insert(path); }
    void remove(const std::string& path) { paths_.erase(path); }
    void clear()                         { paths_.clear(); }

    bool   contains(const std::string& path) const { return paths_.count(path) != 0; }
    bool   empty() const { return paths_.empty(); }
    size_t size()  const { return paths_.size(); }

    // Sorted (std::set is ordered) snapshot - the convert pair list source.
    std::vector<std::string> list() const {
        return std::vector<std::string>(paths_.begin(), paths_.end());
    }

private:
    std::set<std::string> paths_;
};
