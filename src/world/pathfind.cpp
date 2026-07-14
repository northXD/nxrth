// Adonai — A* pathfinder implementation (byte-faithful port of Mori/astar.rs).
#include "world/pathfind.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <queue>
#include <tuple>
#include <unordered_set>

namespace adonai::world {

namespace {

inline std::uint64_t pack(std::uint32_t x, std::uint32_t y) {
    return (static_cast<std::uint64_t>(x) << 32) | y;
}
inline std::uint32_t unpack_x(std::uint64_t p) { return static_cast<std::uint32_t>(p >> 32); }
inline std::uint32_t unpack_y(std::uint64_t p) { return static_cast<std::uint32_t>(p & 0xFFFFFFFFULL); }

// Octile step cost: diagonal -> 14, orthogonal -> 10.
std::uint32_t movement_cost(std::uint32_t fx, std::uint32_t fy, std::uint32_t tx, std::uint32_t ty) {
    std::int64_t dx = std::llabs(static_cast<std::int64_t>(fx) - static_cast<std::int64_t>(tx));
    std::int64_t dy = std::llabs(static_cast<std::int64_t>(fy) - static_cast<std::int64_t>(ty));
    return (dx == 1 && dy == 1) ? 14u : 10u;
}

// Octile heuristic: 14*min(dx,dy) + 10*|dx-dy|.
std::uint32_t calculate_h(std::uint32_t fx, std::uint32_t fy, std::uint32_t tx, std::uint32_t ty) {
    std::int64_t dx = std::llabs(static_cast<std::int64_t>(fx) - static_cast<std::int64_t>(tx));
    std::int64_t dy = std::llabs(static_cast<std::int64_t>(fy) - static_cast<std::int64_t>(ty));
    std::int64_t mn = dx < dy ? dx : dy;
    std::int64_t diff = dx > dy ? dx - dy : dy - dx;
    return static_cast<std::uint32_t>(14 * mn + 10 * diff);
}

// §10.5 neighbor order (affects tie-breaking / determinism).
constexpr std::int32_t kDirs[8][2] = {
    {-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {-1, 1}, {1, -1}, {1, 1},
};

}  // namespace

std::uint8_t AStar::grid_at(std::uint32_t x, std::uint32_t y) const {
    std::size_t idx = static_cast<std::size_t>(y) * width + x;
    if (idx < grid.size()) return grid[idx];
    return 0;
}

bool AStar::is_blocked(std::size_t index, bool has_access) const {
    if (index >= grid.size()) return true;  // out-of-range -> blocked
    std::uint8_t ct = grid[index];
    if (ct == 1 || ct == 6) return true;    // solid
    if (ct == 3) return !has_access;         // access-gated
    return false;                            // passable (0,2,4,5,7,...)
}

void AStar::reset() {
    width = 0;
    height = 0;
    grid.clear();
    if (path_cache.size() > 1000) path_cache.clear();
}

void AStar::update_from_collision_data(std::uint32_t new_width, std::uint32_t new_height,
                                       const std::vector<std::uint8_t>& data) {
    bool dims_changed = new_width != width || new_height != height;
    if (dims_changed) {
        reset();
        width = new_width;
        height = new_height;
        grid.reserve(data.size());
    } else {
        path_cache.clear();
    }
    grid.clear();
    grid.insert(grid.end(), data.begin(), data.end());
}

void AStar::update_from_tiles(std::uint32_t new_width, std::uint32_t new_height,
                              const std::vector<std::pair<std::uint16_t, std::uint8_t>>& tiles) {
    bool dims_changed = new_width != width || new_height != height;
    if (dims_changed) {
        reset();
        width = new_width;
        height = new_height;
        grid.reserve(tiles.size());
    } else {
        path_cache.clear();
    }
    grid.clear();
    for (const auto& t : tiles) grid.push_back(t.second);  // collision_type; fg ignored
}

void AStar::update_single_tile(std::uint32_t x, std::uint32_t y, std::uint8_t collision_type) {
    if (x >= width || y >= height) return;
    std::size_t idx = static_cast<std::size_t>(y) * width + x;
    if (idx >= grid.size()) return;
    grid[idx] = collision_type;
    // Invalidate cache entries whose start OR goal is within the 3x3 of (x,y).
    for (auto it = path_cache.begin(); it != path_cache.end();) {
        const PathKey& k = it->first;
        bool start_near = k.from_x <= x + 1 && k.from_x + 1 >= x && k.from_y <= y + 1 && k.from_y + 1 >= y;
        bool goal_near = k.to_x <= x + 1 && k.to_x + 1 >= x && k.to_y <= y + 1 && k.to_y + 1 >= y;
        if (start_near || goal_near) {
            it = path_cache.erase(it);
        } else {
            ++it;
        }
    }
}

std::vector<Node> AStar::to_nodes(
    const std::vector<std::pair<std::uint32_t, std::uint32_t>>& path) const {
    std::vector<Node> out;
    out.reserve(path.size());
    for (const auto& p : path) out.emplace_back(p.first, p.second, grid_at(p.first, p.second));
    return out;
}

std::optional<std::vector<Node>> AStar::find_path(std::uint32_t from_x, std::uint32_t from_y,
                                                  std::uint32_t to_x, std::uint32_t to_y,
                                                  bool has_access) {
    PathKey key{from_x, from_y, to_x, to_y, has_access};

    // 1. Cache check.
    if (auto cached = path_cache.find(key); cached != path_cache.end()) {
        ++cache_hits;
        if (cached->second.has_value()) return to_nodes(*cached->second);
        return std::nullopt;
    }
    // 2. Miss.
    ++cache_misses;

    // 3. Bounds.
    if (from_x >= width || from_y >= height || to_x >= width || to_y >= height) {
        path_cache[key] = std::nullopt;
        return std::nullopt;
    }

    // 4. Endpoint indices.
    std::size_t start_index = static_cast<std::size_t>(from_y) * width + from_x;
    std::size_t end_index = static_cast<std::size_t>(to_y) * width + to_x;

    // 5. Blocked endpoints.
    if (is_blocked(start_index, has_access) || is_blocked(end_index, has_access)) {
        path_cache[key] = std::nullopt;
        return std::nullopt;
    }

    // 6. Trivial path.
    if (from_x == to_x && from_y == to_y) {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> path = {{from_x, from_y}};
        path_cache[key] = path;
        std::vector<Node> out;
        out.emplace_back(from_x, from_y, grid[start_index]);
        return out;
    }

    // 7. Search setup.
    std::priority_queue<PathNode, std::vector<PathNode>, PathNodeGreater> open_list;
    std::unordered_map<std::uint64_t, std::uint64_t> came_from;
    std::unordered_map<std::uint64_t, std::uint32_t> g_scores;
    std::unordered_set<std::uint64_t> closed_set;
    came_from.reserve(256);
    g_scores.reserve(256);
    closed_set.reserve(256);

    open_list.emplace(from_x, from_y, 0u, calculate_h(from_x, from_y, to_x, to_y));
    g_scores[pack(from_x, from_y)] = 0;

    // 8. Main loop.
    while (!open_list.empty()) {
        PathNode current = open_list.top();
        open_list.pop();

        if (current.x == to_x && current.y == to_y) {
            // reconstruct_optimized_path (§10.6)
            std::vector<std::pair<std::uint32_t, std::uint32_t>> path;
            std::uint64_t start_packed = pack(from_x, from_y);
            std::uint64_t cur = pack(current.x, current.y);
            while (cur != start_packed) {
                path.emplace_back(unpack_x(cur), unpack_y(cur));
                auto it = came_from.find(cur);
                if (it == came_from.end()) break;
                cur = it->second;
            }
            path.emplace_back(unpack_x(start_packed), unpack_y(start_packed));
            std::reverse(path.begin(), path.end());
            path_cache[key] = path;
            return to_nodes(path);
        }

        std::uint64_t cur_packed = pack(current.x, current.y);
        if (closed_set.count(cur_packed)) continue;
        closed_set.insert(cur_packed);

        // process_neighbors (§10.5)
        for (const auto& d : kDirs) {
            std::int64_t nx = static_cast<std::int64_t>(current.x) + d[0];
            std::int64_t ny = static_cast<std::int64_t>(current.y) + d[1];
            if (nx < 0 || ny < 0 || nx >= static_cast<std::int64_t>(width) ||
                ny >= static_cast<std::int64_t>(height)) {
                continue;
            }
            std::uint32_t unx = static_cast<std::uint32_t>(nx);
            std::uint32_t uny = static_cast<std::uint32_t>(ny);
            std::uint64_t np = pack(unx, uny);
            if (closed_set.count(np)) continue;

            std::size_t index = static_cast<std::size_t>(uny) * width + unx;
            if (is_blocked(index, has_access)) continue;

            // Diagonal corner-cut prevention.
            if (d[0] != 0 && d[1] != 0) {
                std::size_t adj1 = static_cast<std::size_t>(current.y) * width + unx;
                std::size_t adj2 = static_cast<std::size_t>(uny) * width + current.x;
                if (is_blocked(adj1, has_access) || is_blocked(adj2, has_access)) continue;
            }

            std::uint32_t tentative_g = current.g + movement_cost(current.x, current.y, unx, uny);
            if (auto git = g_scores.find(np); git != g_scores.end() && tentative_g >= git->second) {
                continue;
            }
            g_scores[np] = tentative_g;
            came_from[np] = cur_packed;
            std::uint32_t h = calculate_h(unx, uny, to_x, to_y);
            open_list.emplace(unx, uny, tentative_g, h);
        }
    }

    // 9. No path.
    path_cache[key] = std::nullopt;
    return std::nullopt;
}

void AStar::clear_cache() {
    path_cache.clear();
    cache_hits = 0;
    cache_misses = 0;
}

std::tuple<std::uint32_t, std::uint32_t, float> AStar::cache_stats() const {
    std::uint32_t total = cache_hits + cache_misses;
    float hit_rate = total > 0 ? static_cast<float>(cache_hits) / static_cast<float>(total) * 100.0f
                               : 0.0f;
    return {cache_hits, cache_misses, hit_rate};
}

}  // namespace adonai::world
