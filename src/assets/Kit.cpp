#include "assets/Kit.h"

#include <algorithm>
#include <cfloat>
#include <format>

#include <cgltf.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace bh {
namespace {

void grow(Vec3& mn, Vec3& mx, Vec3 p) {
    mn = {std::min(mn.x, p.x), std::min(mn.y, p.y), std::min(mn.z, p.z)};
    mx = {std::max(mx.x, p.x), std::max(mx.y, p.y), std::max(mx.z, p.z)};
}

Vec3 xform(const float m[16], Vec3 p, float w) {
    // cgltf matrices are column-major
    return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12] * w, m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13] * w,
            m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14] * w};
}

KitMaterial convertMaterial(const cgltf_material* m) {
    KitMaterial k;
    if (!m) {
        k.name = "default";
        return k;
    }
    k.name = m->name ? m->name : "unnamed";
    if (m->has_pbr_metallic_roughness) {
        const auto& p = m->pbr_metallic_roughness;
        for (int i = 0; i < 3; ++i) k.baseColor[i] = p.base_color_factor[i];
        k.alpha = p.base_color_factor[3];
        k.metallic = p.metallic_factor;
        k.roughness = p.roughness_factor;
    }
    float strength = m->has_emissive_strength ? m->emissive_strength.emissive_strength : 1.0f;
    for (int i = 0; i < 3; ++i) k.emissive[i] = m->emissive_factor[i] * strength;
    if (m->alpha_mode != cgltf_alpha_mode_blend) k.alpha = 1.0f;
    return k;
}

}  // namespace

uint32_t Kit::materialIndex(const KitMaterial& m) {
    for (size_t i = 0; i < materials_.size(); ++i)
        if (materials_[i].name == m.name) return uint32_t(i);
    materials_.push_back(m);
    return uint32_t(materials_.size() - 1);
}

int Kit::find(std::string_view name) const {
    for (size_t i = 0; i < models_.size(); ++i)
        if (models_[i].name == name) return int(i);
    return -1;
}

bool Kit::loadDirectory(const std::filesystem::path& dir, std::string& error) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        error = std::format("assets directory not found: {}", dir.string());
        return false;
    }
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(dir))
        if (e.is_regular_file() && e.path().extension() == ".glb") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        error = std::format("no .glb files in {}", dir.string());
        return false;
    }
    for (const auto& f : files)
        if (!loadModel(f, error)) return false;
    return true;
}

bool Kit::loadModel(const std::filesystem::path& file, std::string& error) {
    cgltf_options options{};
    cgltf_data* data = nullptr;
    const std::string path = file.string();
    if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success) {
        error = std::format("cannot parse {}", path);
        return false;
    }
    if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success) {
        cgltf_free(data);
        error = std::format("cannot load buffers of {}", path);
        return false;
    }

    KitModel model;
    model.name = file.stem().string();
    model.boundsMin = {FLT_MAX, FLT_MAX, FLT_MAX};
    model.boundsMax = {-FLT_MAX, -FLT_MAX, -FLT_MAX};

    for (cgltf_size n = 0; n < data->nodes_count; ++n) {
        const cgltf_node* node = &data->nodes[n];
        float world[16];
        cgltf_node_transform_world(node, world);

        if (node->light && node->light->type == cgltf_light_type_point) {
            KitLight l;
            l.position = xform(world, {0, 0, 0}, 1.0f);
            l.color = {node->light->color[0], node->light->color[1], node->light->color[2]};
            // Blender exports watts as candela (W / 4pi * 683): back to W/sr.
            l.intensity = node->light->intensity / 683.0f;
            l.radius = model.name == "room_ceiling" ? 0.4f : 0.15f;
            model.lights.push_back(l);
        }
        if (!node->mesh) continue;

        for (int pass = 0; pass < 2; ++pass) {  // 0: opaque primitives, 1: transparent
            KitPart part;
            part.name = node->name ? node->name : model.name;
            part.transparent = pass == 1;
            part.firstSubmesh = uint32_t(submeshes_.size());
            part.boundsMin = {FLT_MAX, FLT_MAX, FLT_MAX};
            part.boundsMax = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
            for (cgltf_size p = 0; p < node->mesh->primitives_count; ++p) {
                const cgltf_primitive& prim = node->mesh->primitives[p];
                if (prim.type != cgltf_primitive_type_triangles || !prim.indices) continue;
                KitMaterial mat = convertMaterial(prim.material);
                if ((mat.alpha < 1.0f) != (pass == 1)) continue;
                const cgltf_accessor* pos = nullptr;
                const cgltf_accessor* nrm = nullptr;
                for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
                    if (prim.attributes[a].type == cgltf_attribute_type_position) pos = prim.attributes[a].data;
                    if (prim.attributes[a].type == cgltf_attribute_type_normal) nrm = prim.attributes[a].data;
                }
                if (!pos) continue;
                KitSubmesh sm;
                sm.baseVertex = uint32_t(vertices_.size());
                sm.vertexCount = uint32_t(pos->count);
                sm.firstIndex = uint32_t(indices_.size());
                sm.indexCount = uint32_t(prim.indices->count);
                sm.material = materialIndex(mat);
                for (cgltf_size v = 0; v < pos->count; ++v) {
                    float pp[3] = {0, 0, 0}, nn[3] = {0, 1, 0};
                    cgltf_accessor_read_float(pos, v, pp, 3);
                    if (nrm) cgltf_accessor_read_float(nrm, v, nn, 3);
                    Vec3 wp = xform(world, {pp[0], pp[1], pp[2]}, 1.0f);
                    Vec3 wn = normalize(xform(world, {nn[0], nn[1], nn[2]}, 0.0f));
                    vertices_.push_back({wp.x, wp.y, wp.z, wn.x, wn.y, wn.z});
                    grow(part.boundsMin, part.boundsMax, wp);
                }
                for (cgltf_size i = 0; i < prim.indices->count; ++i)
                    indices_.push_back(uint32_t(cgltf_accessor_read_index(prim.indices, i)));
                model.triangles += sm.indexCount / 3;
                submeshes_.push_back(sm);
            }
            part.submeshCount = uint32_t(submeshes_.size()) - part.firstSubmesh;
            if (part.submeshCount == 0) continue;
            grow(model.boundsMin, model.boundsMax, part.boundsMin);
            grow(model.boundsMin, model.boundsMax, part.boundsMax);
            model.parts.push_back(uint32_t(parts_.size()));
            parts_.push_back(part);
        }
    }
    cgltf_free(data);
    if (model.parts.empty()) {
        error = std::format("{} has no meshes", path);
        return false;
    }
    models_.push_back(std::move(model));
    return true;
}

std::filesystem::path findAssetsDirectory(const std::filesystem::path& override_) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!override_.empty()) return override_;
    auto ok = [&](const fs::path& p) { return fs::exists(p / "kit_manifest.json", ec); };
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    if (GetModuleFileNameW(nullptr, buf, MAX_PATH)) {
        fs::path dir = fs::path(buf).parent_path();
        for (int i = 0; i < 6 && !dir.empty(); ++i) {
            if (ok(dir / "assets")) return dir / "assets";
            if (dir == dir.parent_path()) break;
            dir = dir.parent_path();
        }
    }
#endif
    if (ok(fs::current_path(ec) / "assets")) return fs::current_path(ec) / "assets";
#ifdef BULKHEAD_SOURCE_DIR
    if (ok(fs::path(BULKHEAD_SOURCE_DIR) / "assets")) return fs::path(BULKHEAD_SOURCE_DIR) / "assets";
#endif
    return "assets";
}

}  // namespace bh
