// modkit: 把【无现成同类型模板】的 actor(目前 MonsterArea)注入 scene。
// 蓝本 binaryJson_to_json/inject_monsterarea_v2.py(已逆向出 bjson 真实格式并实测/golden验证):
//   - 名表条目 = [4字节game_hash][字符串\0],token=hash偏移=字符串偏移-4;
//   - 对象/compound 子节点按 game_hash 升序(引擎二分查找取字段);
//   - 缺失字段名要"扩名表"(在 hashstart 插新条目+移位所有节点+修子偏移表+更新 hashstart);
//   - MonsterArea node_hash 必须 = 该图 SceneTree "Battle_Section" 分区 hash。
// 用途:室内/无怪图(原 scene 没有 MonsterArea 模板、名表缺其字段名)也能从普通 scene_patch.json 合并。
#pragma once
#include "modkit/patch_config.h"
#include <cstdint>
#include <string>
#include <vector>

namespace ed9loader {
namespace modkit {

struct SceneInjectResult {
    bool ok = false;
    std::string err;
    std::vector<uint8_t> bytes;   // 注入后的 scene 二进制
};

// 是否支持"无模板注入"该类型(目前仅 MonsterArea)。
bool SupportsTemplatelessInject(const std::string& type);

// 把一个 MonsterArea 注入 scene。serial = 该 type 当前 ActorIDs 发号值(本函数内部会读 ActorIDs 并 +1)。
// a 提供 name/translation/group(分组名,默认 Battle_Section)/fields(scale/rotation/btl_*/stroll_margin/navi_name 覆盖)。
SceneInjectResult InjectMonsterArea(const std::vector<uint8_t>& scene, const AddActor& a);

} // namespace modkit
} // namespace ed9loader
