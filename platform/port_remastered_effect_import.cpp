#include "port_remastered_effect_import.h"

#include "port_remastered_effect_convert.h"
#include "port_remastered_image.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>

namespace PortRemastered {
namespace {

constexpr uint32_t kGenp = EffectFourCC("GENP");
constexpr uint32_t kMati = EffectFourCC("MATI");
constexpr uint32_t kPart = EffectFourCC("PART");
constexpr uint32_t kTxtr = EffectFourCC("TXTR");
constexpr uint32_t kCmdl = EffectFourCC("CMDL");

// Effect textures are drawn small; larger ones are scaled down to this side.
constexpr int kMaxTextureSide = 256;
// A flipbook atlas keeps its frames' size up to this edge.
constexpr int kMaxAtlasSide = 2048;

// Between a pak's byte order and the order an effect stores an id in: the
// first three groups byte-swapped (the same swap both ways).
EffectGuid Swap(const EffectGuid& id) {
  EffectGuid out = id;
  std::swap(out[0], out[3]);
  std::swap(out[1], out[2]);
  std::swap(out[4], out[5]);
  std::swap(out[6], out[7]);
  return out;
}

uint32_t Hash(const EffectGuid& id, uint32_t salt) {
  uint32_t hash = 0x811C9DC5u ^ salt;  // FNV-1a
  for (uint8_t byte : id) {
    hash = (hash ^ byte) * 0x01000193u;
  }
  return hash;
}

std::string Hex(uint32_t id) {
  char text[16];
  std::snprintf(text, sizeof(text), "%08X", id);
  return text;
}

int RoundUp4(int side) { return std::max(8, (side + 3) / 4 * 4); }

bool IsLight(uint32_t fourcc) {
  for (uint32_t light : {EffectFourCC("LTYP"), EffectFourCC("LFOT"), EffectFourCC("LCLR"), EffectFourCC("LINT"),
                         EffectFourCC("LOFF"), EffectFourCC("LDIR"), EffectFourCC("LFOR"), EffectFourCC("LSLA")}) {
    if (fourcc == light) {
      return true;
    }
  }
  return false;
}

bool HasLight(const std::vector<RetailPartProperty>& properties) {
  return std::any_of(properties.begin(), properties.end(),
                     [](const RetailPartProperty& property) { return property.fourcc == EffectFourCC("LTYP"); });
}

const EffectNode* FindNode(const EffectNode& node, const EffectGuid& id) {
  for (const EffectNode& child : node.children) {
    if (child.id == id) {
      return &child;
    }
    if (const EffectNode* found = FindNode(child, id)) {
      return found;
    }
  }
  return nullptr;
}

bool HasProperty(const EffectNode& node, uint32_t fourcc) {
  return std::any_of(node.properties.begin(), node.properties.end(),
                     [&](const EffectProperty& property) { return property.fourcc == fourcc; });
}

// A generator that drew a texture or a model whose converted PART draws
// neither: the conversion lost its look.
bool LostLook(const EffectNode& node, const std::vector<RetailPartProperty>& part) {
  const bool had = HasProperty(node, EffectFourCC("TEXR")) || HasProperty(node, EffectFourCC("MTIN")) ||
                    HasProperty(node, EffectFourCC("PMDL"));
  const bool has = std::any_of(part.begin(), part.end(), [](const RetailPartProperty& property) {
    return property.fourcc == EffectFourCC("TEXR") || property.fourcc == EffectFourCC("PMDL");
  });
  return had && !has;
}

void PutFourCC(std::vector<uint8_t>& out, uint32_t fourcc) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(uint8_t(fourcc >> shift));
  }
}

// `converted` with its light replaced by the disc PART's. Remastered leaves
// out LOFF/LDIR/LFOR/LSLA (it has LIRD/LORD instead, which do not map onto
// them) and differs from retail in places, so the disc's light is the one
// retail's lighting was made for.
std::vector<uint8_t> WithDiscLight(const std::vector<RetailPartProperty>& converted,
                                   const std::vector<RetailPartProperty>& disc) {
  std::vector<uint8_t> out;
  PutFourCC(out, EffectFourCC("GPSM"));
  auto put = [&](const RetailPartProperty& property) {
    PutFourCC(out, property.fourcc);
    out.insert(out.end(), property.value.begin(), property.value.end());
  };
  for (const RetailPartProperty& property : converted) {
    if (!IsLight(property.fourcc)) {
      put(property);
    }
  }
  for (const RetailPartProperty& property : disc) {
    if (IsLight(property.fourcc)) {
      put(property);
    }
  }
  PutFourCC(out, EffectFourCC("_END"));
  return out;
}

