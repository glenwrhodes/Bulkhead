#include "game/CrawlOverlay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <stb_truetype.h>

#include "game/Creature.h"
#include "world/CrawlSurface.h"

namespace bh {

namespace {

struct P {
    float x = 0.0f, y = 0.0f;
};

// ---- fonts: a plain sans from Windows (Segoe UI; Arial if it is missing) ----
struct Font {
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
    bool ok = false;
};

Font loadFont(const char* const* paths) {
    Font f;
    for (const char* const* p = paths; *p; ++p) {
        FILE* fp = nullptr;
        if (fopen_s(&fp, *p, "rb") != 0 || !fp) continue;
        std::fseek(fp, 0, SEEK_END);
        const long n = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);
        f.data.resize(size_t(std::max(0L, n)));
        const size_t got = std::fread(f.data.data(), 1, f.data.size(), fp);
        std::fclose(fp);
        if (got == f.data.size() && stbtt_InitFont(&f.info, f.data.data(), stbtt_GetFontOffsetForIndex(f.data.data(), 0))) {
            f.ok = true;
            return f;
        }
    }
    return f;
}

const Font& font(bool bold) {
    static const char* const kRegular[] = {"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf", nullptr};
    static const char* const kBold[] = {"C:/Windows/Fonts/seguisb.ttf", "C:/Windows/Fonts/segoeuib.ttf",
                                        "C:/Windows/Fonts/arialbd.ttf", nullptr};
    static const Font regular = loadFont(kRegular), semibold = loadFont(kBold);
    return bold && semibold.ok ? semibold : regular;
}

float textWidth(const Font& f, float px, const std::string& s) {
    if (!f.ok) return 0.0f;
    const float scale = stbtt_ScaleForPixelHeight(&f.info, px);
    float w = 0.0f;
    for (size_t i = 0; i < s.size(); ++i) {
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&f.info, s[i], &adv, &lsb);
        w += float(adv) * scale;
        if (i + 1 < s.size()) w += float(stbtt_GetCodepointKernAdvance(&f.info, s[i], s[i + 1])) * scale;
    }
    return w;
}

// Text with its baseline at y, a soft dark shadow under it.
void text(Image& img, float x, float y, float px, const std::string& s, Color c, bool bold = false) {
    const Font& f = font(bold);
    if (!f.ok) return;
    const float scale = stbtt_ScaleForPixelHeight(&f.info, px);
    for (int pass = 0; pass < 2; ++pass) {
        const Color col = pass == 0 ? Color{0, 0, 0, 170} : c;
        const float off = pass == 0 ? std::max(1.0f, px * 0.07f) : 0.0f;
        float pen = x;
        for (size_t i = 0; i < s.size(); ++i) {
            int w = 0, h = 0, xo = 0, yo = 0;
            unsigned char* bmp = stbtt_GetCodepointBitmap(&f.info, 0, scale, s[i], &w, &h, &xo, &yo);
            if (bmp) {
                for (int j = 0; j < h; ++j)
                    for (int k = 0; k < w; ++k)
                        if (bmp[j * w + k])
                            img.blend(int(pen + off) + xo + k, int(y + off) + yo + j, col, float(bmp[j * w + k]) / 255.0f);
                stbtt_FreeBitmap(bmp, nullptr);
            }
            int adv = 0, lsb = 0;
            stbtt_GetCodepointHMetrics(&f.info, s[i], &adv, &lsb);
            pen += float(adv) * scale;
            if (i + 1 < s.size()) pen += float(stbtt_GetCodepointKernAdvance(&f.info, s[i], s[i + 1])) * scale;
        }
    }
}

// ---- drawing ----
void line(Image& img, P a, P b, float w, Color c) {
    const float pad = w * 0.5f + 1.5f;
    const int x0 = std::max(0, int(std::floor(std::min(a.x, b.x) - pad)));
    const int x1 = std::min(img.width() - 1, int(std::ceil(std::max(a.x, b.x) + pad)));
    const int y0 = std::max(0, int(std::floor(std::min(a.y, b.y) - pad)));
    const int y1 = std::min(img.height() - 1, int(std::ceil(std::max(a.y, b.y) + pad)));
    const float abx = b.x - a.x, aby = b.y - a.y, L2 = std::max(1e-6f, abx * abx + aby * aby);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const float px = float(x) + 0.5f - a.x, py = float(y) + 0.5f - a.y;
            const float t = std::clamp((px * abx + py * aby) / L2, 0.0f, 1.0f);
            const float dx = px - abx * t, dy = py - aby * t;
            const float cov = std::clamp(w * 0.5f + 0.5f - std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f);
            if (cov > 0.0f) img.blend(x, y, c, cov);
        }
}

