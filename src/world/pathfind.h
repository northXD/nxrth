// Adonai — A* pathfinder over the per-tile collision grid (ported from Mori/astar.rs).
// Octile costs (10 orthogonal / 14 diagonal), corner-cut prevention, and a path
// cache keyed by (from, to, has_access). Per-bot; rebuilt from tiles each search.
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace adonai::world {

// Open-list node. f = g + h (computed at construction).
struct PathNode {
    std::uint32_t f = 0, g = 0, h = 0, x = 0, y = 0;
    PathNode() = default;
    PathNode(std::uint32_t x_, std::uint32_t y_, std::uint32_t g_, std::uint32_t h_)
        : f(g_ + h_), g(g_), h(h_), x(x_), y(y_) {}
};

// priority_queue comparator: pop smallest f, tie-break smallest h.
struct PathNodeGreater {
    bool operator()(const PathNode& a, const PathNode& b) const {
        if (a.f != b.f) return a.f > b.f;
        return a.h > b.h;
    }
};

// Returned path node. g/h/f are always 0 in the result; only x/y/collision_type
// are meaningful. collision_type is the grid value at (x,y), or 0 if OOB.
struct Node {
    std::uint32_t g = 0, h = 0, f = 0;
    std::uint32_t x = 0, y = 0;
    std::uint8_t collision_type = 0;
    Node(std::uint32_t x_, std::uint32_t y_, std::uint8_t ct) : x(x_), y(y_), collision_type(ct) {}
};

// Path-cache key: (from_x, from_y, to_x, to_y, has_access).
struct PathKey {
    std::uint32_t from_x, from_y, to_x, to_y;
    bool has_access;
    bool operator==(const PathKey& o) const {
        return from_x == o.from_x && from_y == o.from_y && to_x == o.to_x && to_y == o.to_y &&
               has_access == o.has_access;
    }
};

struct PathKeyHash {
    std::size_t operator()(const PathKey& k) const {
        std::uint64_t h = k.from_x;
        h = h * 0x100000001b3ULL ^ k.from_y;
        h = h * 0x100000001b3ULL ^ k.to_x;
        h = h * 0x100000001b3ULL ^ k.to_y;
        h = h * 0x100000001b3ULL ^ (k.has_access ? 1u : 0u);
        return static_cast<std::size_t>(h);
    }
};

class AStar {
public:
    AStar() = default;

    // Grid maintenance ------------------------------------------------------
    // reset(): clear dimensions/grid; clear path_cache only if it has > 1000 entries.
    void reset();
    // Rebuild grid from a raw collision-type buffer (row-major, len == w*h).
    void update_from_collision_data(std::uint32_t width, std::uint32_t height,
                                    const std::vector<std::uint8_t>& data);
    // Rebuild grid from (fg_item_id, collision_type) tiles; fg ignored for the grid.
    void update_from_tiles(std::uint32_t width, std::uint32_t height,
                           const std::vector<std::pair<std::uint16_t, std::uint8_t>>& tiles);
    // Set a single cell and invalidate cache entries near (x,y).
    void update_single_tile(std::uint32_t x, std::uint32_t y, std::uint8_t collision_type);

    // Search ----------------------------------------------------------------
    std::optional<std::vector<Node>> find_path(std::uint32_t from_x, std::uint32_t from_y,
                                               std::uint32_t to_x, std::uint32_t to_y,
                                               bool has_access);

    // Blocked test: OOB index -> blocked; ct 1/6 -> blocked; ct 3 -> !has_access.
    bool is_blocked(std::size_t index, bool has_access) const;

    // Stats -----------------------------------------------------------------
    void clear_cache();
    // (hits, misses, hit_rate_percent).
    std::tuple<std::uint32_t, std::uint32_t, float> cache_stats() const;

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> grid;  // row-major collision grid
    std::unordered_map<PathKey, std::optional<std::vector<std::pair<std::uint32_t, std::uint32_t>>>,
                       PathKeyHash>
        path_cache;
    std::uint32_t cache_hits = 0;
    std::uint32_t cache_misses = 0;

private:
    std::uint8_t grid_at(std::uint32_t x, std::uint32_t y) const;
    std::vector<Node> to_nodes(const std::vector<std::pair<std::uint32_t, std::uint32_t>>& path) const;
};

}  // namespace adonai::world
