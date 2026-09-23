#pragma once

struct ActiveEffect;
struct bhkCharacterController;
struct BGSLoadFormBuffer;

struct InventoryEntry;

struct MiddleProcess
{
    // void SaveActiveEffects()
    void LoadActiveEffects(BGSLoadFormBuffer* apLoadGameBuffer);

    // 0xB0 - pitch
    uint8_t pad0[0xB8];
    float direction; // B8
    uint8_t padBC[0x1A0 - 0xBC];
    GameList<ActiveEffect>* ActiveEffects;
    uint8_t pad1A8[0x218 - 0x1A8];
    BSPointerHandle<TESObjectREFR> commandingActor;
    uint8_t pad21C[0x220 - 0x21C];
    InventoryEntry* leftEquippedObject;
    uint8_t pad228[0x250 - 0x228];
    // The actor's havok character controller. CommonLibVR's MiddleHighProcessData puts it here for VR, and every
    // neighbouring offset this struct already asserts agrees with that header (rotation 0B0, activeEffects 1A0,
    // commandingActor 218, leftHand 220, rightHand 260), so this is read rather than called.
    bhkCharacterController* charController;
    uint8_t pad258[0x260 - 0x258];
    InventoryEntry* rightEquippedObject;
    InventoryEntry* ammoEquippedObject; // could be more than just ammo
    // 0xB8 - direction
    //
    // 0x326 - bool lookat
};

static_assert(offsetof(MiddleProcess, direction) == 0xB8);
static_assert(offsetof(MiddleProcess, leftEquippedObject) == 0x220);
static_assert(offsetof(MiddleProcess, charController) == 0x250);
static_assert(offsetof(MiddleProcess, rightEquippedObject) == 0x260);