// Effects Remastered gave a fresh id, matched to the retail PART they replace.
struct MatchedEffect {
  const char* id;  // as IdToString prints it (EffectGuidString of the stored id)
  uint32_t retail;
};
constexpr MatchedEffect kMatchedEffects[] = {
    // By the name both paks give it (the player's and global paks'). Only names
    // that one retail PART has; the 8 effects that kept a retail id all match
    // their names.
    {"fb4d5181-cd7e-4c5e-8b11-afe35d30e231", 0x1EF973EA},  // BombExplo
    {"bbbb849a-d896-489f-83da-ef76c010888d", 0xC0E95E90},  // BombSet
    {"ca6fc8a1-30a2-441f-bf7a-12b6acd4e938", 0x39F0F5C6},  // BoostBallGlow
    {"ceaee14a-a690-44cc-a90b-0bd34935ae6c", 0x523048E0},  // BusterLight
    {"9c215d05-e582-4897-b958-85eb83c9f4f3", 0x9B564161},  // BusterMuzzle
    {"c1f18af2-c6e5-486b-86e0-0609316733f2", 0x04E29C5B},  // BusterSparks
    {"9499e42a-54bc-4402-8557-aa300f3db8de", 0xD8DB86CA},  // DirtWake
    {"97280bbe-5eef-42b6-b5bd-096b8c089c76", 0xF0C02F49},  // Effect_Ash
    {"70108314-fb42-4090-9ab4-b1c8c6570ee2", 0xA6B67F45},  // Effect_FirePop
    {"4dd6affe-ffd8-40ed-a056-1a77b9dcb9d6", 0xE6FC0230},  // Effect_IceBreak
    {"7bb7ac6f-16a2-45f0-a103-f634fe027768", 0x017DFFD5},  // Effect_IcePop
    {"103f6797-5ea0-4c2b-9c9b-eafdce3e48f4", 0xABE56164},  // Effect_MorphBallIceBreak
    {"9c56f6df-a9ae-403b-8da3-6a6ec6405971", 0x2D65511C},  // Effect_OnFire
    {"7cbb6382-2788-4225-b3a2-3430ba5558b7", 0xF42646D4},  // FlameMuzzle
    {"416c81e0-e3ab-4f72-b3f5-3aab7b851ba6", 0xD5A18910},  // grappleClaw
    {"9905ed28-e8ac-4231-9b05-ca49e338eb2d", 0xCFC222B0},  // grappleHit
    {"3727f6c9-0f4f-4369-ba8f-b22e71bfdd1d", 0x2CC7F7F5},  // grappleMuzzle
    {"14e59c07-ea96-4f88-ae84-a8762b7beab8", 0x87C0BDB2},  // grappleSegment
    {"f8502f32-276d-4db5-998c-f57c7a2cb00b", 0x7072A62D},  // holoTransition
    {"7114b32f-78b8-45fd-adc0-ab905c7671eb", 0x1BBFC5A6},  // Ice2nd_1
    {"c8aa99d2-f7a9-4bc0-bff1-1fb1b42c1990", 0xF97661F1},  // Ice2nd_2
    {"a6c1a9fb-e480-4b50-8e16-49ab849c86b7", 0x21F4D9AB},  // IceAuxMuzzle
    {"0649036e-0b4f-450f-b33d-01e9045a29c9", 0x6ECDC394},  // IceCharge
    {"91b571dc-1b15-4053-8e1a-8744abd2f2e1", 0x9ADE39C3},  // IceMuzzle
    {"b589f70b-1853-490e-bba8-a4f120563ddd", 0xC82F2028},  // IceSmoke
    {"42f41dcd-0da3-40e2-b4ec-d1c6096eda5b", 0xDE1A1140},  // IceSpread1
    {"b28d85f4-67a3-4664-981f-6fa9bf044f93", 0x045DDB2F},  // IceXfer
    {"ec64d113-7337-4ce0-bda7-0f885bfd0f50", 0x43A81EEC},  // MorphBallTransitionFlash
    {"56844d59-1a0f-4313-a6a8-533292dfd75c", 0x8B8CD2F6},  // MudWake
    {"2593f90a-21cf-4249-a3f2-64ef7fabe99b", 0xF639D24E},  // NFTMainFire
    {"369cc052-6d7b-44d0-8a33-e18f6fc67f85", 0xD67EE2D9},  // NFTMainSmoke
    {"d0766ccc-22d9-444f-8529-a8a6ada6cbdd", 0x1F4FD93A},  // NFTSecondaryFire
    {"bfe5b4c5-421f-41d1-9554-6d3e00f72066", 0xAD51661F},  // NFTSecondarySmoke
    {"93004165-738e-404b-ac29-1f1a059719ae", 0x7DA3DEE5},  // NFTSecondarySparks
    {"58175f6a-d712-4852-b350-13321240dc65", 0x7754967A},  // Phazon2nd_1
    {"c8536e8f-2e70-41b2-82b9-1c8410ac9c6f", 0x1C56F6B1},  // PhazonMuzzle
    {"4474842f-b051-45be-b72b-b7d03128d755", 0x18CB74EF},  // PhazonWake
    {"3210e9de-2f83-48f3-9168-d8a3587be355", 0x6C35D8FE},  // PhazonWakeOrange
    {"541f775f-c171-4b2e-a182-6ada992a21dc", 0xC0A88A87},  // Plasma2nd_1
    {"f494900e-6c34-41bb-a372-9f6f1fe6468a", 0xB0F9DBE6},  // PlasmaAuxMuzzle
    {"b2f2c408-d488-43ab-8562-e85bca2b654f", 0xD3053354},  // PlasmaCharge
    {"d682fa42-228c-445a-856f-18581ed7866d", 0x8D7BBFB2},  // PlasmaMuzzle
    {"6a83fb12-e81e-499a-9014-15a539f53ec9", 0x5721EE48},  // PlasmaXfer
    {"51d96194-91b7-416e-8c37-13031270163e", 0x3183F0A0},  // Power2nd_1
    {"4bc8b7ca-ce44-4e88-9e8f-a764138a9601", 0x7E8ADCBA},  // PowerBombExplo
    {"835d9ae7-2f75-48ab-ac9f-b9827b8740ad", 0x4CE91ECB},  // PowerCharge
    {"597c6fe6-6fe7-439b-956b-87728a5a067f", 0x0F21403B},  // PowerMuzzle
    {"411dae2f-d4be-4324-9433-7822f188928c", 0x3DD09610},  // PowerXfer
    {"d3abe44e-d8a8-473b-a60e-c2289eb6a74d", 0x8185DEB3},  // RainWake
    {"c82dd5d6-f6d9-4908-9794-f3fd62627c32", 0xF421ED31},  // SandWake
    {"5c2798a0-12e5-4153-b187-0594d7f8fa5d", 0x60817832},  // ShotSmoke
    {"81cb1d8a-4cb4-4469-8127-d6c761752e7c", 0xC9D4BA43},  // SnowWake
    {"026c1c9a-df22-4230-937b-29e785637252", 0x22B005A1},  // SpiderBallMagnetEffect
    {"7e1ca0b3-d242-4724-b1dc-ecb96e43bc73", 0xE1341D07},  // WallSpark
    {"1b44823d-c587-4b51-9c1d-65144c5fcac5", 0x629C848F},  // Wave2nd_3
    {"50b67be4-15f3-4230-9147-deb8d6a47248", 0x7E520CBC},  // WaveAuxMuzzle
    {"a0d802cb-6516-4a42-9c1d-7614e6122ca9", 0x2BC80C63},  // WaveCharge
    {"59dba49a-777f-42eb-ab47-c5dd91a28d3e", 0x0237C838},  // WaveXfer
    // By where the room scripts place it: a ROOM's EffectMP1 naming only this
    // effect, at the spot (under 0.1 m) of a retail object naming only this
    // PART, in every such placement, with no other fresh effect or carried-over
    // one claiming the PART. The same rule pairs all 97 such placements of the
    // carried-over effects with their own id. The comment is the first room
    // (+ how many more).
    {"0040cf36-fd79-4264-8ad9-4bbdd7b5f18e", 0xABEA897E},  // 20_reflecting_pool
    {"02707a2b-9131-4f9c-917e-fb3f9f05db1c", 0x424FFEF7},  // 08a_IntroUnderwater_ventshaft +1
    {"02d03932-c135-4244-b1c1-50f34488ad25", 0x371A7EEE},  // 00a_over_hall +7
    {"02db3cbc-dbfc-4131-821d-6f96b4a9b506", 0xD81CFC9E},  // 00F_intro_begin
    {"034bd14d-2bc9-481f-a619-f79a584e6f92", 0x7A95281E},  // 03a_crater
    {"06ee7e7c-663b-43be-80e5-520e15ee2976", 0x77B8A73B},  // 0p_connect_tunnel
    {"0a450035-0b32-4ff9-a376-bd7e0345aa1b", 0xCD494C73},  // 18_Ice_Gravity_Chamber
    {"0a6f8735-0847-455f-8365-4fa6fadddf7d", 0xA11929CF},  // 11_Ice_Observatory
    {"0b66c940-0645-410c-b505-c168ac2ede27", 0x9271FCB8},  // 02_Intro_Elevator
    {"0b97f917-0308-4d9c-87c0-429c7e2f2b2f", 0x02787FD6},  // 05_Zoo
    {"0dae37f3-9223-4187-b932-fdd74d92cf46", 0xFDB2830E},  // 14_tl_base01
    {"0e2e3afe-eab2-4739-a82b-636678e996ee", 0xD108687B},  // 12_Mines_eliteboss
    {"0edb2029-b9df-4522-a94c-8c5701d4ae5c", 0xCEE7ADCA},  // 00F_intro_begin
    {"0ff6151e-7272-42b4-8c5e-c5018cc6e44c", 0x89F18B4D},  // 11_Ice_Observatory
    {"115bd210-face-4399-a19a-476dd5ce4d43", 0x82396733},  // 05_IntroUnderwaterZoo
    {"13a0b617-347d-42c3-8009-8bcb6767bf83", 0x40B4B8AC},  // 07_Over_Stonehenge
    {"16dc475c-d50c-42c5-8a4b-20e636a4e980", 0xDBF12204},  // 11_Ice_Observatory
    {"17dd34bd-3af8-4e5d-be0b-7fae6f94c4d6", 0x8804A9BF},  // 05_Mines_forcefields
    {"1afc0d7c-babe-4bdb-b4cd-2052ccc4fb14", 0xA7C92DE0},  // 07_Over_Stonehenge
    {"2077f968-bffe-432c-bcca-6e877fdaf021", 0x50DD1328},  // 22_flaahgraChamber
    {"20cf1159-28b8-4221-8cf7-408a5a8d992f", 0xD89F3CEB},  // 13_mines_vertical_ascent
    {"2254aba7-3bfc-4b48-ae1e-e81e97c6492c", 0x6FC6B324},  // 00b_mines_connect +9
    {"22b3f678-35be-4214-9dd0-ab3d5dff147b", 0x4DDF468C},  // 06_Ice_Temple
    {"23c2978d-1961-498c-bfa5-63c8efaffb99", 0x0FBD4196},  // 07_Over_Stonehenge
    {"25a15301-1cd7-4660-bd07-327810cf6135", 0x13F23AB1},  // 00a_over_hall +7
    {"2710c664-4fbc-4358-bd8a-41824297f55c", 0xA8B1CCA8},  // 00f_ice_connect +1
    {"27ba22cf-87c7-4904-bab7-dae70760ff44", 0x5D664CA2},  // 11_Ice_Observatory
    {"291f812e-7a38-41e7-a277-eea20be5a5c3", 0xA369D3DB},  // 07_Over_Stonehenge
    {"29a7c944-95c8-4137-b63f-c3955d77c27e", 0x1D985C2C},  // 22_flaahgraChamber
    {"2c5365a6-7dc4-40b2-9729-2f7742998a81", 0x930DD780},  // 22_flaahgraChamber
    {"2ef9a569-58d8-422a-b8b1-37e184de3f50", 0x371C563C},  // 00i_Mines_connect
    {"309a173d-f865-4487-b421-c9e64edec577", 0xB4A658C3},  // 08_Mines
    {"309fc565-2f71-414a-83c6-98833253b563", 0x07F04ED5},  // 00l_over_hall
    {"335247ff-2780-4bf0-ad60-2bdc0f609c62", 0x5A5E6C8F},  // 12_Mines_eliteboss
    {"3537fd56-c4d4-4f23-ac77-3d642c52f836", 0x757EDCE9},  // 05_Over_xrayroom +2
    {"39f50802-ef90-4f7a-a9c5-adf3be6bb791", 0x06B3F06E},  // 01_intro_hanger_connect
    {"39f5c872-0e93-4a66-a847-f8465f8af571", 0x4693F099},  // 02_Intro_Elevator
    {"3a33588c-6b7f-453b-8e4a-82bca75f6de0", 0x251B04F4},  // 0v_connect_tunnel +4
    {"3bece9e5-c319-4e01-a28e-a38fee7777b8", 0xB0599D9B},  // 11_Ice_Observatory
    {"3c93322a-845d-49b5-bb15-650bbbaa2e04", 0x450DADFF},  // 00_Mines_Mapstation +2
    {"3f7ae5bd-d9d1-4c01-bca6-4d31b3937538", 0x70043FD4},  // 3_monkey_lower
    {"4044ff75-0097-4368-b240-8a3feb2f5af0", 0xE4CA5AE7},  // 00k_ice_connect +7
    {"41344111-1037-4af3-8c4e-1e137949029a", 0x1ECD34A2},  // 00i_ice_connect +10
    {"4365ca0c-3dd9-4267-99ca-321dc8ee6a3b", 0x2CD29D26},  // 00_Mines_Savestation_D
    {"445c8f18-92d2-449e-86c4-758dfba2b8fe", 0x409749DB},  // 17_ChozoBowling
    {"457769f5-1ac1-4cbe-b111-a4df50009f72", 0xFDBD7828},  // 17_Ice_Cave_B
    {"4abea211-408f-4fc3-b1fa-2984f60aecdb", 0x30F46D12},  // 03b_Crater
    {"4b2dc620-b765-4430-88ea-af0386f0e43b", 0xCEFB49E0},  // 07_Over_Stonehenge
    {"4c1a5053-e91b-4e03-b4ab-1c5b093ebf6c", 0x186971C3},  // 22_flaahgraChamber
    {"4ded839b-6e75-48e8-9218-91af6c230a64", 0x387E2199},  // 03f_Crater
    {"52394d38-35a2-44a5-97ad-995f1b27358b", 0x28016E5F},  // 07_Over_Stonehenge
    {"54da4609-8d65-4132-84e5-0b7fea7f0b30", 0xD16D45BB},  // 12_Mines_eliteboss
    {"56335892-39fe-4573-a81a-9241e7d087ab", 0xC8A42628},  // 00g_ice_connect +8
    {"57893869-e39b-43c8-b86d-e2e419030fba", 0x930B6C5F},  // 5_bathhall
    {"57bc7e0d-b2cf-43df-8fa2-b69c98d07b8c", 0xDED6D5A4},  // 00g_over_hall
    {"5811ed93-8a30-4769-ad84-0eaf8e496361", 0xA28210DF},  // 14_tl_base01
    {"59bb837a-7790-4ff1-9aeb-bd39f60abd2e", 0xDF5B7160},  // 0p_connect_tunnel
    {"5a61ec58-6e8f-48b3-b3e1-7ca6c0e951b6", 0x621D84AC},  // 12_Mines_eliteboss
    {"5b985124-2eff-4ef5-9f11-d5999646f0ae", 0x2230069F},  // 00c_lava_connect
    {"5cdf8300-04fe-4648-9a81-d64a373e8671", 0xE5DDD684},  // 16_furnaces
    {"5fb0da50-c693-41eb-8b33-00ef4ec8638b", 0xB2C0B71F},  // 19_Ice_Thardus
    {"600d7a0c-0719-4de5-99c8-59f8d28605e2", 0x37524FE9},  // 04_Ice_Boost_canyon +3
    {"60985973-2d34-42f2-8c43-2d57460e818a", 0xBD3DC521},  // 01_Over_mainplaza
    {"60dcd6c3-adf1-42c8-8ea4-7fd3934ffcc4", 0x185DD7C5},  // 00b_IntroUnderwater_connect
    {"628b3331-04f8-4da9-a533-3715c6282a70", 0x7909612F},  // 00f_over_hall +10
    {"65e71f3a-0327-4bb2-85b6-ed993f742791", 0x597475AD},  // 0p_connect_tunnel
    {"6916272d-4272-4ec4-a0b2-a4f9b3160ef6", 0x3A617BEB},  // 00b_Lava_Connect +3
    {"69c55769-8353-4afa-adfb-d5b9f9566e1b", 0x1CD31D99},  // 00F_intro_begin
    {"6a264e33-992c-46b7-94e2-112a1696a817", 0x1D93BC95},  // 10_Over_1Alavaarea
    {"6c7d0204-4456-42ce-a50a-1da0b06876d4", 0xA2D977BF},  // 07_Over_Stonehenge
    {"6ed2e1b1-03ba-48a5-9345-06636cea35a6", 0x4E63C759},  // 0p_connect_tunnel
    {"73c163b7-2cbf-4053-8a31-7e36655eda47", 0x898E04A0},  // 22_flaahgraChamber
    {"74065695-c63d-4021-baf3-123ea35b815c", 0x50A5876B},  // 19_hivetotem
    {"77224f08-a7be-42df-b1f6-38425aeeabe5", 0xB58775FC},  // 07_Over_Stonehenge
    {"7790bc6d-7532-42a3-be5e-9711f34a1f99", 0x76B916D1},  // 00e_IntroUnderwater_connect
    {"7a85a79f-a832-4a4d-80e4-051545fa7624", 0xBC84152B},  // 1a_morphball_shrine
    {"7b5059bc-5b2f-4d3c-bb2c-c44a07eaf97e", 0x47E5939E},  // 07_Over_Stonehenge
    {"7d6e4416-66fd-40ad-ba7c-9649cf725503", 0x4619C3F1},  // 07_Over_Stonehenge
    {"7f6c0fc0-adf5-4f37-b6ca-5ffa6731ed42", 0xD74B6FD2},  // 11_Ice_Observatory
    {"7fefddd7-fc42-49cb-b6a8-bc9243e45ae9", 0xDE1E2414},  // 03_over_pickup
    {"7ff707bd-0170-4d03-b140-7cc87321ffe8", 0x1B888B61},  // 18_halfpipe
    {"83e2824b-b4cc-4f42-a435-a601d828a3e5", 0x3DF9F7BE},  // 08_Ice_Ridley
    {"8c43cd20-3484-435d-82be-f1efc0fc7d42", 0xB4BE74A6},  // 00d_Intro_connect
    {"8c5a52c5-0317-44c5-96e3-b2ab015faf2a", 0x3AE271F4},  // 03f_Crater
    {"8d4865f1-bb83-4520-a7d8-da09dee766b1", 0x6A582E2B},  // 13_Over_burningeffigy
    {"8ff1284e-723b-4c98-90ec-6220a713ddc9", 0x205B13E3},  // 02_Intro_Elevator
    {"9251aa29-ab9b-466a-9a3f-03726d09c038", 0x9602A23A},  // 07_Mines_electric
    {"9317cb5b-a425-4b2a-b6eb-6f1a5891a4dd", 0xCCABC2EC},  // 3_monkey_lower
    {"95964af3-68cc-4c95-adc8-4728a3a018ee", 0x53861B29},  // 07_Over_Stonehenge
    {"96a7a567-0eb5-43da-b63a-cb007b5d0af2", 0xBD5CF12E},  // 05_Mines_forcefields
    {"97a7ee62-75ea-4791-b0b1-110227d04092", 0xC0C82ED0},  // 07_Over_Stonehenge
    {"98f103c6-926c-42c3-a766-9ebc65bf79c2", 0x4DD133C8},  // 07_Over_Stonehenge
    {"99bcbed9-5c9b-4317-9623-33eee5d5a683", 0x549475DE},  // 12_Mines_eliteboss
    {"9a15707d-25ba-424b-a6b1-04a52616986e", 0xE93B85DC},  // 07_Over_Stonehenge
    {"9b0876c3-317b-480d-8672-998785401aa0", 0x338F4B8A},  // 04_Intro_Specimen_Chamber
    {"9dd83fc0-8563-42a0-97cf-e5790fce54d1", 0x042EADAC},  // 11_Ice_Observatory
    {"9f3c5719-2e4d-4aa4-bf0b-ca51fd77d50f", 0x83A1450F},  // 00d_Intro_connect
    {"a178f834-68b9-4285-8fb2-73cb172319f0", 0x85DBF2BC},  // 0p_connect_tunnel
    {"a1a0b5e0-f612-459d-bb64-5144c869ab25", 0x6E238E65},  // 03a_crater
    {"a54ddf94-8f35-48c1-bafa-87ca737636c9", 0x00FB9A4D},  // 07_Over_Stonehenge
    {"a5b20f7a-8fa5-40c1-aa41-3ec2a4b688a9", 0x934BAFB8},  // 04_Intro_Specimen_Chamber
    {"a8475a7e-1e0b-4800-9d2c-91731c855dd4", 0xB15B2B5E},  // 07_Over_Stonehenge
    {"a8857d04-09f1-4532-9eeb-5e957355f31d", 0xFA41CC07},  // 05_Mines_forcefields
    {"a926c501-3277-4f76-b0a6-11d4264aa0d5", 0x59BA29DB},  // 07_Mines_electric
    {"ab2037b6-c0ac-4dfb-8bea-98c8b6910a07", 0x87817423},  // 5_bathhall
    {"ab210e1b-9617-4fd6-9c8c-98efb66568c1", 0xABAB33BA},  // 07_Over_Stonehenge
    {"aca74c4a-dd5e-4fe3-89c1-7de257330ee0", 0x277CABEC},  // 12_Mines_eliteboss
    {"af780373-de26-4349-8c27-dc49df6a9f10", 0xDE06865F},  // 05_Zoo
    {"b2b70ba4-0585-4cc9-a2f4-b737cb81b02a", 0xCB271679},  // 00b_Intro_Connect
    {"b3c9de27-f456-4de9-806f-3bf74a99eca7", 0x715DBDF0},  // 02_Intro_Epodroom
    {"b570f388-7be8-4baa-bac4-88771b2ba7f9", 0xA4F230FD},  // 00o_over_hall
    {"b8c3841e-b81b-427a-9b57-f7f970b48c2d", 0xD2967C1D},  // 09_Ice_Lobby
    {"b8cc7d68-1a15-472b-8942-ef68e2a392ef", 0x73483913},  // 07_Intro_Reactor
    {"b9ae3073-22db-4b17-aabe-a660ba6a311b", 0x8214BFCD},  // 04_Intro_Specimen_Chamber
    {"b9c4fbdb-d7b4-4dd9-a3db-13aaa2401c91", 0x8964D1BA},  // 12_Mines_eliteboss
    {"bb850e5d-5d64-4069-ba73-cdd0631054a4", 0x96993798},  // 07_Over_Stonehenge
    {"bb8d240b-e688-48ac-a17c-cc4b4b87240d", 0x4F5FC020},  // 02_Intro_Elevator
    {"be62630f-0d27-49d8-b9a0-3abce40b7f6c", 0x9CE89362},  // 11_Ice_Observatory
    {"c457accd-60a1-4d34-9304-e905b3ca10ef", 0x6097F60F},  // 11_Ice_Observatory
    {"c618ce1f-9579-486c-9f5f-73d9d00d7406", 0x531390D6},  // 0q_connect_tunnel
    {"c65d8956-5d6b-4064-9e62-656d43c5c767", 0x3FEEF398},  // 12_Mines_eliteboss
    {"ca64283e-7b49-47d6-892d-49fcabee3085", 0x46B0CE60},  // 00g_over_hall
    {"cce706c7-4299-4791-974e-4bdc3ed2539f", 0x0C0B11AC},  // 12_Mines_eliteboss
    {"cd873798-b325-4394-a574-caee0bdaeadd", 0x9C1920EF},  // 08_Ice_Ridley
    {"cebbb395-8928-48c4-aeca-c5e9fea5abc5", 0x2509A992},  // 04_Intro_Specimen_Chamber +2
    {"cef0b80d-20b2-4208-877b-03cdf5153b64", 0xA5ED941C},  // 1a_morphball_shrine
    {"cf619c68-33fb-4a55-aafc-4e5fda58b40c", 0xB372929B},  // 05_Over_xrayroom
    {"d1d6d9d2-cf72-4b28-82c4-22c2945789da", 0x2784DC47},  // 22_flaahgraChamber
    {"d2a93799-1173-44fe-a879-2d9895102097", 0x9644A054},  // 00d_Intro_connect
    {"d46bcc2c-6a03-4dd7-8222-f1f1f28c1fc5", 0x5AECC3E3},  // 05_Zoo
    {"d493f9f8-71be-4828-8b56-04a1c81cc22e", 0xA8439D72},  // 12_Ice_Research_B
    {"d5f23771-8d84-4aa3-ba0c-7467d3bd7436", 0xAF33BBB6},  // 3_monkey_upper
    {"d7a66600-615d-4b12-afbc-6e2f460249dd", 0x980D6664},  // 08_Mines
    {"d8b8986c-b46f-4ab9-9fed-9ebdb54c1f98", 0x9EE17789},  // 14_tl_base01
    {"d906ed85-4164-4e35-a8bb-069b806b15a9", 0xC88122F7},  // 07_Over_Stonehenge +5
    {"e2cee018-55b4-4d63-a593-4530a82be1cc", 0x70DB5FE4},  // 04_Ice_Boost_canyon +1
    {"e2da9a12-f6d7-40b6-9fd0-9a456545cd2e", 0x168D4892},  // 07_Mines_electric
    {"e4a7db3e-1eb5-4a32-bbcd-d71b9b3bab6a", 0x5E4647AB},  // 00k_ice_connect +7
    {"ea4d62a3-bbc5-425f-8996-c0eb9ad220e0", 0x61F0823E},  // 0c_connect_tunnel +8
    {"ea839ff8-3cac-418e-bf54-198792e7d250", 0x701B0D4E},  // 15_energycore
    {"eb806cff-4a11-4f21-88b2-ea96888c144f", 0xCD322DD4},  // 00g_ice_connect +8
    {"ecaa1a14-aa8f-49b1-afac-476a58d5390c", 0x0A7DFE25},  // 22_flaahgraChamber
    {"ecc9240a-da0d-409a-8d47-d39bf0d8a283", 0xACC3441A},  // 00F_intro_begin
    {"eea313db-233d-4305-9462-b9c7745debf4", 0xD9B81D49},  // 22_flaahgraChamber
    {"eea883f6-056c-41f9-8615-a3bffc8269a9", 0xD9BA0365},  // 5_bathhall
    {"f2ca3a48-8ddd-498f-bb2a-46cc08da54e7", 0xFA6EC61A},  // 12_Mines_eliteboss
    {"f66294d5-6b7d-40a8-aee4-74c24ce6884f", 0xA35E94DA},  // 12_Mines_eliteboss
    {"fc6996fc-6d6a-4f84-bb98-ec28e9909c1b", 0x807437B3},  // 07_Over_Stonehenge
    {"fdaff8f6-b669-4506-8838-286e7298f7cb", 0x5CF7E943},  // 0p_connect_tunnel
    {"fec1dea9-b8c3-4911-8579-1a38413c537a", 0x877E8A36},  // 15_energycore
};

