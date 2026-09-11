#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lrd::cache {

struct CacheMetrics {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t insertions = 0;
    std::uint64_t updates = 0;
    std::uint64_t evictions = 0;
};

/// A fixed-capacity cache with least-recently-used eviction.
///
/// ## Structure
///
/// Two containers that have to agree with each other:
///
///   entries_   std::list<Entry>, most-recently-used at the front.
///   index_     unordered_map<Key, iterator-into-that-list>.
///
/// The map gives O(1) lookup; the list gives O(1) reordering. Neither can do
/// both alone, which is why every LRU cache in every codebase looks like this.
///
/// ## Why std::list specifically
///
/// The load-bearing property is that **std::list iterators stay valid across
/// splice()**. Moving an entry to the front is a pointer rewire, not a move of
/// the element, so the iterators stored in index_ keep pointing at the right
/// nodes and nothing has to be reindexed. That is what makes a touch O(1).
///
/// A std::vector or std::deque would invalidate iterators on almost any
/// modification, so the map would have to store indices and every reorder would
/// mean shifting elements and rewriting the map - O(n) per access.
///
/// The cost of std::list is real and worth naming: every entry is a separate
/// heap allocation, and traversal is pointer-chasing rather than a linear scan.
/// For a cache that is the right trade - we index into it, we never scan it -
/// but it is the reason an intrusive list is what a production implementation
/// reaches for next.
///
/// ## Why each Entry stores its own key
///
/// Eviction starts from the list (take the back) and has to remove the matching
/// entry from the map, which needs the key. Without a copy of the key in the
/// node, evicting would mean scanning the map to find the entry that points at
/// this iterator - O(n), on the hot path.
///
/// The price is that every key is stored twice: once as the map's key and once
/// in the node. For short string keys that is a few dozen bytes per entry and
/// not worth chasing. The usual fix is to store the key only in the node and
/// key the map by a view into it, which trades a real memory saving for
/// lifetime rules that are easy to get subtly wrong - worth doing only once
/// profiling says the memory matters.
///
/// ## Thread safety
///
/// None. This class is deliberately not synchronised; step 5 wraps it. See
/// docs/concurrency.md for why that wrapper cannot be a reader-writer lock over
/// this interface, which is the single most important thing to understand about
/// this file.
template <typename Key, typename Value, typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>>
class LruCache {
public:
    /// Values are handed out as shared_ptr<const Value>, not by value and not
    /// by reference. That choice is about step 5, and it is worth stating now:
    ///
    ///   * A reference or pointer into the cache would dangle the moment the
    ///     lock is released - or the entry is evicted by another thread.
    ///   * Returning Value by value copies the whole payload *while holding the
    ///     lock*, which is exactly where copying hurts most.
    ///   * A shared_ptr copies one refcounted pointer under the lock, and the
    ///     value stays alive for as long as the caller holds it, even if the
    ///     entry is evicted or overwritten a microsecond later.
    ///
    /// The `const` matters too: it makes values immutable once published, so a
    /// writer replacing an entry swaps in a *new* pointer rather than mutating
    /// one that readers may be looking at. Readers therefore always see a
    /// coherent snapshot and never a half-written value.
    using ValuePtr = std::shared_ptr<const Value>;

    explicit LruCache(std::size_t capacity) : capacity_(capacity) {
        if (capacity == 0) {
            // A zero-capacity cache is almost always a configuration mistake,
            // and the alternative - silently evicting everything immediately -
            // would look like a bug in the caller's code rather than in its
            // configuration.
            throw std::invalid_argument("LruCache capacity must be at least 1");
        }
        index_.reserve(capacity);
    }

    /// Looks up `key` and marks it most-recently-used. Returns nullptr on miss.
    ///
    /// **Note that this is not const.** A cache hit reorders the list, so a
    /// read here is a write to the data structure. That single fact is why a
    /// std::shared_mutex cannot simply be dropped over this class - see
    /// docs/concurrency.md.
    [[nodiscard]] ValuePtr get(const Key& key) {
        const auto found = index_.find(key);
        if (found == index_.end()) {
            ++metrics_.misses;
            return nullptr;
        }

        ++metrics_.hits;
        // O(1), and - critically - `found->second` remains valid afterwards.
        entries_.splice(entries_.begin(), entries_, found->second);
        return entries_.front().value;
    }

