#include "modkit/scene_merge.h"
#include "modkit/bjson_decoder.h"
#include "modkit/bjson_patcher.h"
#include "modkit/scene_schema.h"
#include "modkit/scene_inject.h"

#include <vector>

namespace ed9loader {
namespace modkit {

using nlohmann::json;

SceneMergeResult MergeScene(const std::vector<uint8_t>& originalScene, const PatchConfig& cfg) {
    SceneMergeResult res;
    BjsonDecoder d;
    if (!d.Parse(originalScene)) { res.err = "scene decode: " + d.Error(); return res; }
    BjsonPatcher p(d);

    uint32_t actorOff = 0, aidOff = 0;
    if (!d.FindRootChild("Actor", actorOff)) { res.err = "no Actor root"; return res; }
    if (!d.FindRootChild("ActorIDs", aidOff)) { res.err = "no ActorIDs root"; return res; }
    json editedActor = p.RenderValue(actorOff);
    json editedAid = p.RenderValue(aidOff);

    std::vector<const AddActor*> templateLess;
    bool anyClone = false;

    for (const AddActor& a : cfg.addActors) {
        const uint32_t prefix = IdPrefixFor(a.type);
        if (prefix == 0) { res.err = "unknown/unsupported actor type " + a.type; return res; }

        json tmpl; bool found = false;
        for (const auto& el : editedActor)
            if (el.value("type", std::string()) == a.type) { tmpl = el; found = true; break; }
        if (!found) {
            if (SupportsTemplatelessInject(a.type)) { templateLess.push_back(&a); continue; }
            res.err = "no template actor of type " + a.type; return res;
        }
        anyClone = true;

        int serial = -1;
        for (auto& e : editedAid)
            if (e.value("name", std::string()) == a.type) { serial = (int)e["id"].get<double>(); e["id"] = (double)(serial + 1); break; }
        if (serial < 0) { res.err = "no ActorIDs entry for type " + a.type; return res; }
        const uint32_t id = (prefix << 24) | (uint32_t)serial;

        long maxOut = -1;
        for (const auto& el : editedActor)
            if (el.value("type", std::string()) == a.type) { long o = (long)el["outline_order"].get<double>(); if (o > maxOut) maxOut = o; }

        tmpl["id"] = (double)id;
        tmpl["name"] = a.name;
        if (a.hasTranslation) tmpl["translation"] = { {"x", a.tx}, {"y", a.ty}, {"z", a.tz} };
        tmpl["outline_order"] = (double)(maxOut + 1);
        if (a.hasLpRadius) tmpl["lp_radius"] = a.lpRadius;
        if (a.hasLpHeight) tmpl["lp_height"] = a.lpHeight;
        if (a.fields.is_object())
            for (auto it = a.fields.begin(); it != a.fields.end(); ++it)
                if (tmpl.contains(it.key())) tmpl[it.key()] = it.value();
        editedActor.push_back(tmpl);

        if (a.type == "LookPoint") {
            LpRow h; h.text1 = cfg.MapName(); h.text2 = a.name; h.text3 = a.label;
            res.tblHooks.push_back(std::move(h));
        }
    }

    std::vector<uint8_t> sceneBytes;
    if (anyClone) {
        if (!p.PatchRoot("Actor", editedActor)) { res.err = "PatchRoot Actor: " + p.Error(); return res; }
        if (!p.PatchRoot("ActorIDs", editedAid)) { res.err = "PatchRoot ActorIDs: " + p.Error(); return res; }
        sceneBytes = p.Bytes();
    } else {
        sceneBytes = originalScene;
    }

    for (const AddActor* a : templateLess) {
        SceneInjectResult ir;
        if (a->type == "MonsterArea") ir = InjectMonsterArea(sceneBytes, *a);
        else { res.err = "templateless inject not implemented: " + a->type; return res; }
        if (!ir.ok) { res.err = "inject " + a->type + " \"" + a->name + "\": " + ir.err; return res; }
        sceneBytes = std::move(ir.bytes);
    }

    res.sceneBytes = std::move(sceneBytes);
    res.ok = true;
    return res;
}

}
}