// The retail PART an effect replaces: the id it carried over, else its match's.
std::optional<uint32_t> RetailEffect(const EffectGuid& id) {
  if (const std::optional<uint32_t> retail = EffectRetailId(Swap(id))) {
    return retail;
  }
  const std::string text = EffectGuidString(Swap(id));
  for (const MatchedEffect& matched : kMatchedEffects) {
    if (text == matched.id) {
      return matched.retail;
    }
  }
  return std::nullopt;
}

class Importer {
public:
  explicit Importer(const EffectImportIO& io) : m_io(io) {}

  void Log(const std::string& line) const {
    if (m_io.log) {
      m_io.log(line);
    }
  }

  // The id a Remastered texture (pak order) is written under, converting it
  // the first time; 0 when it cannot be.
  uint32_t Texture(const EffectGuid& id) {
    const auto known = m_textures.find(id);
    if (known != m_textures.end()) {
      return known->second;
    }
    uint32_t out = 0;
    Image image;
    std::string error;
    if (!m_io.texture || !m_io.texture(id, image.width, image.height, image.rgba, error)) {
      Log("effect texture " + EffectGuidString(Swap(id)) + ": " + error);
    } else {
      int width = image.width;
      int height = image.height;
      while (width > kMaxTextureSide || height > kMaxTextureSide) {
        width = std::max(1, width / 2);
        height = std::max(1, height / 2);
      }
      width = RoundUp4(width);
      height = RoundUp4(height);
      if (width != image.width || height != image.height) {
        image = Resize(image, width, height);
      }
      out = m_io.freshId(Hash(id, kTxtr));
      if (m_io.write(Hex(out) + ".TXTR", EncodeTxtrRgba8(image))) {
        ++m_result.textures;
      } else {
        out = 0;
      }
    }
    m_textures.emplace(id, out);
    return out;
  }

