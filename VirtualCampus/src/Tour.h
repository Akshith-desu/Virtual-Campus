// Tour.h - guided fly-through of the campus.
//
// Way-points are the main named campus buildings, visited in nearest-neighbour order.
// The camera flies along a closed Catmull-Rom spline through points around and above each
// building, while it looks at a second spline through the buildings themselves.  The spline
// is re-parameterised by arc length (a lookup table), so the camera moves at constant speed.
#pragma once
#include "Campus.h"

class Tour {
public:
    struct Stop { std::string name; glm::vec3 eye, look; };
    std::vector<Stop> stops;
    bool active = false;
    float speed = 18.0f;        // m/s along the path
    float distance = 0;         // travelled along the path
    Mesh pathMesh;              // the path drawn as a glowing ribbon

    // Catmull-Rom: passes through p1 and p2, with tangents from the neighbouring points.
    static glm::vec3 catmullRom(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3, float t) {
        float t2 = t * t, t3 = t2 * t;
        return 0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                       (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
    }

    void build(const Campus& campus) {
        stops.clear();
        // candidate buildings: named, on campus, reasonably large
        std::vector<int> cand;
        for (size_t i = 0; i < campus.buildings.size(); ++i) {
            const BuildingInfo& b = campus.buildings[i];
            if (b.name.empty() || !b.inCampus || b.area < 350.0f) continue;
            cand.push_back((int)i);
        }
        std::sort(cand.begin(), cand.end(), [&](int a, int b) { return campus.buildings[a].area > campus.buildings[b].area; });
        if (cand.size() > 12) cand.resize(12);
        if (cand.size() < 3) return;
        auto centre = [&](int i) { const BuildingInfo& b = campus.buildings[i]; return (b.bmin + b.bmax) * 0.5f; };

        // nearest-neighbour ordering, starting from the building furthest west
        std::vector<int> order;
        std::vector<char> used(cand.size(), 0);
        size_t cur = 0;
        for (size_t i = 1; i < cand.size(); ++i) if (centre(cand[i]).x < centre(cand[cur]).x) cur = i;
        for (size_t n = 0; n < cand.size(); ++n) {
            used[cur] = 1; order.push_back(cand[cur]);
            float best = 1e30f; size_t nxt = cur;
            for (size_t j = 0; j < cand.size(); ++j)
                if (!used[j] && glm::distance(centre(cand[j]), centre(cand[cur])) < best) { best = glm::distance(centre(cand[j]), centre(cand[cur])); nxt = j; }
            cur = nxt;
        }
        for (int bi : order) {
            const BuildingInfo& b = campus.buildings[bi];
            glm::vec2 c = centre(bi);
            glm::vec2 away = c - campus.center;
            away = glm::length(away) < 1.0f ? glm::vec2(1, 0) : glm::normalize(away);
            float r = glm::length(b.bmax - b.bmin) * 0.5f + 35.0f;
            glm::vec2 e = c + away * r;
            stops.push_back({b.name, glm::vec3(e.x, b.height + 28.0f, e.y), glm::vec3(c.x, b.height * 0.5f, c.y)});
        }
        buildTable();
        buildPathMesh();
    }

    float length() const { return table.empty() ? 0 : table.back(); }

    // advance and return the current eye / look-at point and the name of the nearest stop
    void update(float dt, glm::vec3& eye, glm::vec3& look, std::string& caption, float& captionAlpha) {
        if (stops.size() < 3) { active = false; return; }
        distance = std::fmod(distance + speed * dt, length());
        float u = paramAt(distance);
        eye = evalEye(u);
        look = evalLook(u);
        int seg = int(u) % (int)stops.size();
        float f = u - std::floor(u);
        int nearest = f < 0.5f ? seg : (seg + 1) % (int)stops.size();
        caption = stops[nearest].name;
        captionAlpha = glm::clamp(1.0f - std::abs(f - (f < 0.5f ? 0.0f : 1.0f)) * 3.0f, 0.0f, 1.0f);
    }

private:
    std::vector<float> table;   // arc length at u = i / SAMPLES
    static const int SAMPLES = 40;

    glm::vec3 pt(const std::vector<glm::vec3>& P, float u) const {
        int n = (int)P.size();
        int i = int(std::floor(u)) % n;
        float t = u - std::floor(u);
        return catmullRom(P[(i - 1 + n) % n], P[i], P[(i + 1) % n], P[(i + 2) % n], t);
    }
    glm::vec3 evalEye(float u) const { std::vector<glm::vec3> P; for (auto& s : stops) P.push_back(s.eye); return pt(P, u); }
    glm::vec3 evalLook(float u) const { std::vector<glm::vec3> P; for (auto& s : stops) P.push_back(s.look); return pt(P, u); }

    void buildTable() {
        table.clear();
        table.push_back(0);
        glm::vec3 prev = evalEye(0);
        int total = SAMPLES * (int)stops.size();
        for (int i = 1; i <= total; ++i) {
            glm::vec3 p = evalEye(float(i) / SAMPLES);
            table.push_back(table.back() + glm::distance(prev, p));
            prev = p;
        }
    }
    // inverse of the arc-length table: distance -> spline parameter u
    float paramAt(float d) const {
        auto it = std::lower_bound(table.begin(), table.end(), d);
        size_t i = std::max<size_t>(1, size_t(it - table.begin()));
        if (i >= table.size()) return float(stops.size()) - 1e-4f;
        float seg = table[i] - table[i - 1];
        float f = seg > 0 ? (d - table[i - 1]) / seg : 0;
        return (float(i - 1) + f) / SAMPLES;
    }

    void buildPathMesh() {
        MeshBuilder mb;
        glm::vec4 col(0.2f, 0.9f, 1.0f, 0);
        glm::vec3 up(0, 1, 0);
        int total = SAMPLES * (int)stops.size();
        for (int i = 0; i < total; ++i) {
            glm::vec3 a = evalEye(float(i) / SAMPLES), b = evalEye(float(i + 1) / SAMPLES);
            glm::vec3 d = b - a; d.y = 0;
            if (glm::length(d) < 1e-3f) continue;
            glm::vec3 side = glm::normalize(glm::cross(d, up)) * 0.8f;
            mb.quad({a - side, up, {0, 0}, col}, {b - side, up, {0, 0}, col}, {b + side, up, {0, 0}, col}, {a + side, up, {0, 0}, col});
            mb.quad({a - side, -up, {0, 0}, col}, {b - side, -up, {0, 0}, col}, {b + side, -up, {0, 0}, col}, {a + side, -up, {0, 0}, col});
        }
        pathMesh.upload(mb.v);
    }
};
