// Campus.h - loads campus.json (made by tools/osm_to_campus.py) and turns it into meshes:
//   buildings : footprint polygons extruded to their height (walls) + ear-clipped roofs
//   roads     : polylines widened into strips, with round joints at the bends
//   areas     : lawns, sports pitches, water, parking (triangulated flat polygons)
// Also answers two questions for the rest of the app:
//   collides(p) - is a point inside a building?  (walk-mode collision)
//   pick(ray)   - which building does a ray hit first?  (mouse hover / selection)
#pragma once
#include "Geometry.h"
#include "Mesh.h"
#include "Textures.h"
#include <nlohmann/json.hpp>
#include <cctype>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

struct BuildingInfo {
    std::string name, type;
    std::string shortName;                // label shown on the map pins (e.g. "ECE Department")
    float height = 8;
    bool inCampus = true;
    Poly outer;
    std::vector<Poly> holes;
    glm::vec2 bmin{1e9f}, bmax{-1e9f};   // bounding box (x, z)
    float area = 0;                       // footprint m^2
};

struct RoadInfo {
    std::string name, type;
    float width = 5;
    bool vehicle = true, oneway = false;
    std::vector<glm::vec2> path;
    bool gate = false;                    // the short piece of road through a gate in the campus wall
};

struct AreaInfo {
    std::string type;
    Poly outer;
};

class Campus {
public:
    std::string name = "Placeholder campus";
    std::vector<BuildingInfo> buildings;
    std::vector<RoadInfo> roads;
    std::vector<AreaInfo> areas;
    Poly boundary;
    glm::vec2 center{0}, extent{200};      // of the campus boundary / buildings

    Mesh buildingMesh, outsideMesh;        // campus buildings + compound wall | buildings outside the campus
    Mesh roadMesh, pathMesh, areaMesh;
    Mesh treeMesh, tree2Mesh;              // rain tree + tall mast tree, each drawn many times (instancing)
    std::vector<glm::vec4> trees;          // per tree: x, z, scale, rotation (radians)
    std::vector<char> treeKind;            // 0 = round rain tree, 1 = tall mast tree
    GLuint treeInstanceVBO = 0, tree2InstanceVBO = 0;
    GLsizei treeCount[2] = {0, 0};

    // compound wall along the campus boundary, with gaps (gates) where roads cross it
    struct WallPiece { glm::vec2 a, b; };
    std::vector<WallPiece> wallPieces;
    struct Gate { glm::vec2 pos; float width; bool vehicle; int road; int group = -1; };
    // gates close together (e.g. separate IN and OUT roads) are one named gate
    struct GateGroup { std::string name; glm::vec2 pos; std::vector<int> roads; };
    std::vector<GateGroup> gateGroups;
    int mainGateGroup = -1;
    std::vector<Gate> gates;
    int mainGate = -1;

    int style = 0;                         // 0 = natural colours, 1 = "venue map" colours

    bool load(const std::string& path) {
        std::ifstream f(path);
        if (!f) { std::cerr << "No campus file at " << path << " - using placeholder campus\n"; return false; }
        nlohmann::json j;
        try { f >> j; } catch (const std::exception& e) { std::cerr << "Bad campus.json: " << e.what() << "\n"; return false; }

        auto toPoly = [](const nlohmann::json& a) {
            Poly p; for (auto& q : a) p.emplace_back(q[0].get<float>(), q[1].get<float>()); return p;
        };
        name = j.value("name", "Campus");
        boundary = toPoly(j.value("boundary", nlohmann::json::array()));
        for (auto& b : j["buildings"]) {
            BuildingInfo bi;
            bi.name = b.value("name", "");
            bi.shortName = b.value("short", bi.name);
            bi.type = b.value("type", "yes");
            bi.height = b.value("height", 8.0f);
            bi.inCampus = b.value("inCampus", true);
            bi.outer = toPoly(b["outer"]);
            for (auto& h : b["holes"]) bi.holes.push_back(toPoly(h));
            if (bi.outer.size() >= 3) buildings.push_back(bi);
        }
        for (auto& r : j["roads"]) {
            RoadInfo ri;
            ri.name = r.value("name", "");
            ri.type = r.value("type", "");
            ri.width = r.value("width", 5.0f);
            ri.vehicle = r.value("vehicle", true);
            ri.oneway = r.value("oneway", false);
            ri.path = toPoly(r["path"]);
            roads.push_back(ri);
        }
        for (auto& a : j["areas"]) areas.push_back({a.value("type", "green"), toPoly(a["outer"])});
        std::cout << "Loaded " << name << ": " << buildings.size() << " buildings, "
                  << roads.size() << " roads, " << areas.size() << " areas\n";
        return true;
    }

    // Fallback so the app still runs without campus.json (the step-1 layout).
    void loadPlaceholder() {
        struct B { const char* n; float x, z, w, d, h; };
        const B list[] = {{"Main Block", 0, -40, 60, 25, 22}, {"Library", -55, 10, 30, 30, 15},
                          {"CSE Block", 55, 10, 35, 22, 18}, {"Mechanical Block", 60, -55, 28, 35, 12},
                          {"Auditorium", -60, -55, 32, 26, 14}, {"Canteen", -20, 55, 22, 16, 6}};
        for (const B& b : list) {
            BuildingInfo bi; bi.name = b.n; bi.shortName = b.n; bi.type = "college"; bi.height = b.h;
            bi.outer = {{b.x - b.w / 2, b.z - b.d / 2}, {b.x + b.w / 2, b.z - b.d / 2},
                        {b.x + b.w / 2, b.z + b.d / 2}, {b.x - b.w / 2, b.z + b.d / 2}};
            buildings.push_back(bi);
        }
        roads.push_back({"", "secondary", 10, true, false, {{-110, 25}, {110, 25}}});
        roads.push_back({"", "secondary", 8, true, false, {{0, -75}, {0, 75}}});
    }

    void build() {
        computeBounds();
        splitAtGates();
        findGates();
        buildBuildings();
        buildRoads();
        buildAreas();
        buildTrees();
    }