struct View {
    Camera cam;
    int W, H;
    float tx, ty;
    bool project(Vec3 p, P& out) const {
        const Vec3 d = p - cam.position;
        const float z = dot(d, cam.forward());
        if (z < 0.05f) return false;
        out = {(dot(d, cam.right()) / (z * tx) * 0.5f + 0.5f) * float(W), (0.5f - dot(d, cam.up()) / (z * ty) * 0.5f) * float(H)};
        return true;
    }
    // pixels per metre at a point (for rings that keep their size in the world)
    float scaleAt(Vec3 p) const {
        const float z = std::max(0.05f, dot(p - cam.position, cam.forward()));
        return float(H) * 0.5f / (z * ty);
    }
};

void line3(Image& img, const View& v, Vec3 a, Vec3 b, float w, Color c) {
    P pa, pb;
    if (v.project(a, pa) && v.project(b, pb)) line(img, pa, pb, w, c);
}

void polyline3(Image& img, const View& v, const std::vector<Vec3>& pts, float w, Color c, bool dashed = false) {
    for (size_t i = 0; i + 1 < pts.size(); ++i)
        if (!dashed || i % 2 == 0) line3(img, v, pts[i], pts[i + 1], w, c);
}

Color alpha(Color c, uint8_t a) { return Color{c.r, c.g, c.b, a}; }

struct Box {
    float x0, y0, x1, y1;
    bool hits(const Box& o) const { return x0 < o.x1 && o.x0 < x1 && y0 < o.y1 && o.y0 < y1; }
};

// A label with a leader line from the anchor, pushed away from the middle of the frame.
// `taken` holds the boxes already used this frame (the panels and earlier labels); the
// label slides up or down until it is clear of all of them.
void label(Image& img, std::vector<Box>& taken, P anchor, const std::string& s, Color c, float k, float push = 90.0f) {
    const float W = float(img.width()), H = float(img.height());
    if (anchor.x < 0.0f || anchor.x > W || anchor.y < 0.0f || anchor.y > H) return;  // (its thing is off screen)
    const float cx = W * 0.5f, cy = H * 0.5f;
    float dx = anchor.x - cx, dy = anchor.y - cy;
    const float dl = std::max(1.0f, std::sqrt(dx * dx + dy * dy));
    dx /= dl, dy /= dl;
    const float px = 26.0f * k;
    const float w = textWidth(font(true), px, s);
    const float baseY = anchor.y + dy * push * k - 20.0f * k;
    float txc = std::clamp(dx < 0.0f ? anchor.x + dx * push * k - w : anchor.x + dx * push * k, 12.0f * k, W - w - 12.0f * k);
    auto boxAt = [&](float y) { return Box{txc - 10 * k, y - px * 0.95f - 4 * k, txc + w + 10 * k, y + px * 0.3f + 4 * k}; };
    float y = std::clamp(baseY, 40.0f * k, H - 20.0f * k);
    for (int i = 0; i < 24; ++i) {
        const float off = float((i + 1) / 2) * 34.0f * k * (i % 2 ? 1.0f : -1.0f);
        const float yy = std::clamp(baseY + off, 40.0f * k, H - 20.0f * k);
        bool clear = true;
        for (const Box& b : taken) clear = clear && !boxAt(yy).hits(b);
        if (clear) {
            y = yy;
            break;
        }
        if (i == 23) {  // nowhere free in this column: try the other side of the anchor
            txc = std::clamp(dx < 0.0f ? anchor.x + 40.0f * k : anchor.x - 40.0f * k - w, 12.0f * k, W - w - 12.0f * k);
            y = std::clamp(baseY, 40.0f * k, H - 20.0f * k);
        }
    }
    taken.push_back(boxAt(y));
    const float ex = std::abs(anchor.x - txc) < std::abs(anchor.x - (txc + w)) ? txc : txc + w;
    line(img, anchor, P{ex, y - px * 0.3f}, 2.0f * k, alpha(c, 220));
    img.fillRect(int(txc - 6 * k), int(y - px * 0.95f), int(txc + w + 6 * k), int(y + px * 0.3f), Color{8, 10, 14, 150});
    text(img, txc, y, px, s, c, true);
}

}  // namespace

