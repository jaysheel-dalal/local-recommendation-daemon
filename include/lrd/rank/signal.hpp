#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace lrd::rank {

/// How strongly a user leans toward one category, in [0, 1].
struct CategoryAffinity {
    std::string category;
    double weight = 0.0;
};

/// The local signal a client sends with a recommendation request.
///
/// ## Privacy shape
///
/// This is the whole of what leaves the calling app, and it is deliberately
/// small: a handful of category weights, not a history. No item ids the user has
/// seen, no timestamps, no identifiers. The daemon never receives raw activity -
/// the app derives these weights on-device from whatever it knows and sends only
/// the summary. That is what "privacy by architecture" means here: the sensitive
/// data has no path to the daemon, rather than being sent and then protected.
///
/// ## Why a vector and not an unordered_map
///
/// Affinities are looked up once per candidate, so the lookup is genuinely hot.
/// A map would hash a short string and chase a pointer; a linear scan over a
/// handful of contiguous entries touches one or two cache lines and beats it
/// comfortably at this size. The crossover is somewhere around 20-30 entries,
/// well above any plausible number of interest categories for one user.
///
/// It also keeps the wire format trivial and the struct cheap to copy.
struct UserSignal {
    std::vector<CategoryAffinity> affinities;

    /// Hard filter: an item in one of these categories is never returned,
    /// whatever it scores. Separate from a zero affinity on purpose - "not
    /// interested" and "must never be shown this" are different statements, and
    /// only the second is a guarantee.
    std::vector<std::string> excluded_categories;

    /// Returns the affinity for `category`, or `fallback` if the signal says
    /// nothing about it.
    [[nodiscard]] double affinity_for(std::string_view category, double fallback) const noexcept {
        for (const CategoryAffinity& entry : affinities) {
            if (entry.category == category) {
                return entry.weight;
            }
        }
        return fallback;
    }

    [[nodiscard]] bool excludes(std::string_view category) const noexcept {
        return std::find(excluded_categories.begin(), excluded_categories.end(), category) !=
               excluded_categories.end();
    }
};

}  // namespace lrd::rank
