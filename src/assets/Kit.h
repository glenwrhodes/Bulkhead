#pragma once
// The Blender kit, loaded from assets/*.glb (cgltf). Geometry from every model is
// flattened into one shared vertex/index pool. A model is made of parts (one per glTF
// node, split again into opaque and transparent geometry); a part is the unit the
// renderer builds an acceleration structure for and instances in the world.
// No D3D12 here: unit tests load the real kit.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/Math.h"

namespace bh {

struct KitVertex {
    float px, py, pz;
    float nx, ny, nz;
};

struct KitMaterial {
    std::string name;
    float baseColor[3] = {0.8f, 0.8f, 0.8f};
    float alpha = 1.0f;
    float metallic = 0.0f;
    float roughness = 0.5f;
    float emissive[3] = {0, 0, 0};  // radiance (colour x strength)
};

struct KitSubmesh {
    uint32_t firstIndex = 0;   // into Kit::indices
    uint32_t indexCount = 0;
    uint32_t baseVertex = 0;   // indices are relative to this vertex
    uint32_t vertexCount = 0;
    uint32_t material = 0;
};

struct KitPart {
    std::string name;          // glTF node name, e.g. "door_sliding_LeafL"
    uint32_t firstSubmesh = 0;
    uint32_t submeshCount = 0;
    bool transparent = false;  // alpha-blended materials (glass)
    Vec3 boundsMin, boundsMax;
};

struct KitLight {
    Vec3 position;             // model space
    Vec3 color{1, 1, 1};
    float intensity = 0.0f;    // radiant intensity, W/sr
    float radius = 0.15f;
};

// An effect anchor from Blender: an empty named <model>_fx_<name> (sparks, steam, the
// muzzle). direction is the empty's Blender +Y axis, in model space.
struct KitMarker {
    std::string name;          // the part after "_fx_", e.g. "spark0", "steam", "muzzle"
    Vec3 position;
    Vec3 direction{0, 0, -1};
};

struct KitModel {
    std::string name;          // file stem, e.g. "corridor_straight"
    std::vector<uint32_t> parts;
    std::vector<KitLight> lights;
    std::vector<KitMarker> markers;
    Vec3 boundsMin, boundsMax;
    uint32_t triangles = 0;
};

class Kit {
public:
    // Loads every .glb in the directory. Returns false and fills error on failure.
    bool loadDirectory(const std::filesystem::path& dir, std::string& error);
    bool loadModel(const std::filesystem::path& file, std::string& error);

    int find(std::string_view modelName) const;  // -1 if missing
    const KitModel& model(int i) const { return models_[size_t(i)]; }

    const std::vector<KitVertex>& vertices() const { return vertices_; }
    const std::vector<uint32_t>& indices() const { return indices_; }
    const std::vector<KitSubmesh>& submeshes() const { return submeshes_; }
    const std::vector<KitPart>& parts() const { return parts_; }
    const std::vector<KitModel>& models() const { return models_; }
    const std::vector<KitMaterial>& materials() const { return materials_; }

private:
    uint32_t materialIndex(const KitMaterial& m);

    std::vector<KitVertex> vertices_;
    std::vector<uint32_t> indices_;
    std::vector<KitSubmesh> submeshes_;
    std::vector<KitPart> parts_;
    std::vector<KitModel> models_;
    std::vector<KitMaterial> materials_;
};

// Finds the assets directory: --assets override, then upward from the executable,
// then the source tree the program was built from.
std::filesystem::path findAssetsDirectory(const std::filesystem::path& override_);

}  // namespace bh