  // An array texture's layers packed into one atlas TXTR, row-major from the
  // top: a power-of-two column count, and frames halved only while an edge is
  // over kMaxAtlasSide. Id 0 when it cannot be.
  FlipbookAtlas Flipbook(const EffectGuid& id) {
    const auto known = m_flipbooks.find(id);
    if (known != m_flipbooks.end()) {
      return known->second;
    }
    FlipbookAtlas out;
    int width = 0, height = 0, layers = 0;
    std::vector<uint8_t> rgba;
    std::string error;
    if (!m_io.layers || !m_io.layers(id, width, height, layers, rgba, error)) {
      Log("effect flipbook " + EffectGuidString(Swap(id)) + ": " + (m_io.layers ? error : "no layer reader"));
    } else if (layers < 1 || width < 1 || height < 1 || rgba.size() != size_t(width) * height * layers * 4) {
      Log("effect flipbook " + EffectGuidString(Swap(id)) + ": no layers");
    } else {
      int cols = 1;
      while (cols * cols < layers) {
        cols *= 2;
      }
      const int rows = (layers + cols - 1) / cols;
      int frameW = width;
      int frameH = height;
      while (cols * frameW > kMaxAtlasSide || rows * frameH > kMaxAtlasSide) {
        frameW = RoundUp4(frameW / 2);
        frameH = RoundUp4(frameH / 2);
      }
      Image atlas;
      atlas.width = cols * frameW;
      atlas.height = rows * frameH;
      atlas.rgba.assign(size_t(atlas.width) * atlas.height * 4, 0);
      for (int k = 0; k < layers; ++k) {
        Image frame;
        frame.width = width;
        frame.height = height;
        const uint8_t* src = rgba.data() + size_t(k) * width * height * 4;
        frame.rgba.assign(src, src + size_t(width) * height * 4);
        if (frameW != width || frameH != height) {
          frame = Resize(frame, frameW, frameH);
        }
        const int x0 = (k % cols) * frameW;
        const int y0 = (k / cols) * frameH;
        for (int y = 0; y < frameH; ++y) {
          std::memcpy(atlas.rgba.data() + (size_t(y0 + y) * atlas.width + x0) * 4,
                      frame.rgba.data() + size_t(y) * frameW * 4, size_t(frameW) * 4);
        }
      }
      const uint32_t fresh = m_io.freshId(Hash(id, kTxtr ^ 0xF11Bu));
      if (m_io.write(Hex(fresh) + ".TXTR", EncodeTxtrRgba8(atlas))) {
        ++m_result.textures;
        ++m_result.flipbooks;
        out = FlipbookAtlas{fresh, cols, rows, layers};
      }
    }
    m_flipbooks.emplace(id, out);
    return out;
  }

