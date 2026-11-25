// Included after the game's guarded memory reader.
constexpr uintptr_t kClientRootSlot=0x72451C0;

struct CatchControl {
    uintptr_t actor=0;
    uintptr_t context=0;
    uintptr_t token=0;
};

bool SameControl(const CatchControl& a,const CatchControl& b) {
    return a.actor==b.actor && a.context==b.context && a.token==b.token;
}

CatchControl ReadCatchControl(uintptr_t actor) {
    // Native CatchUpdate (6747BC..6747FF) compares the current context's
    // control token to the actor's token and requires the actor's local flag.
    // The context's +50 actor pointer is a different field.
    uintptr_t root=0,context=0,token=0,actorToken=0,again=0;
    uint8_t local=0;
    if (!actor || !Get(TargetAddress(kClientRootSlot),root) || !root ||
        !Get(root+0x30,context) || !context ||
        !Get(context+0x58,token) || !token ||
        !Get(actor+0xA0,actorToken) || token!=actorToken ||
        !Get(actor+0x94,local) || !local ||
        !Get(context+0x58,again) || again!=token) return {};
    return {actor,context,token};
}
