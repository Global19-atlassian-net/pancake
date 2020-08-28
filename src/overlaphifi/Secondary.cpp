// Authors: Ivan Sovic

#include <pacbio/overlaphifi/Secondary.h>

#include <sstream>

namespace PacBio {
namespace Pancake {

std::vector<OverlapPriority> FlagSecondaryAndSupplementary(std::vector<OverlapPtr>& overlaps,
                                                           double allowedOverlapFraction,
                                                           double minSecondaryScoreFraction)
{
    if (overlaps.empty()) {
        return {};
    }

    std::vector<OverlapPriority> priorities(overlaps.size());

    // Mark every region except the first as secondary.
    priorities.front().priority = 0;
    priorities.front().isSupplementary = false;
    for (size_t i = 1; i < overlaps.size(); ++i) {
        auto& ovl = overlaps[i];
        if (ovl == nullptr) {
            throw std::runtime_error("Overlap is nullptr in FlagSecondaryAndSupplementary!");
        }
        priorities[i].priority = 1;
        priorities[i].isSupplementary = false;
    }

    const double topScore = static_cast<double>(overlaps.front()->Score);
    const double minSecondaryScore = topScore * minSecondaryScoreFraction;

    IntervalVectorInt32 queryIntervals;
    std::unordered_map<int32_t, IntervalVectorInt32> targetIntervals;
    IntervalTreeInt32 queryTrees;
    std::unordered_map<int32_t, IntervalTreeInt32> targetTrees;

    // Initial construction of the interval trees.
    CreateRegionIntervalTrees(overlaps, priorities, [](int32_t a) { return a == 0; },
                              queryIntervals, queryTrees, targetIntervals, targetTrees);

    // Loop through all mappings, and check if they overlap the primary mappings and by how much.
    // If the overlap amount is below the specified fraction, mark the region as supplementary and
    // add it to the interval trees.
    for (size_t i = 0; i < overlaps.size(); ++i) {
        const auto& aln = overlaps[i];
        auto& alnPriority = priorities[i];
        if (alnPriority.priority == 0) {
            continue;
        }
        const double score = static_cast<double>(aln->Score);

        if (CheckRegionSupplementary(overlaps, aln, queryTrees, targetTrees,
                                     allowedOverlapFraction)) {
            // This is a supplementary alignment.
            alnPriority.isSupplementary = true;
            alnPriority.priority = 0;

            // Add the new supplementary mapping to the query tree.
            queryIntervals.emplace_back(IntervalInt32(aln->AstartFwd(), aln->AendFwd() - 1, i));
            auto queryIntervalsCopy = queryIntervals;
            queryTrees = IntervalTreeInt32(std::move(queryIntervalsCopy));

            // Add the new supplementary mapping to the target trees.
            int32_t encodedTargetId = (aln->Bid << 1) | static_cast<int32_t>(aln->Brev);
            targetIntervals[encodedTargetId].emplace_back(
                IntervalInt32(aln->BstartFwd(), aln->BendFwd() - 1, i));
            auto targetIntervalsCopy = targetIntervals[encodedTargetId];
            targetTrees[encodedTargetId] = IntervalTreeInt32(std::move(targetIntervalsCopy));

        } else if (score >= minSecondaryScore) {
            // This is a secondary mapping.
            alnPriority.isSupplementary = false;
            alnPriority.priority = 1;

        } else {
            // Score is too low to be marked as secondary. Flag it with a special
            // priority value.
            alnPriority.isSupplementary = false;
            alnPriority.priority = 2;
        }
    }

    return priorities;
}

int32_t CalcIntervalOverlap(int32_t s1, int32_t e1, int32_t s2, int32_t e2)
{
    return std::max(0, std::min(e1, e2) - std::max(s1, s2));
}

void CreateRegionIntervalTrees(const std::vector<OverlapPtr>& overlaps,
                               const std::vector<OverlapPriority>& priorities,
                               std::function<bool(int32_t a)> CompPriority,
                               IntervalVectorInt32& queryIntervals, IntervalTreeInt32& queryTrees,
                               std::unordered_map<int32_t, IntervalVectorInt32>& targetIntervals,
                               std::unordered_map<int32_t, IntervalTreeInt32>& targetTrees)
{
    if (overlaps.size() != priorities.size()) {
        std::ostringstream oss;
        oss << "Size of the overlaps and of the priorities is not the same! overlaps.size() = "
            << overlaps.size() << ", priorities.size() = " << priorities.size();
        throw std::runtime_error(oss.str());
    }

    queryIntervals.clear();
    targetIntervals.clear();
    targetTrees.clear();

    // IntervalVectorInt64 query_intervals_secondary;
    // std::unordered_map<int64_t, IntervalVectorInt64> target_intervals_secondary;
    for (int32_t i = 0; i < static_cast<int32_t>(overlaps.size()); ++i) {
        const auto& aln = overlaps[i];
        // Skip regions that are not of interest.
        if (CompPriority(priorities[i].priority) == false) {
            continue;
        }
        // Accumulate the intervals.
        queryIntervals.emplace_back(IntervalInt32(aln->AstartFwd(), aln->AendFwd() - 1, i));

        int32_t encodedTargetId = (aln->Bid << 1) | static_cast<int32_t>(aln->Brev);
        targetIntervals[encodedTargetId].emplace_back(
            IntervalInt32(aln->BstartFwd(), aln->BendFwd() - 1, i));
    }

    // Construct the query interval trees from the intervals.
    auto queryIntervalsCopy = queryIntervals;
    queryTrees = IntervalTreeInt32(std::move(queryIntervalsCopy));

    // Construct the target interval trees from the intervals.
    for (auto it = targetIntervals.begin(); it != targetIntervals.end(); ++it) {
        auto targetIntervalsCopy = targetIntervals[it->first];
        targetTrees[it->first] = IntervalTreeInt32(std::move(targetIntervalsCopy));
    }
}

bool CheckRegionSupplementary(const std::vector<OverlapPtr>& overlaps, const OverlapPtr& currentOvl,
                              IntervalTreeInt32& queryTrees,
                              std::unordered_map<int32_t, IntervalTreeInt32>& targetTrees,
                              double allowedOverlapFraction)
{
    bool isSupplementary = true;

    /////////////////////////////////////////////////////
    /// Check the query coordinate space for overlaps ///
    /////////////////////////////////////////////////////
    // Find if the current alignment has any overlaps in the query coordinate space.
    auto foundQueryIntervals =
        queryTrees.findOverlapping(currentOvl->AstartFwd(), currentOvl->AendFwd() - 1);
    if (allowedOverlapFraction > 0.0) {  // For speed.
        for (const auto& interval : foundQueryIntervals) {
            auto& otherAln = overlaps[interval.value];
            // Amount of overlap in the query coordinates, as a fraction.
            int32_t ovlQuery = CalcIntervalOverlap(currentOvl->AstartFwd(), currentOvl->AendFwd(),
                                                   otherAln->AstartFwd(), otherAln->AendFwd());
            double ovlQueryFrac =
                static_cast<double>(ovlQuery) /
                static_cast<double>(std::min(currentOvl->ASpan(), otherAln->ASpan()));
            // Amount of overlap in the target coordinates, as a fraction.
            int32_t ovlTarget = CalcIntervalOverlap(currentOvl->BstartFwd(), currentOvl->BendFwd(),
                                                    otherAln->BstartFwd(), otherAln->BendFwd());
            double ovlTargetFrac =
                static_cast<double>(ovlTarget) /
                static_cast<double>(std::min(currentOvl->BSpan(), otherAln->BSpan()));
            // The two mappings shouldn't overlap by more than the allowed fraction in both the query and target coordinates.
            if (ovlQueryFrac <= allowedOverlapFraction && ovlTargetFrac <= allowedOverlapFraction) {
                continue;
            }
            // If it's above the allowed limit, this is not a valid supplementary.
            isSupplementary = false;
            break;
        }
    } else {
        if (foundQueryIntervals.size()) {
            isSupplementary = false;
        }
    }
    // If we found a too large overlap fraction between the current region and another one,
    // this is not a supplementary mapping.
    if (isSupplementary == false) {
        return false;
    }

    //////////////////////////////////////////////////////
    /// Check the target coordinate space for overlaps ///
    //////////////////////////////////////////////////////
    auto itTrees = targetTrees.find(currentOvl->Bid);
    if (itTrees != targetTrees.end()) {
        auto foundTargetIntervals =
            itTrees->second.findOverlapping(currentOvl->BstartFwd(), currentOvl->BendFwd() - 1);
        if (allowedOverlapFraction > 0.0) {  // For speed.
            for (const auto& interval : foundTargetIntervals) {
                auto& otherAln = overlaps[interval.value];
                // Amount of overlap in the query coordinates, as a fraction.
                int32_t ovlQuery =
                    CalcIntervalOverlap(currentOvl->AstartFwd(), currentOvl->AendFwd(),
                                        otherAln->AstartFwd(), otherAln->AendFwd());
                double ovlQueryFrac =
                    static_cast<double>(ovlQuery) /
                    static_cast<double>(std::min(currentOvl->ASpan(), otherAln->ASpan()));
                // Amount of overlap in the target coordinates, as a fraction.
                int32_t ovlTarget =
                    CalcIntervalOverlap(currentOvl->BstartFwd(), currentOvl->BendFwd(),
                                        otherAln->BstartFwd(), otherAln->BendFwd());
                double ovlTargetFrac =
                    static_cast<double>(ovlTarget) /
                    static_cast<double>(std::min(currentOvl->BSpan(), otherAln->BSpan()));
                // The two mappings shouldn't overlap by more than the allowed fraction in both the query and target coordinates.
                if (ovlQueryFrac <= allowedOverlapFraction &&
                    ovlTargetFrac <= allowedOverlapFraction) {
                    continue;
                }
                // If it's above the allowed limit, this is not a valid supplementary.
                isSupplementary = false;
                break;
            }
        } else {
            if (foundTargetIntervals.size()) {
                isSupplementary = false;
            }
        }
    }
    // If we found a too large overlap fraction between the current region and another one,
    // this is not a supplementary mapping.
    if (isSupplementary == false) {
        return false;
    }

    return true;
}

}  // namespace Pancake
}  // namespace PacBio