  // The id a Remastered-only model (pak order) is written under, converting
  // it the first time; 0 when it cannot be.
  uint32_t Model(const EffectGuid& id) {
    const auto known = m_models.find(id);
    if (known != m_models.end()) {
      return known->second;
    }
    uint32_t out = 0;
    std::string error;
    if (m_io.model) {
      out = m_io.freshId(Hash(id, kCmdl));
      if (m_io.model(id, out, error)) {
        ++m_result.models;
      } else {
        Log("effect model " + EffectGuidString(Swap(id)) + ": " + error);
        out = 0;
      }
    }
    m_models.emplace(id, out);
    return out;
  }

  // The retail id for an id as an effect stores it: the disc's own when it
  // was carried over from retail, else a converted texture's or model's.
  uint32_t Stored(const EffectGuid& stored, uint32_t type) {
    const std::optional<uint32_t> retail = EffectRetailId(stored);
    if (retail && m_io.retailId(*retail)) {
      return *retail;
    }
    const EffectGuid id = Swap(stored);
    if (type == kTxtr && m_io.typeOf(id) == kTxtr) {
      return Texture(id);
    }
    if (type == kCmdl && m_io.typeOf(id) == kCmdl) {
      return Model(id);
    }
    return 0;
  }

  // The first texture a material instance names that converts.
  uint32_t Material(const EffectGuid& stored) {
    std::vector<uint8_t> data;
    std::string error;
    if (!m_io.read(kMati, Swap(stored), data, error)) {
      return 0;
    }
    for (size_t at = 0; at + 16 <= data.size(); ++at) {
      EffectGuid id;
      std::memcpy(id.data(), data.data() + at, 16);
      if (m_io.typeOf(Swap(id)) == kTxtr) {
        if (const uint32_t texture = Stored(id, kTxtr)) {
          return texture;
        }
      }
    }
    return 0;
  }