    /// Looks up `key` *without* affecting recency.
    ///
    /// This one really is const, and that is the point of its existence: it is
    /// the operation that could safely run under a shared lock while other
    /// readers do the same. Useful for diagnostics, and a concrete illustration
    /// of what get() would have to become for a reader-writer lock to work.
    [[nodiscard]] ValuePtr peek(const Key& key) const {
        const auto found = index_.find(key);
        return found == index_.end() ? nullptr : found->second->value;
    }

    /// Inserts or replaces `key`, marking it most-recently-used. Evicts the
    /// least-recently-used entry first if the cache is full.
    void put(const Key& key, Value value) {
        put_ptr(key, std::make_shared<const Value>(std::move(value)));
    }

    /// Overload for a value that is already shared - lets a caller publish one
    /// buffer to several keys without copying it.
    void put_shared(const Key& key, ValuePtr value) { put_ptr(key, std::move(value)); }

    /// Returns true if the key was present.
    bool erase(const Key& key) {
        const auto found = index_.find(key);
        if (found == index_.end()) {
            return false;
        }
        entries_.erase(found->second);
        index_.erase(found);
        return true;
    }

    void clear() noexcept {
        entries_.clear();
        index_.clear();
    }

    [[nodiscard]] bool contains(const Key& key) const { return index_.contains(key); }
    [[nodiscard]] std::size_t size() const noexcept { return index_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] const CacheMetrics& metrics() const noexcept { return metrics_; }

    /// Visits every entry, oldest-to-newest, without disturbing recency.
    ///
    /// **const, and that is the point.** Ranking has to scan the whole cache to
    /// score candidates. Doing that through get() would mark every item
    /// most-recently-used on every recommendation, which destroys the recency
    /// signal the eviction policy depends on - the cache would evict essentially
    /// at random. A scan is not a use.
    template <typename Fn>
    void for_each(Fn&& visit) const {
        for (const Entry& entry : entries_) {
            visit(entry.key, entry.value);
        }
    }

    /// Keys in recency order, most-recently-used first.
    ///
    /// Exists for tests: asserting on eviction *order* is the only way to know
    /// the LRU discipline actually holds, and checking it through get() would
    /// perturb the very thing under test.
    [[nodiscard]] std::vector<Key> keys_mru_to_lru() const {
        std::vector<Key> keys;
        keys.reserve(entries_.size());
        for (const Entry& entry : entries_) {
            keys.push_back(entry.key);
        }
        return keys;
    }

private:
    struct Entry {
        Key key;  ///< A copy, so eviction can remove the matching index entry.
        ValuePtr value;
    };

    using EntryList = std::list<Entry>;
    using Index = std::unordered_map<Key, typename EntryList::iterator, Hash, KeyEqual>;

    void put_ptr(const Key& key, ValuePtr value) {
        if (const auto found = index_.find(key); found != index_.end()) {
            ++metrics_.updates;
            // Replace the pointer rather than mutating the pointed-to value.
            // Any reader still holding the previous ValuePtr keeps a coherent
            // old snapshot instead of watching a value change underneath it.
            found->second->value = std::move(value);
            entries_.splice(entries_.begin(), entries_, found->second);
            return;
        }

        // Evict before inserting, so the cache never momentarily exceeds its
        // capacity. With a large value that transient overshoot could be the
        // difference between fitting in memory and not.
        if (index_.size() >= capacity_) {
            evict_lru();
        }

        entries_.push_front(Entry{key, std::move(value)});
        index_.emplace(key, entries_.begin());
        ++metrics_.insertions;
    }

    void evict_lru() {
        if (entries_.empty()) {
            return;
        }
        // The back of the list is by construction the least recently used.
        index_.erase(entries_.back().key);
        entries_.pop_back();
        ++metrics_.evictions;
    }

    std::size_t capacity_;
    EntryList entries_;  ///< front = most recently used, back = least.
    Index index_;
    CacheMetrics metrics_;
};

}  // namespace lrd::cache
