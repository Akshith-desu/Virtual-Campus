// Geometry.h - 2D polygon algorithms used to build the campus.
//   * signedArea            - orientation test (shoelace formula)
//   * pointInPolygon        - inside/outside test (ray casting, even-odd rule)
//   * triangulate           - ear-clipping triangulation of a polygon with holes
//                             (holes are first "bridged" into the outer ring)
// Polygons are lists of (x, z) points in metres, no repeated closing point.
#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <algorithm>
#include <cmath>

using Poly = std::vector<glm::vec2>;

inline float signedArea(const Poly& p) {
    float a = 0;
    for (size_t i = 0, j = p.size() - 1; i < p.size(); j = i++)
        a += p[j].x * p[i].y - p[i].x * p[j].y;
    return a * 0.5f;
}

// Ray casting: shoot a ray towards +x and count edge crossings.
inline bool pointInPolygon(const glm::vec2& pt, const Poly& p) {
    bool inside = false;
    for (size_t i = 0, j = p.size() - 1; i < p.size(); j = i++) {
        if ((p[i].y > pt.y) != (p[j].y > pt.y) &&
            pt.x < (p[j].x - p[i].x) * (pt.y - p[i].y) / (p[j].y - p[i].y) + p[i].x)
            inside = !inside;
    }
    return inside;
}

namespace detail {
inline float cross2(const glm::vec2& o, const glm::vec2& a, const glm::vec2& b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}
inline bool inTriangle(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b, const glm::vec2& c) {
    float d1 = cross2(a, b, p), d2 = cross2(b, c, p), d3 = cross2(c, a, p);
    bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(neg && pos);
}

// Merge one hole into the outer ring by cutting a "bridge" from the hole's
// right-most vertex to a visible outer vertex (Eberly's method, simplified).
inline void bridgeHole(Poly& outer, const Poly& hole) {
    size_t hi = 0;
    for (size_t i = 1; i < hole.size(); ++i) if (hole[i].x > hole[hi].x) hi = i;
    glm::vec2 M = hole[hi];

    // cast a ray from M towards +x, find the closest outer edge it hits
    float bestX = 1e30f; size_t bestEdge = SIZE_MAX;
    for (size_t i = 0; i < outer.size(); ++i) {
        const glm::vec2 &a = outer[i], &b = outer[(i + 1) % outer.size()];
        if ((a.y > M.y) == (b.y > M.y)) continue;
        float x = a.x + (M.y - a.y) * (b.x - a.x) / (b.y - a.y);
        if (x >= M.x && x < bestX) { bestX = x; bestEdge = i; }
    }
    size_t vis;
    if (bestEdge == SIZE_MAX) {   // fallback: nearest outer vertex
        vis = 0;
        for (size_t i = 1; i < outer.size(); ++i)
            if (glm::distance(outer[i], M) < glm::distance(outer[vis], M)) vis = i;
    } else {
        size_t a = bestEdge, b = (bestEdge + 1) % outer.size();
        vis = outer[a].x > outer[b].x ? a : b;
        glm::vec2 I(bestX, M.y);
        // if another reflex vertex lies inside triangle (M, I, P), use the one closest in angle
        float bestAng = 1e30f;
        for (size_t i = 0; i < outer.size(); ++i) {
            if (i == vis) continue;
            if (outer[i].x >= M.x && inTriangle(outer[i], M, I, outer[vis])) {
                float ang = std::atan2(std::abs(outer[i].y - M.y), outer[i].x - M.x);
                if (ang < bestAng) { bestAng = ang; vis = i; }
            }
        }
    }
    // splice: outer[0..vis], hole[hi..end, 0..hi], M, outer[vis], outer[vis+1..]
    Poly merged(outer.begin(), outer.begin() + vis + 1);
    for (size_t k = 0; k <= hole.size(); ++k) merged.push_back(hole[(hi + k) % hole.size()]);
    merged.insert(merged.end(), outer.begin() + vis, outer.end());
    outer.swap(merged);
}
} // namespace detail

// Ear clipping. Returns triangles as consecutive point triples (counter-clockwise).
inline std::vector<glm::vec2> triangulate(Poly outer, std::vector<Poly> holes) {
    std::vector<glm::vec2> tris;
    if (outer.size() < 3) return tris;
    if (signedArea(outer) < 0) std::reverse(outer.begin(), outer.end());   // outer = CCW
    for (Poly& h : holes) if (signedArea(h) > 0) std::reverse(h.begin(), h.end());  // holes = CW
    std::sort(holes.begin(), holes.end(), [](const Poly& a, const Poly& b) {
        auto mx = [](const Poly& p) { float m = -1e30f; for (auto& v : p) m = std::max(m, v.x); return m; };
        return mx(a) > mx(b);   // bridge right-most holes first
    });
    for (const Poly& h : holes) if (h.size() >= 3) detail::bridgeHole(outer, h);

    std::vector<size_t> idx(outer.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;

    size_t guard = 0;
    while (idx.size() > 3 && guard++ < 10000) {
        bool clipped = false;
        for (size_t i = 0; i < idx.size(); ++i) {
            size_t ip = idx[(i + idx.size() - 1) % idx.size()], ic = idx[i], in = idx[(i + 1) % idx.size()];
            const glm::vec2 &a = outer[ip], &b = outer[ic], &c = outer[in];
            if (detail::cross2(a, b, c) <= 1e-6f) continue;          // reflex corner: not an ear
            bool empty = true;
            for (size_t j : idx) {
                if (j == ip || j == ic || j == in) continue;
                const glm::vec2& p = outer[j];
                if (p == a || p == b || p == c) continue;              // bridge duplicates
                if (detail::inTriangle(p, a, b, c)) { empty = false; break; }
            }
            if (!empty) continue;
            tris.insert(tris.end(), {a, b, c});                       // clip the ear
            idx.erase(idx.begin() + i);
            clipped = true;
            break;
        }
        if (!clipped) {   // degenerate input: fall back to a fan so nothing is missing
            for (size_t i = 1; i + 1 < idx.size(); ++i)
                tris.insert(tris.end(), {outer[idx[0]], outer[idx[i]], outer[idx[i + 1]]});
            return tris;
        }
    }
    if (idx.size() == 3) tris.insert(tris.end(), {outer[idx[0]], outer[idx[1]], outer[idx[2]]});
    return tris;
}