  // Each embedded child's fresh id, and the retail type it converts to (0 for
  // a form that is not converted, so nothing resolves to it).
  struct Child {
    uint32_t id;
    uint32_t type;
  };

  void Children(const EffectNode& node, uint32_t root, std::map<EffectGuid, Child>& out) {
    for (const EffectNode& child : node.children) {
      out.emplace(child.id, Child{m_io.freshId(Hash(child.id, root)), EffectRetailType(child.form)});
      Children(child, root, out);
    }
  }

  void Effect(const EffectGuid& id) {
    const std::optional<uint32_t> retail = RetailEffect(id);
    if (!retail || !m_io.retailId(*retail)) {
      return;
    }
    ++m_result.candidates;
    const std::string name = Hex(*retail) + ".PART";
    std::vector<uint8_t> data;
    std::string error;
    EffectNode effect;
    if (!m_io.read(kGenp, id, data, error) || !ParseEffect(data.data(), data.size(), effect, error)) {
      ++m_result.failed;
      Log(name + ": " + error);
      return;
    }
    std::map<EffectGuid, Child> children;
    Children(effect, *retail, children);
    EffectConvertIO io;
    io.assetId = [&](const EffectGuid& stored, uint32_t type) -> uint32_t {
      const auto child = children.find(stored);
      if (child != children.end() && child->second.type == type) {
        return child->second.id;
      }
      return Stored(stored, type);
    };
    io.materialTexture = [&](const EffectGuid& material) { return Material(material); };
    io.flipbook = [&](const EffectGuid& stored) { return Flipbook(Swap(stored)); };
    const std::vector<ConvertedPart> parts = ConvertEffect(effect, data.data(), io);
    std::vector<RetailPartProperty> check;
    if (parts.empty() || !SplitRetailPart(parts[0].part.data(), parts[0].part.size(), check, error)) {
      ++m_result.failed;
      Log(name + ": the converted effect does not read as a PART");
      return;
    }
    // A part that lost its texture or model would replace the disc's textured
    // effect with an invisible one: keep the disc's, before anything is written.
    for (const ConvertedPart& part : parts) {
      const EffectNode* node = part.root ? &effect : FindNode(effect, part.id);
      if (node != nullptr && SplitRetailPart(part.part.data(), part.part.size(), check, error) &&
          LostLook(*node, check)) {
        ++m_result.failed;
        Log(name + ": " + (part.root ? std::string("the root") : "child " + EffectGuidString(part.id)) +
            " has no texture, the disc's is kept");
        return;
      }
    }
    // Children first, so the root never names a child that was not written.
    for (size_t i = 1; i < parts.size(); ++i) {
      const auto child = children.find(parts[i].id);
      if (child == children.end() ||
          !SplitRetailEffect(parts[i].type, parts[i].part.data(), parts[i].part.size(), check, error) ||
          !m_io.write(Hex(child->second.id) + "." + EffectFourCCString(parts[i].type), parts[i].part)) {
        // The root would name a missing child: leave the disc's PART in place.
        ++m_result.failed;
        Log(name + ": child " + EffectGuidString(parts[i].id) + " not written");
        return;
      }
      ++m_result.parts;
      m_result.dropped += parts[i].droppedRetail;
    }
    std::vector<uint8_t> root = parts[0].part;
    std::vector<RetailPartProperty> converted;
    std::vector<RetailPartProperty> disc;
    std::vector<uint8_t> discData;
    if (m_io.retail && SplitRetailPart(root.data(), root.size(), converted, error) && HasLight(converted) &&
        m_io.retail(kPart, *retail, discData) &&
        SplitRetailPart(discData.data(), discData.size(), disc, error) && HasLight(disc)) {
      root = WithDiscLight(converted, disc);
      Log(name + ": light from the disc");
    }
    if (!m_io.write(name, root)) {
      ++m_result.failed;
      Log(name + ": could not write it");
      return;
    }
    ++m_result.parts;
    ++m_result.written;
    m_result.dropped += parts[0].droppedRetail;
  }

  EffectImportResult Run() {
    for (const EffectGuid& id : m_io.effects) {
      Effect(id);
    }
    return m_result;
  }

private:
  const EffectImportIO& m_io;
  std::map<EffectGuid, uint32_t> m_textures;  // by Remastered id, 0 for one that failed
  std::map<EffectGuid, FlipbookAtlas> m_flipbooks;
  std::map<EffectGuid, uint32_t> m_models;
  EffectImportResult m_result;
};

}  // namespace

namespace {
std::atomic<bool> sEffects{false};
}  // namespace

bool WantsRemasteredEffects() {
  const char* env = std::getenv("MP_REMASTERED_EFFECTS");
  if (env == nullptr || env[0] == '\0') {
    return sEffects.load();
  }
  std::string value(env);
  for (char& c : value) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (value == "1" || value == "true" || value == "on" || value == "yes") {
    return true;
  }
  if (value == "0" || value == "false" || value == "off" || value == "no") {
    return false;
  }
  return sEffects.load();
}

void SetImportEffects(bool on) { sEffects = on; }

EffectImportResult ImportEffects(const EffectImportIO& io) { return Importer(io).Run(); }

}  // namespace PortRemastered