void drawCrawlOverlay(Image& img, const CrawlOverlayInput& in) {
    if (!in.creature || !in.path || !in.uOf) return;
    const Creature& cr = *in.creature;
    const CrawlPath& path = *in.path;
    View v{in.camera, img.width(), img.height(), 0.0f, 0.0f};
    v.ty = std::tan(in.camera.fovY * 0.5f);
    v.tx = v.ty * float(img.width()) / float(img.height());
    const float k = float(img.height()) / 1080.0f;
    const Color legCol[4] = {rgb(0x46dcff), rgb(0xffb84a), rgb(0x7dff8a), rgb(0xff6ad5)};
    const char* legName[4] = {"left middle", "right middle", "left hind", "right hind"};
    const Color white = rgb(0xffffff);

    // ---- the surface it is attached to: a strip round the cross-section where it crawls ----
    const float sC = in.sBody;
    {
        const Color sc = Color{235, 240, 245, 90};
        for (float du : {-0.6f, 0.0f, 0.6f}) {
            std::vector<Vec3> pts;
            for (float s = sC - 2.0f; s <= sC + 2.0f; s += 0.12f) {
                const float u = in.uOf(s) + du;
                pts.push_back(path.point(s, u) + path.normal(s, u) * 0.01f);
            }
            polyline3(img, v, pts, du == 0.0f ? 1.2f * k : 2.0f * k, sc, du == 0.0f);
        }
        for (float s = std::floor((sC - 2.0f) / 0.45f) * 0.45f; s <= sC + 2.0f; s += 0.45f) {
            if (s < sC - 2.0f) continue;
            std::vector<Vec3> pts;
            for (float du = -0.6f; du <= 0.6f + 1e-3f; du += 0.2f) {
                const float u = in.uOf(s) + du;
                pts.push_back(path.point(s, u) + path.normal(s, u) * 0.01f);
            }
            polyline3(img, v, pts, 1.2f * k, sc);
        }
    }
    // ---- which way is up for it: the surface normal under its middle ----
    const Vec3 center = cr.bodyCenter();
    const Vec3 n = path.normal(sC, in.uOf(sC));
    const Vec3 tip = center + n * 0.65f;
    line3(img, v, center, tip, 3.0f * k, white);
    {
        const Vec3 side = normalize(cross(n, path.tangent(sC)));
        line3(img, v, tip, tip - n * 0.12f + side * 0.07f, 3.0f * k, white);
        line3(img, v, tip, tip - n * 0.12f - side * 0.07f, 3.0f * k, white);
    }

    // ---- legs: the step arc, the footholds, the two bones ----
    int swingLeg = -1, plantedLeg = -1;
    for (int l = 0; l < 4; ++l) {
        const Creature::LegState& ls = cr.legs()[size_t(l)];
        const Color c = legCol[l];
        if (!ls.planted) {
            swingLeg = swingLeg < 0 ? l : swingLeg;
            std::vector<Vec3> arc;
            const Vec3 lift = normalize(ls.nFrom + ls.nTo) * 0.13f;
            for (int i = 0; i <= 24; ++i) {
                const float w = float(i) / 24.0f, ws = w * w * (3.0f - 2.0f * w);
                arc.push_back(ls.from + (ls.to - ls.from) * ws + lift * std::sin(kPi * w));
            }
            polyline3(img, v, arc, 2.5f * k, alpha(c, 230), true);
        } else if (plantedLeg < 0) {
            plantedLeg = l;
        }
        // the next foothold: a ring flat on the surface (its normal nTo)
        {
            const Vec3 a = normalize(cross(ls.nTo, std::abs(ls.nTo.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0}));
            const Vec3 b = cross(ls.nTo, a);
            std::vector<Vec3> ring;
            for (int i = 0; i <= 20; ++i) {
                const float t = 2.0f * kPi * float(i) / 20.0f;
                ring.push_back(ls.to + ls.nTo * 0.01f + (a * std::cos(t) + b * std::sin(t)) * 0.07f);
            }
            polyline3(img, v, ring, 2.5f * k, c);
            line3(img, v, ls.to - a * 0.1f, ls.to + a * 0.1f, 1.5f * k, alpha(c, 200));
            line3(img, v, ls.to - b * 0.1f, ls.to + b * 0.1f, 1.5f * k, alpha(c, 200));
        }
        // bones
        line3(img, v, ls.hip, ls.knee, 5.0f * k, c);
        line3(img, v, ls.knee, ls.claw, 3.5f * k, c);
        P ph, pk, pc;
        if (v.project(ls.hip, ph)) img.fillCircle(ph.x, ph.y, 6.0f * k, white);
        if (v.project(ls.knee, pk)) img.fillCircle(pk.x, pk.y, 5.0f * k, c);
        if (v.project(ls.claw, pc)) {
            if (ls.planted) {
                img.fillCircle(pc.x, pc.y, 7.0f * k, white);
                img.fillCircle(pc.x, pc.y, 4.0f * k, c);
            } else {
                img.fillCircle(pc.x, pc.y, 5.0f * k, c);
            }
        }
    }

    // ---- labels (kept clear of the two panels and of each other) ----
    std::vector<Box> taken{{0.0f, 0.0f, 610.0f * k, 236.0f * k},
                           {0.0f, float(img.height()) - 266.0f * k, 574.0f * k, float(img.height())}};
    {
        const int L = plantedLeg >= 0 ? plantedLeg : 0;
        const Creature::LegState& ls = cr.legs()[size_t(L)];
        P a;
        if (v.project((ls.hip + ls.knee) * 0.5f, a)) label(img, taken, a, "femur (hip to knee)", legCol[L], k, 110.0f);
        if (v.project((ls.knee + ls.claw) * 0.5f, a)) label(img, taken, a, "tibia (knee to claw)", legCol[L], k, 150.0f);
        if (v.project(ls.claw, a)) label(img, taken, a, "planted claw", white, k, 200.0f);
    }
    if (swingLeg >= 0) {
        const Creature::LegState& ls = cr.legs()[size_t(swingLeg)];
        P a;
        const Vec3 mid = (ls.from + ls.to) * 0.5f + normalize(ls.nFrom + ls.nTo) * 0.13f;
        if (v.project(mid, a)) label(img, taken, a, "step arc", legCol[swingLeg], k, 120.0f);
        if (v.project(ls.to, a)) label(img, taken, a, "next foothold", legCol[swingLeg], k, 160.0f);
    }
    {
        P a;
        if (v.project(tip, a)) label(img, taken, a, "up (surface normal)", white, k, 80.0f);
        const float u = in.uOf(sC + 1.0f) + 0.6f;
        if (v.project(path.point(sC + 1.0f, u), a)) label(img, taken, a, "the surface it holds", Color{235, 240, 245, 255}, k, 70.0f);
    }

    // ---- caption and legend, top left ----
    {
        const float x = 36.0f * k, y = 36.0f * k;
        img.fillRect(int(x - 14 * k), int(y - 10 * k), int(x + 560 * k), int(y + 188 * k), Color{8, 10, 14, 165});
        text(img, x, y + 34 * k, 34.0f * k, "How it crawls", white, true);
        text(img, x, y + 74 * k, 26.0f * k, std::string("on: ") + in.stage, Color{200, 225, 240, 255});
        for (int l = 0; l < 4; ++l) {
            const float lx = x + float(l % 2) * 270.0f * k, ly = y + 116.0f * k + float(l / 2) * 36.0f * k;
            img.fillRect(int(lx), int(ly - 18 * k), int(lx + 22 * k), int(ly + 2 * k), legCol[l]);
            text(img, lx + 32 * k, ly, 24.0f * k, legName[l], white);
        }
    }
    // ---- the gait: each leg's step cycle, bottom left ----
    {
        const float x = 36.0f * k, y = float(img.height()) - 214.0f * k, barX = x + 170.0f * k, barW = 330.0f * k;
        img.fillRect(int(x - 14 * k), int(y - 40 * k), int(barX + barW + 24 * k), int(y + 170 * k), Color{8, 10, 14, 165});
        text(img, x, y - 8 * k, 26.0f * k, "step cycle:  planted  |  swinging", white, true);
        for (int l = 0; l < 4; ++l) {
            const Creature::LegState& ls = cr.legs()[size_t(l)];
            const float ry = y + 22.0f * k + float(l) * 36.0f * k;
            text(img, x, ry + 8 * k, 22.0f * k, legName[l], legCol[l]);
            const float split = barX + barW * Creature::kDuty;
            img.fillRect(int(barX), int(ry - 10 * k), int(split), int(ry + 10 * k), alpha(legCol[l], 170));
            img.fillRect(int(split), int(ry - 10 * k), int(barX + barW), int(ry + 10 * k), Color{40, 46, 54, 200});
            const float mx = barX + barW * ls.cycle;
            img.fillRect(int(mx - 2 * k), int(ry - 15 * k), int(mx + 2 * k), int(ry + 15 * k), white);
        }
    }
}

}  // namespace bh