    // Do segments ab and cd cross? (orientation test; used to find where roads pass through the wall)
    static bool segmentsCross(glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec2 d) {
        auto orient = [](glm::vec2 p, glm::vec2 q, glm::vec2 r) { return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x); };
        float d1 = orient(c, d, a), d2 = orient(c, d, b), d3 = orient(a, b, c), d4 = orient(a, b, d);
        return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
    }

    // Instanced rendering: the GPU draws the same tree mesh once per entry in `trees`.
    void drawTrees() const {
        if (treeCount[0]) { glBindVertexArray(treeMesh.vao);  glDrawArraysInstanced(GL_TRIANGLES, 0, treeMesh.count, treeCount[0]); }
        if (treeCount[1]) { glBindVertexArray(tree2Mesh.vao); glDrawArraysInstanced(GL_TRIANGLES, 0, tree2Mesh.count, treeCount[1]); }
    }

    // split the trees by kind into the two instance buffers
    void uploadTreeInstances() {
        std::vector<glm::vec4> k0, k1;
        for (size_t i = 0; i < trees.size(); ++i) (treeKind[i] ? k1 : k0).push_back(trees[i]);
        treeCount[0] = (GLsizei)k0.size(); treeCount[1] = (GLsizei)k1.size();
        glBindBuffer(GL_ARRAY_BUFFER, treeInstanceVBO);
        glBufferData(GL_ARRAY_BUFFER, k0.size() * sizeof(glm::vec4), k0.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, tree2InstanceVBO);
        glBufferData(GL_ARRAY_BUFFER, k1.size() * sizeof(glm::vec4), k1.data(), GL_STATIC_DRAW);
    }

    // re-colour everything for the chosen style
    void setStyle(int st) {
        style = st;
        buildBuildings();
        buildRoads();
        buildAreas();
    }

    bool hitsTree(const glm::vec2& p, float r = 0.5f) const {
        for (const glm::vec4& t : trees)
            if (std::abs(p.x - t.x) < r + 0.3f && std::abs(p.y - t.y) < r + 0.3f &&
                glm::distance(p, glm::vec2(t.x, t.y)) < r + 0.25f * t.z) return true;
        return false;
    }

    // ---------------------------------------------------------------- queries
    // inside a building footprint (and not in its courtyard)?
    static bool insideFootprint(const BuildingInfo& b, const glm::vec2& p) {
        if (!pointInPolygon(p, b.outer)) return false;
        for (const Poly& h : b.holes) if (pointInPolygon(p, h)) return false;
        return true;
    }

    bool collides(const glm::vec2& p, float margin = 0.4f) const {
        const glm::vec2 offs[5] = {{0, 0}, {margin, 0}, {-margin, 0}, {0, margin}, {0, -margin}};
        for (const BuildingInfo& b : buildings) {
            if (p.x < b.bmin.x - margin || p.x > b.bmax.x + margin ||
                p.y < b.bmin.y - margin || p.y > b.bmax.y + margin) continue;
            for (const glm::vec2& o : offs) if (insideFootprint(b, p + o)) return true;
        }
        return hitsWall(p, margin);
    }

    bool hitsWall(const glm::vec2& p, float margin) const {
        for (const WallPiece& w : wallPieces) {
            glm::vec2 ab = w.b - w.a;
            float l2 = glm::dot(ab, ab);
            float t = l2 > 0 ? glm::clamp(glm::dot(p - w.a, ab) / l2, 0.0f, 1.0f) : 0.0f;
            if (glm::distance(p, w.a + ab * t) < 0.2f + margin) return true;
        }
        return false;
    }

    // Ray picking: slab test against each building's 3D bounding box,
    // then march along the ray inside the box testing the real footprint.
    int pick(const glm::vec3& origin, const glm::vec3& dir, float maxDist = 3000.0f, bool campusOnly = false) const {
        int best = -1; float bestT = maxDist;
        for (size_t i = 0; i < buildings.size(); ++i) {
            const BuildingInfo& b = buildings[i];
            if (campusOnly && !b.inCampus) continue;
            glm::vec3 lo(b.bmin.x, 0, b.bmin.y), hi(b.bmax.x, b.height, b.bmax.y);
            float t0 = 0, t1 = bestT;
            bool miss = false;
            for (int a = 0; a < 3 && !miss; ++a) {
                if (std::abs(dir[a]) < 1e-6f) { if (origin[a] < lo[a] || origin[a] > hi[a]) miss = true; continue; }
                float ta = (lo[a] - origin[a]) / dir[a], tb = (hi[a] - origin[a]) / dir[a];
                if (ta > tb) std::swap(ta, tb);
                t0 = std::max(t0, ta); t1 = std::min(t1, tb);
                if (t0 > t1) miss = true;
            }
            if (miss) continue;
            for (float t = t0; t <= t1; t += 0.25f) {
                glm::vec3 p = origin + dir * t;
                if (p.y >= 0 && p.y <= b.height && insideFootprint(b, {p.x, p.z})) {
                    if (t < bestT) { bestT = t; best = (int)i; }
                    break;
                }
            }
        }
        return best;
    }

    // A spot on a road near the middle of campus to start walking from.
    glm::vec2 startPoint() const {
        glm::vec2 best = center; float bestD = 1e30f;
        for (const RoadInfo& r : roads) {
            if (!r.vehicle) continue;
            for (const glm::vec2& p : r.path) {
                if (!boundary.empty() && !pointInPolygon(p, boundary)) continue;
                float d = glm::distance(p, center);
                if (d < bestD && !collides(p, 1.5f)) { bestD = d; best = p; }
            }
        }
        return best;
    }

