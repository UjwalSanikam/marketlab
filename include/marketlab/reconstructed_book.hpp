#pragma once

#include <map>
#include <vector>
#include <optional>
#include <string>
#include <functional>
#include <cstdint>

#include "types.hpp"
#include "market_data_types.hpp"

namespace marketlab {

class ReconstructedOrderBook {
public:
    explicit ReconstructedOrderBook(InstrumentMetadata meta = {}) : metadata_(std::move(meta)) {}

    [[nodiscard]] FeedStatus status() const noexcept { return status_; }
    [[nodiscard]] bool is_valid() const noexcept { return status_ == FeedStatus::Valid; }
    [[nodiscard]] uint64_t last_update_id() const noexcept { return last_update_id_; }
    [[nodiscard]] const std::string& invalid_reason() const noexcept { return invalid_reason_; }
    [[nodiscard]] const InstrumentMetadata& metadata() const noexcept { return metadata_; }

    void mark_invalid(std::string reason) {
        status_ = FeedStatus::Invalid;
        invalid_reason_ = std::move(reason);
    }

    // Apply REST depth snapshot
    void apply_snapshot(uint64_t last_update_id,
                        const std::vector<LevelUpdate>& bids,
                        const std::vector<LevelUpdate>& asks) {
        bids_.clear();
        asks_.clear();
        total_bid_qty_ = 0;
        total_ask_qty_ = 0;

        for (const auto& lvl : bids) {
            if (lvl.quantity > 0) {
                bids_[lvl.price] = lvl.quantity;
                total_bid_qty_ += lvl.quantity;
            }
        }
        for (const auto& lvl : asks) {
            if (lvl.quantity > 0) {
                asks_[lvl.price] = lvl.quantity;
                total_ask_qty_ += lvl.quantity;
            }
        }

        last_update_id_ = last_update_id;
        status_ = FeedStatus::Synchronizing;
        invalid_reason_.clear();
    }

    // Apply streaming depth update following Binance synchronization algorithm
    bool apply_depth_update(uint64_t first_update_id,
                            uint64_t final_update_id,
                            const std::vector<LevelUpdate>& bids,
                            const std::vector<LevelUpdate>& asks) {
        if (status_ == FeedStatus::Uninitialized) {
            // Cannot apply depth updates without an initial snapshot
            return false;
        }

        if (status_ == FeedStatus::Synchronizing) {
            // Drop updates that are older than the snapshot
            if (final_update_id <= last_update_id_) {
                return false; // Stale update
            }

            // The first processed update must bridge the snapshot
            if (first_update_id <= last_update_id_ + 1 && final_update_id >= last_update_id_ + 1) {
                // First valid crossing update! Apply level updates
                apply_levels(bids, asks);
                last_update_id_ = final_update_id;
                status_ = FeedStatus::Valid;
                invalid_reason_.clear();
                return true;
            } else if (first_update_id > last_update_id_ + 1) {
                // Gap between snapshot and first update
                mark_invalid("Snapshot gap: expected U <= " + std::to_string(last_update_id_ + 1) +
                             " but got U=" + std::to_string(first_update_id));
                return false;
            } else {
                return false;
            }
        }

        if (status_ == FeedStatus::Valid) {
            // Drop duplicate updates
            if (final_update_id <= last_update_id_) {
                return false; // Duplicate
            }

            // Check sequence continuity
            if (first_update_id <= last_update_id_ + 1) {
                apply_levels(bids, asks);
                last_update_id_ = final_update_id;
                return true;
            } else {
                // Sequence gap!
                mark_invalid("Sequence gap: expected U <= " + std::to_string(last_update_id_ + 1) +
                             " but got U=" + std::to_string(first_update_id));
                return false;
            }
        }

        // status_ == FeedStatus::Invalid
        return false;
    }

    [[nodiscard]] std::optional<Price> best_bid() const noexcept {
        if (bids_.empty()) return std::nullopt;
        return bids_.begin()->first;
    }

    [[nodiscard]] std::optional<Price> best_ask() const noexcept {
        if (asks_.empty()) return std::nullopt;
        return asks_.begin()->first;
    }

    [[nodiscard]] std::optional<Price> mid_price() const noexcept {
        auto bb = best_bid();
        auto ba = best_ask();
        if (bb && ba) {
            return (*bb + *ba) / 2;
        }
        return std::nullopt;
    }

    [[nodiscard]] Quantity get_bid_qty(Price price) const noexcept {
        auto it = bids_.find(price);
        if (it != bids_.end()) return it->second;
        return 0;
    }

    [[nodiscard]] Quantity get_ask_qty(Price price) const noexcept {
        auto it = asks_.find(price);
        if (it != asks_.end()) return it->second;
        return 0;
    }

    [[nodiscard]] Quantity total_bid_quantity() const noexcept { return total_bid_qty_; }
    [[nodiscard]] Quantity total_ask_quantity() const noexcept { return total_ask_qty_; }

    [[nodiscard]] const std::map<Price, Quantity, std::greater<Price>>& bids() const noexcept { return bids_; }
    [[nodiscard]] const std::map<Price, Quantity, std::less<Price>>& asks() const noexcept { return asks_; }

    [[nodiscard]] bool check_invariants(std::string* out_err = nullptr) const {
        if (!bids_.empty() && !asks_.empty()) {
            if (bids_.begin()->first >= asks_.begin()->first) {
                if (out_err) *out_err = "Crossed book: best_bid (" + std::to_string(bids_.begin()->first) +
                                       ") >= best_ask (" + std::to_string(asks_.begin()->first) + ")";
                return false;
            }
        }
        for (const auto& [p, q] : bids_) {
            if (q <= 0) {
                if (out_err) *out_err = "Non-positive quantity at bid level " + std::to_string(p);
                return false;
            }
        }
        for (const auto& [p, q] : asks_) {
            if (q <= 0) {
                if (out_err) *out_err = "Non-positive quantity at ask level " + std::to_string(p);
                return false;
            }
        }
        return true;
    }

private:
    void apply_levels(const std::vector<LevelUpdate>& bids, const std::vector<LevelUpdate>& asks) {
        for (const auto& lvl : bids) {
            auto it = bids_.find(lvl.price);
            if (lvl.quantity == 0) {
                if (it != bids_.end()) {
                    total_bid_qty_ -= it->second;
                    bids_.erase(it);
                }
            } else {
                if (it != bids_.end()) {
                    total_bid_qty_ += (lvl.quantity - it->second);
                    it->second = lvl.quantity;
                } else {
                    total_bid_qty_ += lvl.quantity;
                    bids_[lvl.price] = lvl.quantity;
                }
            }
        }

        for (const auto& lvl : asks) {
            auto it = asks_.find(lvl.price);
            if (lvl.quantity == 0) {
                if (it != asks_.end()) {
                    total_ask_qty_ -= it->second;
                    asks_.erase(it);
                }
            } else {
                if (it != asks_.end()) {
                    total_ask_qty_ += (lvl.quantity - it->second);
                    it->second = lvl.quantity;
                } else {
                    total_ask_qty_ += lvl.quantity;
                    asks_[lvl.price] = lvl.quantity;
                }
            }
        }
    }

    InstrumentMetadata metadata_;
    FeedStatus status_{FeedStatus::Uninitialized};
    uint64_t last_update_id_{0};
    std::string invalid_reason_;

    std::map<Price, Quantity, std::greater<Price>> bids_;
    std::map<Price, Quantity, std::less<Price>> asks_;
    Quantity total_bid_qty_{0};
    Quantity total_ask_qty_{0};
};

} // namespace marketlab