private:
    // shortest distance from p to the centre line of any road, minus its half width
    float roadClearance(const glm::vec2& p) const {
        float best = 1e9f;
        for (const RoadInfo& r : roads)
            for (size_t k = 0; k + 1 < r.path.size(); ++k) {
                glm::vec2 a = r.path[k], b = r.path[k + 1], ab = b - a;
                float l2 = glm::dot(ab, ab);
                float t = l2 > 0 ? glm::clamp(glm::dot(p - a, ab) / l2, 0.0f, 1.0f) : 0.0f;
                best = std::min(best, glm::distance(p, a + ab * t) - r.width * 0.5f);
            }
        return best;
    }

    void buildTrees() {
        unsigned seed = 12345u;
        auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return float(seed >> 8) / float(1u << 24); };
        float mastShare = 0.25f;                  // share of tall mast trees (avenues have more)
        auto tryPlace = [&](glm::vec2 p, float minRoadGap) {
            if (collides(p, 3.5f)) return;                                    // not in / against a building
            if (roadClearance(p) < minRoadGap) return;                        // not on a road or path
            for (const glm::vec4& t : trees)                                  // not on top of another tree
                if (std::abs(t.x - p.x) < 4.5f && std::abs(t.y - p.y) < 4.5f) return;
            trees.push_back({p.x, p.y, 0.75f + 0.6f * rnd(), rnd() * 6.2831853f});
            treeKind.push_back(rnd() < mastShare ? 1 : 0);
        };
        // avenue trees along both sides of the vehicle roads
        for (const RoadInfo& r : roads) {
            if (!r.vehicle) continue;
            for (size_t k = 0; k + 1 < r.path.size(); ++k) {
                glm::vec2 a = r.path[k], b = r.path[k + 1], d = b - a;
                float len = glm::length(d);
                if (len < 1) continue;
                glm::vec2 dir = d / len, side(-dir.y, dir.x);
                for (float s = 6; s < len; s += 13 + 4 * rnd())
                    for (int sgn = -1; sgn <= 1; sgn += 2)
                        if (rnd() < 0.8f) tryPlace(a + dir * s + side * float(sgn) * (r.width * 0.5f + 2.8f), 1.5f);
            }
        }
        // scattered trees in lawns and gardens
        mastShare = 0.15f;
        for (const AreaInfo& ar : areas) {
            if (ar.type != "green" && ar.type != "scrub") continue;
            glm::vec2 lo(1e9f), hi(-1e9f);
            for (const glm::vec2& p : ar.outer) { lo = glm::min(lo, p); hi = glm::max(hi, p); }
            int n = int((hi.x - lo.x) * (hi.y - lo.y) / 180.0f);
            for (int i = 0; i < n && i < 400; ++i) {
                glm::vec2 p(lo.x + rnd() * (hi.x - lo.x), lo.y + rnd() * (hi.y - lo.y));
                if (pointInPolygon(p, ar.outer)) tryPlace(p, 1.0f);
            }
        }
        // a few more inside the campus boundary, between buildings
        if (!boundary.empty()) {
            for (int i = 0; i < 6000; ++i) {
                glm::vec2 p(center.x + (rnd() - 0.5f) * extent.x, center.y + (rnd() - 0.5f) * extent.y);
                if (pointInPolygon(p, boundary) && rnd() < 0.55f) tryPlace(p, 2.0f);
            }
            // a row of trees just inside the compound wall
            mastShare = 0.4f;
            for (size_t k = 0; k < boundary.size(); ++k) {
                glm::vec2 a = boundary[k], b = boundary[(k + 1) % boundary.size()], d = b - a;
                float len = glm::length(d);
                if (len < 1) continue;
                glm::vec2 dir = d / len, in(-dir.y, dir.x);
                if (!pointInPolygon((a + b) * 0.5f + in * 4.0f, boundary)) in = -in;
                for (float s0 = 5; s0 < len; s0 += 9 + 4 * rnd()) tryPlace(a + dir * s0 + in * 4.0f, 2.0f);
            }
        }

        // ---- two tree meshes: a broad rain tree and a tall, slim mast tree (common on Bengaluru campuses)
        const float t = 1.618034f;
        std::vector<glm::vec3> V = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
                                    {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
        const int F[20][3] = {{0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},{1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},
                              {3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},{4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1}};
        auto makeTree = [&](Mesh& mesh, float trunkH, float trunkR, glm::vec3 canopyScale, float canopyY, glm::vec3 leaf, int seedK) {
            MeshBuilder mb;
            glm::vec4 bark(0.38f, 0.29f, 0.20f, 0);
            const int S = 7;
            for (int k = 0; k < S; ++k) {
                float a0 = k * 6.2831853f / S, a1 = (k + 1) * 6.2831853f / S;
                glm::vec3 n0(std::cos(a0), 0, std::sin(a0)), n1(std::cos(a1), 0, std::sin(a1));
                mb.quad({n0 * trunkR, n0, {0, 0}, bark}, {n1 * trunkR, n1, {1, 0}, bark},
                        {n1 * trunkR * 0.6f + glm::vec3(0, trunkH, 0), n1, {1, 1}, bark}, {n0 * trunkR * 0.6f + glm::vec3(0, trunkH, 0), n0, {0, 1}, bark});
            }
            auto canopyVertex = [&](glm::vec3 p) {
                p = glm::normalize(p);
                float bump = 0.82f + 0.36f * tex::hash(int(p.x * 50 + 100), int(p.y * 50 + 100) * 7 + int(p.z * 50 + 100), seedK);
                glm::vec3 pos = p * bump * canopyScale + glm::vec3(0, canopyY, 0);
                // lighter on top (sunlit leaves), darker underneath, a little random variation
                float g = (0.78f + 0.3f * tex::hash(int(p.x * 13 + 50), int(p.z * 13 + 50), seedK + 1)) * (0.75f + 0.35f * std::max(p.y, 0.0f));
                glm::vec3 n = glm::normalize(p / canopyScale);       // normal of the stretched sphere
                return Vertex{pos, n, {0, 0}, glm::vec4(leaf * g, 0)};
            };
            for (auto& f : F) {
                glm::vec3 a = V[f[0]], b = V[f[1]], c = V[f[2]];
                glm::vec3 ab = (a + b) * 0.5f, bc = (b + c) * 0.5f, ca = (c + a) * 0.5f;
                glm::vec3 sub[4][3] = {{a, ab, ca}, {ab, b, bc}, {ca, bc, c}, {ab, bc, ca}};
                for (auto& s3 : sub) {
                    Vertex v0 = canopyVertex(s3[0]), v1 = canopyVertex(s3[1]), v2 = canopyVertex(s3[2]);
                    glm::vec3 outward = glm::normalize(v0.normal + v1.normal + v2.normal);
                    glm::vec3 n = glm::cross(v1.pos - v0.pos, v2.pos - v0.pos);
                    if (glm::dot(n, outward) < 0) std::swap(v1, v2);
                    mb.v.push_back(v0); mb.v.push_back(v1); mb.v.push_back(v2);
                }
            }
            mesh.upload(mb.v);
        };
        makeTree(treeMesh, 4.0f, 0.28f, glm::vec3(3.4f, 2.3f, 3.4f), 5.6f, glm::vec3(0.24f, 0.42f, 0.16f), 77);   // rain tree
        makeTree(tree2Mesh, 2.0f, 0.15f, glm::vec3(1.2f, 5.0f, 1.2f), 6.4f, glm::vec3(0.13f, 0.32f, 0.12f), 91);  // mast tree

        // per-instance attribute (location 4) advances once per tree, not per vertex
        auto instancing = [](Mesh& mesh, GLuint& vbo) {
            glBindVertexArray(mesh.vao);
            glGenBuffers(1, &vbo);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(glm::vec4), (void*)0);
            glEnableVertexAttribArray(4);
            glVertexAttribDivisor(4, 1);
            glBindVertexArray(0);
        };
        instancing(treeMesh, treeInstanceVBO);
        instancing(tree2Mesh, tree2InstanceVBO);
        uploadTreeInstances();
        std::cout << "Placed " << trees.size() << " trees\n";
    }

    void computeBounds() {
        glm::vec2 lo(1e9f), hi(-1e9f);
        for (BuildingInfo& b : buildings) {
            for (const glm::vec2& p : b.outer) { b.bmin = glm::min(b.bmin, p); b.bmax = glm::max(b.bmax, p); }
            b.area = std::abs(signedArea(b.outer));
            for (const Poly& h : b.holes) b.area -= std::abs(signedArea(h));
            if (b.inCampus) { lo = glm::min(lo, b.bmin); hi = glm::max(hi, b.bmax); }
        }
        for (const glm::vec2& p : boundary) { lo = glm::min(lo, p); hi = glm::max(hi, p); }
        if (lo.x < hi.x) { center = (lo + hi) * 0.5f; extent = hi - lo; }
    }

    // Colour scheme by building type, with a small per-building variation.
    glm::vec3 buildingColor(const BuildingInfo& b, size_t i) const {
        float jitter = 0.95f + 0.10f * float((i * 2654435761u) % 1000) / 1000.0f;
        if (style == 1)                                    // like the printed venue map
            return b.inCampus ? glm::vec3(0.88f, 0.88f, 0.42f) * jitter : glm::vec3(0.55f, 0.62f, 0.52f);
        glm::vec3 c;
        std::string n = b.name;
        for (char& ch : n) ch = (char)std::tolower((unsigned char)ch);
        if (!b.inCampus)                                   c = {0.74f, 0.73f, 0.70f};
        else if (b.type == "hostel" || n.find("hostel") != std::string::npos) c = {0.86f, 0.72f, 0.62f};
        else if (n.find("library") != std::string::npos)   c = {0.74f, 0.45f, 0.34f};
        else if (b.type == "construction")                 c = {0.72f, 0.72f, 0.70f};
        else if (n.rfind("rvu", 0) == 0)                   c = {0.84f, 0.86f, 0.90f};   // RV University blocks
        else if (b.type == "roof" || b.type == "garage")   c = {0.64f, 0.62f, 0.58f};
        else                                               c = {0.92f, 0.86f, 0.72f};   // cream plaster
        return c * jitter;
    }

    // Cut every vehicle road that crosses the campus wall into: outside part | gate (about 16 m) | inside part.
    // So "close the gate" closes only the gate, and the gate's inward direction is unambiguous.
    void splitAtGates() {
        if (boundary.size() < 3) return;
        for (int guard = 0; guard < 200; ++guard) {
            bool changed = false;
            for (size_t r = 0; r < roads.size() && !changed; ++r) {
                RoadInfo& rd = roads[r];
                if (!rd.vehicle || rd.gate) continue;
                for (size_t k = 0; k + 1 < rd.path.size() && !changed; ++k)
                    for (size_t e = 0; e < boundary.size() && !changed; ++e) {
                        glm::vec2 a = boundary[e], b = boundary[(e + 1) % boundary.size()];
                        glm::vec2 p = rd.path[k], q = rd.path[k + 1];
                        if (!segmentsCross(p, q, a, b)) continue;
                        glm::vec2 d1 = q - p, d2 = b - a;
                        float den = d1.x * d2.y - d1.y * d2.x;
                        if (std::abs(den) < 1e-6f) continue;
                        float t = ((a.x - p.x) * d2.y - (a.y - p.y) * d2.x) / den;
                        glm::vec2 x = p + d1 * t, dir = glm::normalize(d1);
                        glm::vec2 g0 = x - dir * std::min(8.0f, glm::distance(p, x) * 0.9f);
                        glm::vec2 g1 = x + dir * std::min(8.0f, glm::distance(q, x) * 0.9f);
                        RoadInfo before = rd, gate = rd, after = rd;
                        before.path.assign(rd.path.begin(), rd.path.begin() + k + 1); before.path.push_back(g0);
                        gate.path = {g0, g1}; gate.gate = true;
                        after.path = {g1}; after.path.insert(after.path.end(), rd.path.begin() + k + 1, rd.path.end());
                        std::vector<RoadInfo> parts;
                        if (glm::distance(before.path.front(), g0) > 0.5f || before.path.size() > 2) parts.push_back(before);
                        parts.push_back(gate);
                        if (glm::distance(after.path.back(), g1) > 0.5f || after.path.size() > 2) parts.push_back(after);
                        roads.erase(roads.begin() + r);
                        roads.insert(roads.begin() + r, parts.begin(), parts.end());
                        changed = true;
                    }
            }
            if (!changed) break;
        }
    }

    // Roads crossing the campus boundary = gates in the compound wall.
    void findGates() {
        gates.clear(); mainGate = -1;
        if (boundary.size() < 3) return;
        for (const RoadInfo& r : roads) {
            if (r.vehicle && !r.gate) continue;           // vehicle roads cross the wall only at their gate piece
            for (size_t k = 0; k + 1 < r.path.size(); ++k)
                for (size_t e = 0; e < boundary.size(); ++e) {
                    glm::vec2 a = boundary[e], b = boundary[(e + 1) % boundary.size()];
                    glm::vec2 p = r.path[k], q = r.path[k + 1];
                    if (!segmentsCross(p, q, a, b)) continue;
                    glm::vec2 d1 = q - p, d2 = b - a;
                    float den = d1.x * d2.y - d1.y * d2.x;
                    if (std::abs(den) < 1e-6f) continue;
                    float t = ((a.x - p.x) * d2.y - (a.y - p.y) * d2.x) / den;
                    gates.push_back({p + d1 * t, r.width, r.vehicle, int(&r - &roads[0])});
                }
        }
        // group vehicle gates that are within 30 m of each other
        gateGroups.clear(); mainGateGroup = -1;
        for (size_t i = 0; i < gates.size(); ++i) {
            if (!gates[i].vehicle) continue;
            for (size_t g = 0; g < gateGroups.size(); ++g)
                if (glm::distance(gateGroups[g].pos, gates[i].pos) < 30.0f) { gates[i].group = (int)g; break; }
            if (gates[i].group < 0) { gates[i].group = (int)gateGroups.size(); gateGroups.push_back({"", gates[i].pos, {}}); }
            gateGroups[gates[i].group].roads.push_back(gates[i].road);
        }
        // the main gate is the one closest to the main road (trunk / primary, e.g. Mysore Road)
        float best = 1e9f;
        for (size_t i = 0; i < gates.size(); ++i) {
            if (!gates[i].vehicle) continue;
            for (const RoadInfo& r : roads) {
                if (r.type != "trunk" && r.type != "trunk_link" && r.type != "primary" && r.type != "primary_link") continue;
                for (const glm::vec2& p : r.path) {
                    float d = glm::distance(p, gates[i].pos);
                    if (d < best) { best = d; mainGate = (int)i; mainGateGroup = gates[i].group; }
                }
            }
        }
        if (mainGate < 0)                      // no main road on the map: use the widest gate
            for (size_t i = 0; i < gates.size(); ++i) if (gates[i].vehicle && (mainGate < 0 || gates[i].width > gates[mainGate].width)) mainGate = (int)i;
        if (mainGateGroup < 0 && mainGate >= 0) mainGateGroup = gates[mainGate].group;
        // names: Main Gate, the others by compass direction from the campus centre (north = -z)
        for (size_t g = 0; g < gateGroups.size(); ++g) {
            if ((int)g == mainGateGroup) { gateGroups[g].name = "Main Gate"; continue; }
            glm::vec2 d = gateGroups[g].pos - center;
            std::string dir = std::abs(d.x) > std::abs(d.y) ? (d.x > 0 ? "East" : "West") : (d.y > 0 ? "South" : "North");
            std::string name = dir + " Gate";
            int same = 0;
            for (size_t h = 0; h < g; ++h) if (gateGroups[h].name.rfind(name, 0) == 0) same++;
            gateGroups[g].name = same ? name + " " + std::to_string(same + 1) : name;
        }
    }

    // A thin box from a to b (both faces, top, ends), used for the compound wall and pillars.
    static void wallBox(MeshBuilder& mb, glm::vec2 a, glm::vec2 b, float thick, float h, glm::vec4 col) {
        glm::vec2 d = b - a;
        float len = glm::length(d);
        if (len < 0.05f) return;
        d /= len;
        glm::vec2 s(-d.y * thick * 0.5f, d.x * thick * 0.5f);
        glm::vec2 c[4] = {a - s, b - s, b + s, a + s};
        glm::vec2 mid = (a + b) * 0.5f;
        for (int k = 0; k < 4; ++k) {
            glm::vec2 p = c[k], q = c[(k + 1) % 4], e = q - p;
            glm::vec3 n = glm::normalize(glm::vec3(e.y, 0, -e.x));
            if (glm::dot(glm::vec2(n.x, n.z), (p + q) * 0.5f - mid) < 0) n = -n;
            mb.quad({{p.x, 0, p.y}, n, {0, 500}, col}, {{q.x, 0, q.y}, n, {0, 500}, col},
                    {{q.x, h, q.y}, n, {0, 500}, col}, {{p.x, h, p.y}, n, {0, 500}, col});
        }
        glm::vec3 up(0, 1, 0);
        glm::vec4 cap(glm::vec3(col) * 0.85f, 0);
        mb.quad({{c[0].x, h, c[0].y}, up, {0, 500}, cap}, {{c[1].x, h, c[1].y}, up, {0, 500}, cap},
                {{c[2].x, h, c[2].y}, up, {0, 500}, cap}, {{c[3].x, h, c[3].y}, up, {0, 500}, cap});
    }

    // ---- rooftop details, as on most Bengaluru college blocks: a stair/lift headroom with black
    //      water tanks on top, and on some big roofs rows of south-facing solar panels.
    //      uv.y = 500 -> plain plaster, uv.y = 900 -> plain material (uv.x = 1: glossy solar glass)
    static float edgeDistance(glm::vec2 p, const Poly& ring) {
        float best = 1e9f;
        for (size_t k = 0; k < ring.size(); ++k) {
            glm::vec2 a = ring[k], b = ring[(k + 1) % ring.size()], ab = b - a;
            float t = glm::clamp(glm::dot(p - a, ab) / std::max(1e-6f, glm::dot(ab, ab)), 0.0f, 1.0f);
            best = std::min(best, glm::distance(p, a + t * ab));
        }
        return best;
    }
    bool onRoof(const BuildingInfo& b, glm::vec2 p, float margin) const {
        if (!pointInPolygon(p, b.outer) || edgeDistance(p, b.outer) < margin) return false;
        for (const Poly& h : b.holes) if (pointInPolygon(p, h) || edgeDistance(p, h) < margin) return false;
        return true;
    }
    static void box(MeshBuilder& mb, glm::vec2 c, glm::vec2 ax, float hx, float hz, float y0, float y1, glm::vec4 col, float vMark) {
        glm::vec2 az(-ax.y, ax.x);
        glm::vec2 q[4] = {c - ax * hx - az * hz, c + ax * hx - az * hz, c + ax * hx + az * hz, c - ax * hx + az * hz};
        for (int k = 0; k < 4; ++k) {
            glm::vec2 a = q[k], b = q[(k + 1) % 4], d = b - a;
            glm::vec3 n = glm::normalize(glm::vec3(d.y, 0, -d.x));
            if (glm::dot(glm::vec2(n.x, n.z), a - c) < 0) n = -n;
            mb.quad({{a.x, y0, a.y}, n, {0, vMark}, col}, {{b.x, y0, b.y}, n, {1, vMark}, col},
                    {{b.x, y1, b.y}, n, {1, vMark}, col}, {{a.x, y1, a.y}, n, {0, vMark}, col});
        }
        glm::vec3 up(0, 1, 0);
        mb.quad({{q[0].x, y1, q[0].y}, up, {q[0].x * 0.1f, q[0].y * 0.1f}, col}, {{q[1].x, y1, q[1].y}, up, {q[1].x * 0.1f, q[1].y * 0.1f}, col},
                {{q[2].x, y1, q[2].y}, up, {q[2].x * 0.1f, q[2].y * 0.1f}, col}, {{q[3].x, y1, q[3].y}, up, {q[3].x * 0.1f, q[3].y * 0.1f}, col});
    }
    static void cylinder(MeshBuilder& mb, glm::vec2 c, float r, float y0, float y1, glm::vec4 col) {
        const int N = 12;
        for (int k = 0; k < N; ++k) {
            float a0 = 6.2831853f * k / N, a1 = 6.2831853f * (k + 1) / N;
            glm::vec2 p0 = c + r * glm::vec2(std::cos(a0), std::sin(a0)), p1 = c + r * glm::vec2(std::cos(a1), std::sin(a1));
            glm::vec3 n0(std::cos(a0), 0, std::sin(a0)), n1(std::cos(a1), 0, std::sin(a1));
            mb.quad({{p0.x, y0, p0.y}, n0, {0, 900}, col}, {{p1.x, y0, p1.y}, n1, {0, 900}, col},
                    {{p1.x, y1, p1.y}, n1, {0, 900}, col}, {{p0.x, y1, p0.y}, n0, {0, 900}, col});
            mb.tri({{c.x, y1 + 0.12f, c.y}, {0, 1, 0}, {0, 900}, col}, {{p0.x, y1, p0.y}, {0, 1, 0}, {0, 900}, col},
                   {{p1.x, y1, p1.y}, {0, 1, 0}, {0, 900}, col});
        }
    }
    void rooftop(MeshBuilder& mb, const BuildingInfo& b, size_t i, glm::vec3 wallCol) {
        if (b.area < 250.0f) return;   // sheds and small blocks: no details
        unsigned seed = unsigned(i) * 2654435761u + 12345u;
        auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return float((seed >> 8) & 0xFFFF) / 65535.0f; };
        float H = b.height;
        glm::vec4 id(0, 0, 0, float(i + 1));
        // main axis = direction of the longest wall
        glm::vec2 ax(1, 0); float longest = 0;
        for (size_t k = 0; k < b.outer.size(); ++k) {
            glm::vec2 d = b.outer[(k + 1) % b.outer.size()] - b.outer[k];
            if (glm::length(d) > longest) { longest = glm::length(d); ax = glm::normalize(d); }
        }
        auto sample = [&](float margin, glm::vec2& out) {
            for (int tries = 0; tries < 60; ++tries) {
                glm::vec2 p(glm::mix(b.bmin.x, b.bmax.x, rnd()), glm::mix(b.bmin.y, b.bmax.y, rnd()));
                if (onRoof(b, p, margin)) { out = p; return true; }
            }
            return false;
        };
        // stair / lift headrooms (one per ~1500 m2, at most 3), each with 1-2 black water tanks on top
        std::vector<glm::vec2> rooms;
        int nRooms = std::min(3, 1 + int(b.area / 1500.0f));
        for (int r = 0; r < nRooms; ++r) {
            glm::vec2 p;
            if (!sample(4.0f, p)) break;
            bool clash = false;
            for (glm::vec2 q : rooms) if (glm::distance(p, q) < 15.0f) clash = true;
            if (clash) continue;
            rooms.push_back(p);
            glm::vec4 wc(wallCol, float(i + 1));
            float top = H + 3.0f;
            box(mb, p, ax, 2.4f, 1.8f, H, top, wc, 500.0f);
            // door: a dark panel just in front of one face
            glm::vec2 az(-ax.y, ax.x), dc = p + az * 1.82f;
            glm::vec3 dn(az.x, 0, az.y);
            glm::vec4 doorC(0.30f, 0.24f, 0.20f, float(i + 1));
            glm::vec2 d0 = dc - ax * 0.5f, d1 = dc + ax * 0.5f;
            mb.quad({{d0.x, H, d0.y}, dn, {0, 900}, doorC}, {{d1.x, H, d1.y}, dn, {0, 900}, doorC},
                    {{d1.x, H + 2.1f, d1.y}, dn, {0, 900}, doorC}, {{d0.x, H + 2.1f, d0.y}, dn, {0, 900}, doorC});
            glm::vec4 tank(0.07f, 0.07f, 0.08f, float(i + 1));
            int nTanks = 1 + int(rnd() * 2.0f);
            for (int t = 0; t < nTanks; ++t)
                cylinder(mb, p + ax * (nTanks == 1 ? 0.0f : (t == 0 ? -1.1f : 1.1f)), 0.75f, top, top + 1.5f, tank);
        }
        // solar panels on some large roofs: rows facing south (+z), tilted about 13 degrees (Bengaluru's latitude)
        if (b.area > 1000.0f && rnd() < 0.45f) {
            glm::vec4 panel(0.10f, 0.16f, 0.30f, float(i + 1));
            glm::vec4 frame(0.55f, 0.56f, 0.58f, float(i + 1));
            const float w = 1.0f, depth = 1.7f, tilt = glm::radians(13.0f);
            // rows run along the building; panels tilt towards whichever side of the building faces south (+z)
            glm::vec2 az(-ax.y, ax.x);
            glm::vec2 f = std::abs(az.y) >= std::abs(ax.y) ? (az.y >= 0 ? az : -az) : (ax.y >= 0 ? ax : -ax);
            glm::vec2 r(-f.y, f.x);
            float rise = depth * std::sin(tilt), run = depth * std::cos(tilt);
            glm::vec3 n = glm::normalize(glm::vec3(f.x * std::sin(tilt), std::cos(tilt), f.y * std::sin(tilt)));
            float r0 = 1e9f, r1 = -1e9f, f0 = 1e9f, f1 = -1e9f;
            for (glm::vec2 q : b.outer) {
                r0 = std::min(r0, glm::dot(q, r)); r1 = std::max(r1, glm::dot(q, r));
                f0 = std::min(f0, glm::dot(q, f)); f1 = std::max(f1, glm::dot(q, f));
            }
            int count = 0;
            for (float fv = f0 + 3.0f; fv < f1 - 3.0f && count < 200; fv += 3.4f)
                for (float rv = r0 + 3.0f; rv < r1 - 3.0f && count < 200; rv += w + 0.1f) {
                    glm::vec2 c = r * rv + f * fv;               // back (high) corner
                    glm::vec2 c1 = c + r * w, c2 = c1 + f * run, c3 = c + f * run;
                    if (!onRoof(b, c, 2.5f) || !onRoof(b, c2, 2.5f)) continue;
                    bool nearRoom = false;
                    for (glm::vec2 q : rooms) if (glm::distance(c, q) < 6.0f || glm::distance(c2, q) < 6.0f) nearRoom = true;
                    if (nearRoom) continue;
                    float y0 = H + 0.4f;
                    mb.quad({{c.x, y0 + rise, c.y}, n, {1, 900}, panel}, {{c1.x, y0 + rise, c1.y}, n, {1, 900}, panel},
                            {{c2.x, y0, c2.y}, n, {1, 900}, panel}, {{c3.x, y0, c3.y}, n, {1, 900}, panel});
                    glm::vec3 bn(-f.x, 0, -f.y);                // back support
                    mb.quad({{c.x, H, c.y}, bn, {0, 900}, frame}, {{c1.x, H, c1.y}, bn, {0, 900}, frame},
                            {{c1.x, y0 + rise, c1.y}, bn, {0, 900}, frame}, {{c.x, y0 + rise, c.y}, bn, {0, 900}, frame});
                    ++count;
                }
        }
        // entrance porch on the middle of the longest wall: canopy slab on two pillars, glazed double door
        for (size_t k = 0; k < b.outer.size(); ++k) {
            glm::vec2 p0 = b.outer[k], p1 = b.outer[(k + 1) % b.outer.size()], d = p1 - p0;
            if (std::abs(glm::length(d) - longest) > 1e-3f || longest < 12.0f) continue;
            glm::vec2 m = (p0 + p1) * 0.5f, n = glm::normalize(glm::vec2(d.y, -d.x)), t = glm::normalize(d);
            glm::vec4 wc(wallCol * 0.95f, float(i + 1));
            box(mb, m + n * 1.5f, t, 3.6f, 1.5f, 3.2f, 3.5f, wc, 500.0f);                  // canopy
            for (float sgn : {-1.0f, 1.0f})
                box(mb, m + n * 2.7f + t * (3.2f * sgn), t, 0.18f, 0.18f, 0.0f, 3.2f, wc, 500.0f);   // pillars
            box(mb, m + n * 1.5f, t, 3.4f, 1.4f, 0.0f, 0.15f, glm::vec4(0.55f, 0.52f, 0.48f, float(i + 1)), 500.0f); // step
            glm::vec3 nn(n.x, 0, n.y);
            glm::vec2 a0 = m + n * 0.04f - t * 1.2f, a1 = m + n * 0.04f + t * 1.2f;
            glm::vec4 glass(0.16f, 0.22f, 0.28f, float(i + 1));
            mb.quad({{a0.x, 0.15f, a0.y}, nn, {1, 900}, glass}, {{a1.x, 0.15f, a1.y}, nn, {1, 900}, glass},
                    {{a1.x, 2.6f, a1.y}, nn, {1, 900}, glass}, {{a0.x, 2.6f, a0.y}, nn, {1, 900}, glass});
            break;
        }
        (void)id;
    }

    void buildBuildings() {
        MeshBuilder campusMB, outsideMB;
        for (size_t i = 0; i < buildings.size(); ++i) {
            BuildingInfo& b = buildings[i];
            MeshBuilder& mb = b.inCampus ? campusMB : outsideMB;
            // orientation: outer ring counter-clockwise, holes clockwise -> one normal formula for every wall
            if (signedArea(b.outer) < 0) std::reverse(b.outer.begin(), b.outer.end());
            for (Poly& h : b.holes) if (signedArea(h) > 0) std::reverse(h.begin(), h.end());

            glm::vec3 col = buildingColor(b, i);
            glm::vec4 c(col, float(i + 1));
            float H = b.height;
            float P = (b.type == "roof" || b.type == "garage") ? 0.0f : 0.9f;    // roof parapet height

            // ---- walls: one quad per footprint edge (extrusion), plus the parapet above the roof line
            auto walls = [&](const Poly& ring) {
                float u = 0;
                for (size_t k = 0; k < ring.size(); ++k) {
                    glm::vec2 a = ring[k], bb = ring[(k + 1) % ring.size()];
                    glm::vec2 d = bb - a;
                    float len = glm::length(d);
                    if (len < 0.01f) continue;
                    glm::vec3 n = glm::normalize(glm::vec3(d.y, 0, -d.x));   // outward
                    float u0 = u / 4.0f, u1 = (u + len) / 4.0f;              // one window every 4 m
                    float v1 = H / 3.5f;                                     // one floor every 3.5 m
                    mb.quad({{a.x, 0, a.y}, n, {u0, 0}, c}, {{bb.x, 0, bb.y}, n, {u1, 0}, c},
                            {{bb.x, H, bb.y}, n, {u1, v1}, c}, {{a.x, H, a.y}, n, {u0, v1}, c});
                    if (P > 0) {
                        // parapet: outside face + inside face (v = 500 marks "plain plaster, no windows")
                        mb.quad({{a.x, H, a.y}, n, {u0, 500}, c}, {{bb.x, H, bb.y}, n, {u1, 500}, c},
                                {{bb.x, H + P, bb.y}, n, {u1, 500.3f}, c}, {{a.x, H + P, a.y}, n, {u0, 500.3f}, c});
                        mb.quad({{a.x, H, a.y}, -n, {u0, 500}, c}, {{bb.x, H, bb.y}, -n, {u1, 500}, c},
                                {{bb.x, H + P, bb.y}, -n, {u1, 500.3f}, c}, {{a.x, H + P, a.y}, -n, {u0, 500.3f}, c});
                    }
                    u += len;
                }
            };
            walls(b.outer);
            for (const Poly& h : b.holes) walls(h);

            // ---- flat roof: ear-clipping triangulation of the footprint (with courtyards)
            // natural look: weathered grey concrete (RCC roofs), not the wall colour
            glm::vec3 concrete(0.60f, 0.59f, 0.56f);
            glm::vec4 roofC(style == 1 ? col : glm::mix(col * 0.8f, concrete, 0.75f), float(i + 1));
            std::vector<glm::vec2> t = triangulate(b.outer, b.holes);
            for (size_t k = 0; k + 2 < t.size(); k += 3) {
                Vertex v[3];
                for (int m = 0; m < 3; ++m) v[m] = {{t[k + m].x, H, t[k + m].y}, {0, 1, 0}, t[k + m] * 0.1f, roofC};
                mb.tri(v[0], v[1], v[2]);
            }
            if (b.inCampus && P > 0 && style == 0) rooftop(mb, b, i, col);
        }

        // ---- compound wall (2.2 m) along the boundary, open where roads cross it, pillars at each gate
        wallPieces.clear();
        if (boundary.size() >= 3) {
            glm::vec4 wallC(style == 1 ? glm::vec3(0.95f, 0.95f, 0.90f) : glm::vec3(0.88f, 0.83f, 0.72f), 0);
            glm::vec4 pillarC(style == 1 ? glm::vec3(0.98f) : glm::vec3(0.72f, 0.42f, 0.32f), 0);   // brick-red gate pillars
            for (size_t e = 0; e < boundary.size(); ++e) {
                glm::vec2 a = boundary[e], b = boundary[(e + 1) % boundary.size()];
                float len = glm::distance(a, b);
                if (len < 0.1f) continue;
                std::vector<std::pair<float, float>> gaps;          // in metres along this edge
                for (const Gate& g : gates) {
                    glm::vec2 ab = b - a;
                    float t = glm::dot(g.pos - a, ab) / (len * len);
                    if (t < -0.01f || t > 1.01f) continue;
                    if (glm::distance(a + ab * t, g.pos) > 0.5f) continue;
                    float half = g.width * 0.5f + 1.5f;
                    gaps.push_back({t * len - half, t * len + half});
                }
                std::sort(gaps.begin(), gaps.end());
                float pos = 0;
                glm::vec2 dir = (b - a) / len;
                auto piece = [&](float s0, float s1) {
                    if (s1 - s0 < 0.3f) return;
                    glm::vec2 p = a + dir * s0, q = a + dir * s1;
                    wallBox(campusMB, p, q, 0.35f, 2.2f, wallC);
                    wallPieces.push_back({p, q});
                };
                for (auto& g : gaps) {
                    piece(pos, std::max(pos, g.first));
                    for (float s0 : {g.first, g.second})            // gate pillars
                        if (s0 > 0 && s0 < len) {
                            glm::vec2 c0 = a + dir * s0;
                            wallBox(campusMB, c0 - dir * 0.4f, c0 + dir * 0.4f, 0.8f, 2.9f, pillarC);
                        }
                    pos = std::max(pos, g.second);
                }
                piece(pos, len);
            }
        }
        buildingMesh.upload(campusMB.v);
        outsideMesh.upload(outsideMB.v);
    }

    static void addDisc(MeshBuilder& mb, glm::vec2 c, float r, float y, glm::vec4 col, float u) {
        const int N = 12;
        glm::vec3 n(0, 1, 0);
        for (int k = 0; k < N; ++k) {
            float a0 = k * 6.2831853f / N, a1 = (k + 1) * 6.2831853f / N;
            mb.tri({{c.x, y, c.y}, n, {u, 0}, col},
                   {{c.x + r * std::cos(a0), y, c.y + r * std::sin(a0)}, n, {u, 0}, col},
                   {{c.x + r * std::cos(a1), y, c.y + r * std::sin(a1)}, n, {u, 0}, col});
        }
    }

public:
    void buildRoads() {
        MeshBuilder car, foot;
        for (const RoadInfo& r : roads) {
            MeshBuilder& mb = r.vehicle ? car : foot;
            float y = r.vehicle ? 0.06f : 0.04f;
            glm::vec4 col = r.vehicle ? glm::vec4(0.32f, 0.32f, 0.34f, 0) : glm::vec4(0.76f, 0.70f, 0.60f, 0);
            if (style == 1) col = r.vehicle ? glm::vec4(0.95f, 0.95f, 0.93f, 0) : glm::vec4(0.80f, 0.86f, 0.74f, 0);
            bool centreLine = r.vehicle && r.width >= 6.0f;
            float hw = r.width * 0.5f, u = 0;
            glm::vec3 n(0, 1, 0);
            for (size_t k = 0; k + 1 < r.path.size(); ++k) {
                glm::vec2 a = r.path[k], b = r.path[k + 1], d = b - a;
                float len = glm::length(d);
                if (len < 0.01f) continue;
                glm::vec2 side = glm::vec2(-d.y, d.x) / len * hw;
                float u0 = u / 6.0f, u1 = (u + len) / 6.0f;   // dash every 6 m
                float v0 = 0.0f, v1 = centreLine ? 1.0f : 0.0f;   // v runs across the road (0 = no centre line)
                mb.quad({{a.x - side.x, y, a.y - side.y}, n, {u0, v0}, col},
                        {{b.x - side.x, y, b.y - side.y}, n, {u1, v0}, col},
                        {{b.x + side.x, y, b.y + side.y}, n, {u1, v1}, col},
                        {{a.x + side.x, y, a.y + side.y}, n, {u0, v1}, col});
                u += len;
            }
            // round joints at every node fill the gaps at bends and junctions
            for (const glm::vec2& p : r.path) addDisc(mb, p, hw, y - 0.005f, col, 0);
        }
        roadMesh.upload(car.v);
        pathMesh.upload(foot.v);
    }
private:

    void buildAreas() {
        MeshBuilder mb;
        for (const AreaInfo& a : areas) {
            glm::vec4 col = style == 1 ? glm::vec4(0.26f, 0.50f, 0.34f, 0) : glm::vec4(0.42f, 0.62f, 0.32f, 0);
            if (a.type == "pitch")   col = {0.30f, 0.56f, 0.28f, 0};
            if (a.type == "scrub")   col = {0.48f, 0.56f, 0.34f, 0};
            if (a.type == "water")   col = {0.22f, 0.42f, 0.62f, 0};
            if (a.type == "parking") col = {0.50f, 0.50f, 0.50f, 0};
            float y = a.type == "parking" ? 0.025f : 0.02f;
            std::vector<glm::vec2> t = triangulate(a.outer, {});
            for (size_t k = 0; k + 2 < t.size(); k += 3) {
                Vertex v[3];
                for (int m = 0; m < 3; ++m) v[m] = {{t[k + m].x, y, t[k + m].y}, {0, 1, 0}, t[k + m] * 0.1f, col};
                mb.tri(v[0], v[1], v[2]);
            }
        }
        areaMesh.upload(mb.v);
    }
};
