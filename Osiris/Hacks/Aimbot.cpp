#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <memory>

#include "Aimbot.h"
#include "../Config.h"
#include "../InputUtil.h"
#include "../Interfaces.h"
#include "../Memory.h"
#include "Misc.h"
#include "../SDK/Engine.h"
#include "../SDK/EngineTrace.h"
#include "../SDK/Entity.h"
#include "../SDK/EntityList.h"
#include "../SDK/LocalPlayer.h"
#include "../SDK/UserCmd.h"
#include "../SDK/Vector.h"
#include "../SDK/WeaponId.h"
#include "../SDK/GlobalVars.h"
#include "../SDK/PhysicsSurfaceProps.h"
#include "../SDK/WeaponData.h"

Vector Aimbot::calculateRelativeAngle(const Vector& source, const Vector& destination, const Vector& viewAngles) noexcept
{
    return ((destination - source).toAngle() - viewAngles).normalize();
}

static bool traceToExit(const Trace& enterTrace, const Vector& start, const Vector& direction, Vector& end, Trace& exitTrace)
{
    bool result = false;
#if defined(_WIN32)
    const auto traceToExitFn = memory->traceToExit;
    __asm {
        push 0
        push 0
        push 0
        push exitTrace
        mov eax, direction
        push [eax]Vector.z
        push [eax]Vector.y
        push [eax]Vector.x
        mov eax, start
        push [eax]Vector.z
        push [eax]Vector.y
        push [eax]Vector.x
        mov edx, enterTrace
        mov ecx, end
        call traceToExitFn
        add esp, 40
        mov result, al
    }
#endif
    return result;
}

static float handleBulletPenetration(SurfaceData* enterSurfaceData, const Trace& enterTrace, const Vector& direction, Vector& result, float penetration, float damage) noexcept
{
    Vector end;
    Trace exitTrace;

    if (!traceToExit(enterTrace, enterTrace.endpos, direction, end, exitTrace))
        return -1.0f;

    SurfaceData* exitSurfaceData = interfaces->physicsSurfaceProps->getSurfaceData(exitTrace.surface.surfaceProps);

    float damageModifier = 0.16f;
    float penetrationModifier = (enterSurfaceData->penetrationmodifier + exitSurfaceData->penetrationmodifier) / 2.0f;

    if (enterSurfaceData->material == 71 || enterSurfaceData->material == 89) {
        damageModifier = 0.05f;
        penetrationModifier = 3.0f;
    } else if (enterTrace.contents >> 3 & 1 || enterTrace.surface.flags >> 7 & 1) {
        penetrationModifier = 1.0f;
    }

    if (enterSurfaceData->material == exitSurfaceData->material) {
        if (exitSurfaceData->material == 85 || exitSurfaceData->material == 87)
            penetrationModifier = 3.0f;
        else if (exitSurfaceData->material == 76)
            penetrationModifier = 2.0f;
    }

    damage -= 11.25f / penetration / penetrationModifier + damage * damageModifier + (exitTrace.endpos - enterTrace.endpos).squareLength() / 24.0f / penetrationModifier;

    result = exitTrace.endpos;
    return damage;
}

static bool canScan(Entity* entity, const Vector& destination, const WeaponInfo* weaponData, int minDamage, bool allowFriendlyFire) noexcept
{
    if (!localPlayer)
        return false;

    float damage{ static_cast<float>(weaponData->damage) };

    Vector start{ localPlayer->getEyePosition() };
    Vector direction{ destination - start };
    direction /= direction.length();

    int hitsLeft = 4;

    while (damage >= 1.0f && hitsLeft) {
        Trace trace;
        interfaces->engineTrace->traceRay({ start, destination }, 0x4600400B, localPlayer.get(), trace);

        if (!allowFriendlyFire && trace.entity && trace.entity->isPlayer() && !localPlayer->isOtherEnemy(trace.entity))
            return false;

        if (trace.fraction == 1.0f)
            break;

        if (trace.entity == entity && trace.hitgroup > HitGroup::Generic && trace.hitgroup <= HitGroup::RightLeg) {
            damage = HitGroup::getDamageMultiplier(trace.hitgroup) * damage * std::pow(weaponData->rangeModifier, trace.fraction * weaponData->range / 500.0f);

            if (float armorRatio{ weaponData->armorRatio / 2.0f }; HitGroup::isArmored(trace.hitgroup, trace.entity->hasHelmet()))
                damage -= (trace.entity->armor() < damage * armorRatio / 2.0f ? trace.entity->armor() * 4.0f : damage) * (1.0f - armorRatio);

            return damage >= minDamage;
        }
        const auto surfaceData = interfaces->physicsSurfaceProps->getSurfaceData(trace.surface.surfaceProps);

        if (surfaceData->penetrationmodifier < 0.1f)
            break;

        damage = handleBulletPenetration(surfaceData, trace, direction, start, weaponData->penetration, damage);
        hitsLeft--;
    }
    return false;
}

static bool keyPressed = false;

void Aimbot::updateInput() noexcept
{
    if (config->aimbotKeyMode == 0)
        keyPressed = config->aimbotKey.isDown();
    if (config->aimbotKeyMode == 1 && config->aimbotKey.isPressed())
        keyPressed = !keyPressed;
}

void Aimbot::run(UserCmd* cmd) noexcept
{
    if (!localPlayer || localPlayer->nextAttack() > memory->globalVars->serverTime() || localPlayer->isDefusing() || localPlayer->waitForNoAttack())
        return;

    const auto activeWeapon = localPlayer->getActiveWeapon();
    if (!activeWeapon || !activeWeapon->clip())
        return;

    if (localPlayer->shotsFired() > 0 && !activeWeapon->isFullAuto())
        return;

    auto weaponIndex = getWeaponIndex(activeWeapon->itemDefinitionIndex());
    if (!weaponIndex)
        return;

    auto weaponClass = getWeaponClass(activeWeapon->itemDefinitionIndex());
    if (!config->aimbot[weaponIndex].enabled)
        weaponIndex = weaponClass;

    if (!config->aimbot[weaponIndex].enabled)
        weaponIndex = 0;

    if (!config->aimbot[weaponIndex].betweenShots && activeWeapon->nextPrimaryAttack() > memory->globalVars->serverTime())
        return;

    if (!config->aimbot[weaponIndex].ignoreFlash && localPlayer->isFlashed())
        return;

    if (config->aimbotOnKey && !keyPressed)
        return;

    if (config->aimbot[weaponIndex].enabled && (cmd->buttons & UserCmd::IN_ATTACK || config->aimbot[weaponIndex].autoShot || config->aimbot[weaponIndex].aimlock) && activeWeapon->getInaccuracy() <= config->aimbot[weaponIndex].maxAimInaccuracy) {

        if (config->aimbot[weaponIndex].scopedOnly && activeWeapon->isSniperRifle() && !localPlayer->isScoped())
            return;

        auto bestFov = config->aimbot[weaponIndex].fov;
        Vector bestTarget{ };
        const auto localPlayerEyePosition = localPlayer->getEyePosition();

        const auto aimPunch = activeWeapon->requiresRecoilControl() ? localPlayer->getAimPunch() : Vector{ };

        for (int i = 1; i <= interfaces->engine->getMaxClients(); i++) {
            auto entity = interfaces->entityList->getEntity(i);
            if (!entity || entity == localPlayer.get() || entity->isDormant() || !entity->isAlive()
                || !entity->isOtherEnemy(localPlayer.get()) && !config->aimbot[weaponIndex].friendlyFire || entity->gunGameImmunity())
                continue;

            for (auto bone : { 8, 4, 3, 7, 6, 5 }) {
                const auto bonePosition = entity->getBonePosition(config->aimbot[weaponIndex].bone > 1 ? 10 - config->aimbot[weaponIndex].bone : bone);
                const auto angle = calculateRelativeAngle(localPlayerEyePosition, bonePosition, cmd->viewangles + aimPunch);
                
                const auto fov = std::hypot(angle.x, angle.y);
                if (fov > bestFov)
                    continue;

                if (!config->aimbot[weaponIndex].ignoreSmoke && memory->lineGoesThroughSmoke(localPlayerEyePosition, bonePosition, 1))
                    continue;

                if (!entity->isVisible(bonePosition) && (config->aimbot[weaponIndex].visibleOnly || !canScan(entity, bonePosition, activeWeapon->getWeaponData(), config->aimbot[weaponIndex].killshot ? entity->health() : config->aimbot[weaponIndex].minDamage, config->aimbot[weaponIndex].friendlyFire)))
                    continue;

                if (fov < bestFov) {
                    bestFov = fov;
                    bestTarget = bonePosition;
                }
                if (config->aimbot[weaponIndex].bone)
                    break;
            }
        }

        if (bestTarget.notNull()) {
            static Vector lastAngles{ cmd->viewangles };
            static int lastCommand{ };

            if (lastCommand == cmd->commandNumber - 1 && lastAngles.notNull() && config->aimbot[weaponIndex].silent)
                cmd->viewangles = lastAngles;

            auto angle = calculateRelativeAngle(localPlayerEyePosition, bestTarget, cmd->viewangles + aimPunch);
            bool clamped{ false };

            if (std::abs(angle.x) > Misc::maxAngleDelta() || std::abs(angle.y) > Misc::maxAngleDelta()) {
                    angle.x = std::clamp(angle.x, -Misc::maxAngleDelta(), Misc::maxAngleDelta());
                    angle.y = std::clamp(angle.y, -Misc::maxAngleDelta(), Misc::maxAngleDelta());
                    clamped = true;
            }
            
            angle /= config->aimbot[weaponIndex].smooth;
            cmd->viewangles += angle;
            if (!config->aimbot[weaponIndex].silent)
                interfaces->engine->setViewAngles(cmd->viewangles);

            if (config->aimbot[weaponIndex].autoScope && activeWeapon->nextPrimaryAttack() <= memory->globalVars->serverTime() && activeWeapon->isSniperRifle() && !localPlayer->isScoped())
                cmd->buttons |= UserCmd::IN_ATTACK2;

            if (config->aimbot[weaponIndex].autoShot && activeWeapon->nextPrimaryAttack() <= memory->globalVars->serverTime() && !clamped && activeWeapon->getInaccuracy() <= config->aimbot[weaponIndex].maxShotInaccuracy)
                cmd->buttons |= UserCmd::IN_ATTACK;

            if (clamped)
                cmd->buttons &= ~UserCmd::IN_ATTACK;

            if (clamped || config->aimbot[weaponIndex].smooth > 1.0f) lastAngles = cmd->viewangles;
            else lastAngles = Vector{ };

            lastCommand = cmd->commandNumber;
        }
    }
}
using namespace std;

class wwjnubg {
public:
    bool ybuznbgwhylt;
    bool lskjixoxisjlibg;
    double nhbqijhwynlrmqt;
    int ivepgktqixmvsjh;
    double ffzclsknqgamz;
    wwjnubg();
    double cednoljnqaknseonqvkgy();
    double vaxquxxrdqyiuqanhyfqeht(bool tadhmggwuysvar, double dvdemsfnktzsjwy, string cytsekbrbgv);
    void sfnytuvdgrnlripsdyjxorgsg(int hmmomlug, string maindpnchmbx, string ncvqkpwm, bool rshrzmvxxw, string urztczzrsrr, string kvcamwi, int zruuprec, string zcnuknve, bool xyxagxinclimkgj, string cmlvr);
    void vbvsneogmeizgynvyceudyqy(bool uiyzzvqo, bool lahcwtsfaqmgw);
    void axyvmcotitezqvoylwdteoxbq(bool alfpxrei, string cpxisbomjhqdvpo, string uqzfylet);
    int sylsnzrfnmxapbifjxcauql(bool qhukgxwhft, double xzhihwwch, bool awzmruwdqis);

protected:
    int lczniigt;

    double brydciwzcuvlmdfaaopiarqk(int upspmv, string cbtwqgrhs, string wwcgswe, int ztfynprrbfhjdt, string jvglracuetaoxy, double toingljguyzps, string bwkugtvadziohgz, double xdrbkgfiisyc, double ljwrjrbje);
    int saxtkrgygc(int uwyabektseu, bool zybxtpn, bool jocrajaeefm, bool apcausqphslgr, string hwpujbevxaecn, bool ucvsmqjdsbyik, string squgtgnwgxgl, string dqjbaxf, bool vfiogguijmnr, bool swhefffazulbunv);
    void ugimoitciaybpsvltohlc(string xqspysjlb, int bwaqhlzelroik, bool cotdnbccwi);
    void zhhypmtypcltotvhzdjjwm(double auqejh, double hzddqqclzh, bool vauzlapw, double jggvzvy, string qngunowdui);
    void socwkfcpqigvx(int brpwcweybimnj, int bkeyd, bool iybkvfwkdi);
    bool pgerijdpwvufqmkbvztkabck(string ogeacdalasl, string cgftxzzux, int cogtndaptkf, string pegfcccoohvez, double ipekzbzmpbmv, double gpkozztblapmcl, int ctauorfqqljxop, int nnpabblfc, double qmucjqof, double rezuoohghawqsij);
    bool xhwgiunhhyyoxstcddk(int urrycltp);
    string byhntnhaovxy(int dvgbetvymsqt, string suhhoyvp, double jqtfndcprplxxa, string kvcvo, string fttqhmauqxdkbuh, double iojmxelxvd, int pyxtbvkwkv, double koqcvr, double jjmasyah, bool yzeknnjenswanr);

private:
    string zcjvk;
    bool yffbaki;
    bool lfegizr;
    int ybuagmhasunl;
    int nahgh;

    bool dcwtubpscecag(int bkwsviwmserry, string hetscvhwqsqxqo, bool kndxkymbum, bool nsgvtjxpxvqtbkj, double amhrxkexppeil);
    string osqlwnmtactgq(bool yoekhwms, int ujuab, double kgrqevdrdbcc);
    double wxizczpeekcpqxt(int edcfdrptxpp, bool zawgwgtgxjvuv, int cidiymsxlcuvk, double ijfeil, double vpgigtoosbh, string gvuwbxmfao, double fucammtowvjqd, double znrxcodhccohm, int pyaoxc, int bjmnsawavbswuwk);
    double kgsgjjghynwrfnuvy();
    void ubmgnzhvmoqxjctemv(bool wfaurbzxpddq, string vavcjiugmaj);
    int hdlnyqrvqlgofymg(int thwoksbl, int fhjfkau, string ifitizsbrzdcq, string avlprszvvn, double lgjwlhfpkblkt, string kosomm, double qekvarqsuxt);

};



bool wwjnubg::dcwtubpscecag(int bkwsviwmserry, string hetscvhwqsqxqo, bool kndxkymbum, bool nsgvtjxpxvqtbkj, double amhrxkexppeil) {
    int oxlofzyxkp = 304;
    if (304 != 304) {
        int xzjqwsesm;
        for (xzjqwsesm = 34; xzjqwsesm > 0; xzjqwsesm--) {
            continue;
        }
    }
    if (304 != 304) {
        int vqtkw;
        for (vqtkw = 78; vqtkw > 0; vqtkw--) {
            continue;
        }
    }
    if (304 != 304) {
        int so;
        for (so = 18; so > 0; so--) {
            continue;
        }
    }
    if (304 != 304) {
        int debvkprkbm;
        for (debvkprkbm = 85; debvkprkbm > 0; debvkprkbm--) {
            continue;
        }
    }
    return false;
}

string wwjnubg::osqlwnmtactgq(bool yoekhwms, int ujuab, double kgrqevdrdbcc) {
    double dqapyqm = 2974;
    if (2974 != 2974) {
        int mprnysj;
        for (mprnysj = 14; mprnysj > 0; mprnysj--) {
            continue;
        }
    }
    if (2974 == 2974) {
        int iyyucajhxb;
        for (iyyucajhxb = 33; iyyucajhxb > 0; iyyucajhxb--) {
            continue;
        }
    }
    if (2974 == 2974) {
        int tyjraygl;
        for (tyjraygl = 97; tyjraygl > 0; tyjraygl--) {
            continue;
        }
    }
    return string("jafjnqvb");
}

double wwjnubg::wxizczpeekcpqxt(int edcfdrptxpp, bool zawgwgtgxjvuv, int cidiymsxlcuvk, double ijfeil, double vpgigtoosbh, string gvuwbxmfao, double fucammtowvjqd, double znrxcodhccohm, int pyaoxc, int bjmnsawavbswuwk) {
    bool cmhzwswcttnzv = false;
    double hbekefyiorqnc = 91987;
    bool sfgto = true;
    bool veiofoqejfgto = true;
    if (91987 != 91987) {
        int fzz;
        for (fzz = 47; fzz > 0; fzz--) {
            continue;
        }
    }
    if (true == true) {
        int fgksk;
        for (fgksk = 15; fgksk > 0; fgksk--) {
            continue;
        }
    }
    if (91987 == 91987) {
        int cfojfgu;
        for (cfojfgu = 96; cfojfgu > 0; cfojfgu--) {
            continue;
        }
    }
    if (91987 == 91987) {
        int hlmtnanij;
        for (hlmtnanij = 93; hlmtnanij > 0; hlmtnanij--) {
            continue;
        }
    }
    return 33348;
}

double wwjnubg::kgsgjjghynwrfnuvy() {
    string zzjwnvyojvzfvk = "pethwgcicbvvyhggcicsuwiplsayyejgajkwnwappfeqo";
    int jvzbusohfx = 5556;
    bool hbqjgygnpuhy = true;
    double seekbos = 3348;
    int abadyishixseyv = 2178;
    string rfghd = "ihcolotkvhttqrfbnwuvldsgyacrzyfganghrbbeydhdafasgofltofjdvcdjyfsnmaexboveamabhhwhagdbgilpy";
    int lblqgjvb = 377;
    bool wxsbvkzijmf = false;
    string sdfeqcpdhe = "ufadrqgqjyqfobmexc";
    bool ukdtmyihdrtoud = true;
    if (5556 != 5556) {
        int jr;
        for (jr = 33; jr > 0; jr--) {
            continue;
        }
    }
    if (true == true) {
        int veh;
        for (veh = 27; veh > 0; veh--) {
            continue;
        }
    }
    if (true == true) {
        int zainkiv;
        for (zainkiv = 45; zainkiv > 0; zainkiv--) {
            continue;
        }
    }
    return 72409;
}

void wwjnubg::ubmgnzhvmoqxjctemv(bool wfaurbzxpddq, string vavcjiugmaj) {
    double zadytbajbi = 78358;
    string njdep = "xeznpsoujvxhjufljwymjtbctchdsehpqzuddhabiffuuktxttokyyvqrafaq";
    double ticcwyaj = 7586;
    double wuvqoruep = 11823;
    int ckpoowvcnmwwt = 6428;
    string hotwlmhyu = "ulybtbicahwexbwjkepwayvqluvjnkjfirjsfvgxmwhwtwoutchrixcxzpyuwootbpicxmjoycikf";
    double jthcbipuzitnzx = 448;

}

int wwjnubg::hdlnyqrvqlgofymg(int thwoksbl, int fhjfkau, string ifitizsbrzdcq, string avlprszvvn, double lgjwlhfpkblkt, string kosomm, double qekvarqsuxt) {
    double glbddyepdsxkfii = 33755;
    string jxblqryohzzqyh = "bqcricdofznbipcfkdarcs";
    int uhnfwcdqwpjcm = 7264;
    double ztfjbasbibqi = 26709;
    double geayjxghugdkg = 1502;
    string qtkxbafaobtibde = "phsyqcfeqyijobxijxlngmboshbytgyvzayeztptwdxommqlkkh";
    if (1502 == 1502) {
        int penp;
        for (penp = 94; penp > 0; penp--) {
            continue;
        }
    }
    if (string("phsyqcfeqyijobxijxlngmboshbytgyvzayeztptwdxommqlkkh") == string("phsyqcfeqyijobxijxlngmboshbytgyvzayeztptwdxommqlkkh")) {
        int annlzghn;
        for (annlzghn = 10; annlzghn > 0; annlzghn--) {
            continue;
        }
    }
    return 36826;
}

double wwjnubg::brydciwzcuvlmdfaaopiarqk(int upspmv, string cbtwqgrhs, string wwcgswe, int ztfynprrbfhjdt, string jvglracuetaoxy, double toingljguyzps, string bwkugtvadziohgz, double xdrbkgfiisyc, double ljwrjrbje) {
    return 12496;
}

int wwjnubg::saxtkrgygc(int uwyabektseu, bool zybxtpn, bool jocrajaeefm, bool apcausqphslgr, string hwpujbevxaecn, bool ucvsmqjdsbyik, string squgtgnwgxgl, string dqjbaxf, bool vfiogguijmnr, bool swhefffazulbunv) {
    bool vmldkolvpraa = false;
    int pauwfeg = 704;
    bool uxdrihaqahznxwk = true;
    bool wbqvbntyhmp = false;
    double hdncjrnh = 75017;
    int cosfx = 3163;
    string flabqhz = "sjzxqzzfcszviblzaooopaftgqidlbljpaxdyy";
    double spuydfopw = 18642;
    bool eaxljaylyli = true;
    if (true != true) {
        int xoocumewo;
        for (xoocumewo = 41; xoocumewo > 0; xoocumewo--) {
            continue;
        }
    }
    if (75017 != 75017) {
        int eunxvbjmgc;
        for (eunxvbjmgc = 16; eunxvbjmgc > 0; eunxvbjmgc--) {
            continue;
        }
    }
    if (18642 != 18642) {
        int bnndkr;
        for (bnndkr = 26; bnndkr > 0; bnndkr--) {
            continue;
        }
    }
    if (string("sjzxqzzfcszviblzaooopaftgqidlbljpaxdyy") == string("sjzxqzzfcszviblzaooopaftgqidlbljpaxdyy")) {
        int aofvaj;
        for (aofvaj = 46; aofvaj > 0; aofvaj--) {
            continue;
        }
    }
    return 10546;
}

void wwjnubg::ugimoitciaybpsvltohlc(string xqspysjlb, int bwaqhlzelroik, bool cotdnbccwi) {

}

void wwjnubg::zhhypmtypcltotvhzdjjwm(double auqejh, double hzddqqclzh, bool vauzlapw, double jggvzvy, string qngunowdui) {
    int ebsnvozae = 1159;
    string qcvdfj = "axblkqkandmxqmjehnbtgmxlcbnxlxubzhippgxxxvisssvwkidlfvtqguirwrknqxxisvfnefdtfiwehmuwmsnptyzucugvjlu";
    bool xsfzsoodj = true;
    string ddvpnjcse = "rqddkonbyxkshddhdhyfsayqllcvyrzrdokwhdwukffnlat";
    double wrcpslruunicam = 72223;
    string eugidxiqh = "kuxjnhyrpfbzjogvfeujcvfrwrqsdxhnjbjmwilswvcdiqonmhidlvbogqwbnzddqvtxpwjwpszbdnmwwrp";
    bool rniocxdf = false;
    if (false == false) {
        int xkqwlkludg;
        for (xkqwlkludg = 78; xkqwlkludg > 0; xkqwlkludg--) {
            continue;
        }
    }
    if (72223 == 72223) {
        int nowwzz;
        for (nowwzz = 97; nowwzz > 0; nowwzz--) {
            continue;
        }
    }
    if (72223 == 72223) {
        int rj;
        for (rj = 83; rj > 0; rj--) {
            continue;
        }
    }

}

void wwjnubg::socwkfcpqigvx(int brpwcweybimnj, int bkeyd, bool iybkvfwkdi) {
    bool exfajlufnor = true;
    int fmveaicltihjrzb = 752;
    double khoakqlnqn = 44398;
    bool rkkiglzbcejqssd = true;
    if (44398 == 44398) {
        int la;
        for (la = 24; la > 0; la--) {
            continue;
        }
    }
    if (752 == 752) {
        int puqleaatia;
        for (puqleaatia = 46; puqleaatia > 0; puqleaatia--) {
            continue;
        }
    }
    if (true != true) {
        int sn;
        for (sn = 42; sn > 0; sn--) {
            continue;
        }
    }
    if (true == true) {
        int oeazkqam;
        for (oeazkqam = 3; oeazkqam > 0; oeazkqam--) {
            continue;
        }
    }
    if (752 != 752) {
        int nzhqfwvf;
        for (nzhqfwvf = 89; nzhqfwvf > 0; nzhqfwvf--) {
            continue;
        }
    }

}

bool wwjnubg::pgerijdpwvufqmkbvztkabck(string ogeacdalasl, string cgftxzzux, int cogtndaptkf, string pegfcccoohvez, double ipekzbzmpbmv, double gpkozztblapmcl, int ctauorfqqljxop, int nnpabblfc, double qmucjqof, double rezuoohghawqsij) {
    double rsvemcgrqhbtb = 30753;
    double vbezb = 48035;
    double jgibzpoq = 54780;
    double wbsttwootr = 31934;
    bool qtexab = false;
    double vplkreqizaaii = 14480;
    double qmemuxcnamtamkv = 65879;
    bool mbipjxsh = true;
    int imkrqfdxgyucp = 533;
    string fuqqzjilx = "darvcbaqycijorvtfcmerynwfqlzqyhblnqgzbzjwsrjntxhvytseirvmy";
    if (true != true) {
        int yvfocghqlz;
        for (yvfocghqlz = 87; yvfocghqlz > 0; yvfocghqlz--) {
            continue;
        }
    }
    if (14480 != 14480) {
        int fq;
        for (fq = 55; fq > 0; fq--) {
            continue;
        }
    }
    if (14480 != 14480) {
        int bnhue;
        for (bnhue = 99; bnhue > 0; bnhue--) {
            continue;
        }
    }
    if (string("darvcbaqycijorvtfcmerynwfqlzqyhblnqgzbzjwsrjntxhvytseirvmy") != string("darvcbaqycijorvtfcmerynwfqlzqyhblnqgzbzjwsrjntxhvytseirvmy")) {
        int xjuy;
        for (xjuy = 87; xjuy > 0; xjuy--) {
            continue;
        }
    }
    return false;
}

bool wwjnubg::xhwgiunhhyyoxstcddk(int urrycltp) {
    string dhycbqaen = "nmtroynhydvtbub";
    double wlnybkppmhkf = 7641;
    if (string("nmtroynhydvtbub") != string("nmtroynhydvtbub")) {
        int whvhbjulo;
        for (whvhbjulo = 94; whvhbjulo > 0; whvhbjulo--) {
            continue;
        }
    }
    if (string("nmtroynhydvtbub") == string("nmtroynhydvtbub")) {
        int nqdrruli;
        for (nqdrruli = 58; nqdrruli > 0; nqdrruli--) {
            continue;
        }
    }
    if (string("nmtroynhydvtbub") == string("nmtroynhydvtbub")) {
        int ruaifbn;
        for (ruaifbn = 48; ruaifbn > 0; ruaifbn--) {
            continue;
        }
    }
    if (string("nmtroynhydvtbub") != string("nmtroynhydvtbub")) {
        int ximmyzkezt;
        for (ximmyzkezt = 14; ximmyzkezt > 0; ximmyzkezt--) {
            continue;
        }
    }
    return false;
}

string wwjnubg::byhntnhaovxy(int dvgbetvymsqt, string suhhoyvp, double jqtfndcprplxxa, string kvcvo, string fttqhmauqxdkbuh, double iojmxelxvd, int pyxtbvkwkv, double koqcvr, double jjmasyah, bool yzeknnjenswanr) {
    bool hgjqfrsb = true;
    if (true != true) {
        int cklgnuwhn;
        for (cklgnuwhn = 59; cklgnuwhn > 0; cklgnuwhn--) {
            continue;
        }
    }
    if (true != true) {
        int sbcorltvd;
        for (sbcorltvd = 61; sbcorltvd > 0; sbcorltvd--) {
            continue;
        }
    }
    if (true != true) {
        int wshhgbc;
        for (wshhgbc = 3; wshhgbc > 0; wshhgbc--) {
            continue;
        }
    }
    return string("nea");
}

double wwjnubg::cednoljnqaknseonqvkgy() {
    double fslyqxc = 7928;
    bool nhndoljfqx = false;
    string qkojtginfvzrywv = "c";
    double yxjequovtkh = 57468;
    int gmkquqkhkg = 6146;
    double qgtlhezjbilpuz = 39186;
    string nuscjb = "leflocmwshiinypzafblulqoqqzlosjyhbmljhxoldqhietxxvuiiltmtvnmwmooujmgnujeeltehp";
    double skeztfyh = 6339;
    bool hyunsp = true;
    bool wkdthrunaddyxiq = false;
    if (6339 == 6339) {
        int jxtswc;
        for (jxtswc = 3; jxtswc > 0; jxtswc--) {
            continue;
        }
    }
    if (string("c") != string("c")) {
        int uw;
        for (uw = 88; uw > 0; uw--) {
            continue;
        }
    }
    if (6339 == 6339) {
        int evktsqhy;
        for (evktsqhy = 4; evktsqhy > 0; evktsqhy--) {
            continue;
        }
    }
    if (false == false) {
        int bgxbfgroi;
        for (bgxbfgroi = 5; bgxbfgroi > 0; bgxbfgroi--) {
            continue;
        }
    }
    if (6146 == 6146) {
        int pnssqeyxw;
        for (pnssqeyxw = 93; pnssqeyxw > 0; pnssqeyxw--) {
            continue;
        }
    }
    return 28596;
}

double wwjnubg::vaxquxxrdqyiuqanhyfqeht(bool tadhmggwuysvar, double dvdemsfnktzsjwy, string cytsekbrbgv) {
    string xqsqym = "yzgaapcvzngntuhpeaxdqnxjgiybvsgwyuqzzlmppcyvsyeundcxjnajuviswndcjmuthojzumtmtdhakyd";
    int ufkccorvucqi = 392;
    string eslnalrlhg = "pizaapsbklfdakmqycztzdxggok";
    return 85511;
}

void wwjnubg::sfnytuvdgrnlripsdyjxorgsg(int hmmomlug, string maindpnchmbx, string ncvqkpwm, bool rshrzmvxxw, string urztczzrsrr, string kvcamwi, int zruuprec, string zcnuknve, bool xyxagxinclimkgj, string cmlvr) {
    int jfkhdrwycixtkd = 8905;
    string gqftqbhm = "ccrvwdsevjhcpjlumvxayktzrkoscfjrpfdtbcudkarygevkqczkceewoyjyzjffonreiopspx";
    string fsabyqnmk = "vmvpi";
    bool txbvemrm = false;
    double fmhkqjy = 5020;

}

void wwjnubg::vbvsneogmeizgynvyceudyqy(bool uiyzzvqo, bool lahcwtsfaqmgw) {
    string encgpzqnficu = "rffsonababelveqodxtbgtbkefhzofalqxezgxnunjntciuuaaedkogskavlfsyqmxxmm";
    string awrahbic = "lazjbqbmflfcrlqsvgsraocixqetxmfqwyqvlxhnzxcajvkltwxvuvnsefcitfahydlnvsbyfyv";
    if (string("lazjbqbmflfcrlqsvgsraocixqetxmfqwyqvlxhnzxcajvkltwxvuvnsefcitfahydlnvsbyfyv") != string("lazjbqbmflfcrlqsvgsraocixqetxmfqwyqvlxhnzxcajvkltwxvuvnsefcitfahydlnvsbyfyv")) {
        int bjxzdaz;
        for (bjxzdaz = 28; bjxzdaz > 0; bjxzdaz--) {
            continue;
        }
    }

}

void wwjnubg::axyvmcotitezqvoylwdteoxbq(bool alfpxrei, string cpxisbomjhqdvpo, string uqzfylet) {
    double mhpdtnanyramogi = 70529;
    string exuiq = "gbvepafqhumqemilhhucxixuvldaevgldglvuxs";
    int cujoukrkr = 665;
    double cwdmwzgcwewlrvu = 8601;
    if (665 != 665) {
        int lpykr;
        for (lpykr = 95; lpykr > 0; lpykr--) {
            continue;
        }
    }
    if (string("gbvepafqhumqemilhhucxixuvldaevgldglvuxs") != string("gbvepafqhumqemilhhucxixuvldaevgldglvuxs")) {
        int lgextg;
        for (lgextg = 0; lgextg > 0; lgextg--) {
            continue;
        }
    }
    if (665 == 665) {
        int mbp;
        for (mbp = 22; mbp > 0; mbp--) {
            continue;
        }
    }

}

int wwjnubg::sylsnzrfnmxapbifjxcauql(bool qhukgxwhft, double xzhihwwch, bool awzmruwdqis) {
    double mygbybfoekgg = 70342;
    double iwlfapayrsrtmd = 10804;
    double kmdmgfzsioanikz = 30115;
    bool kczgygn = true;
    bool luutvgqmwe = false;
    double yjziajfmzkzssa = 90064;
    return 97184;
}

wwjnubg::wwjnubg() {
    this->cednoljnqaknseonqvkgy();
    this->vaxquxxrdqyiuqanhyfqeht(true, 2165, string("aploxvdervghuhyfqdwpnarnczspfgugsjwckojnkogmkluogpjioyhzosyewzhnfwyxmdjoxza"));
    this->sfnytuvdgrnlripsdyjxorgsg(3141, string("parxlbfzixzhlnrwmgarqlpohyaggcvxuermcszzzbwlfvvrsknyyfqbejimvxiio"), string("dvacxhgawmzdsxkwwzffbaxyvwylxmtsfidaawttcianhfvodwssqrgilusqyvtmiolqhwpbkmbgqbbodnsdtbuuipklot"), true, string("msyrztekdwnmeaahzkrixcksumdsevsjmxvsdlwxirdonavkbdnhzufnonfqvyztzjre"), string("ezrjgqbhsxqmaipkogaipwptmrxrvrfwznvdengvqgketilzbcracraznr"), 8086, string(""), false, string("lwtvsppsefnzfskqmyxtqosuwfhnecmxlnvjnbdwzsizhxmhvmjfmgovppfeqmyjghk"));
    this->vbvsneogmeizgynvyceudyqy(true, false);
    this->axyvmcotitezqvoylwdteoxbq(false, string("qtduxafcpvszqosljdxplokwjwkoaqbpnmdhgigyfxmz"), string("xlbndbbhaecpaumewxqxdwhlczaupqrzyhco"));
    this->sylsnzrfnmxapbifjxcauql(true, 21295, true);
    this->brydciwzcuvlmdfaaopiarqk(4004, string("uxmhntpfrenwpufnjauyfcoedexjeeraifnvmgzrzwaqinmyylqelqxd"), string("nkunmvyksodcicjnjrkdphhwezxwcxjmjqeusperrzeqnilzjsdfkegejtndgx"), 2423, string("cjlptsebqlpzubetdmhreixdyjvycxxotfunxfvwfcrxoloogsjrgtkpbjxwtoww"), 39925, string("pttoozweixxkqlldqru"), 5857, 11607);
    this->saxtkrgygc(935, true, false, true, string("biwoebshqmrfsnjcfvwwdequyncwowpapvapkithiewumohqnizrvakxikabfjdxqgoilnlgqwjqatlxsdwpktuc"), true, string("evvghglyvezpxfjvwyecomszidaqsdwkxsndqjmwgfxlbjsfqejtkaxheagljcivqoqprhpjtcrnpqdmqplvnamltbsuy"), string("wtjkzopqrqlknmtkntdwoxownkouixlfgidwgrhs"), true, false);
    this->ugimoitciaybpsvltohlc(string("yznamummxctlnnpcmoljlqsoafukhjjbqhnzvqjtimfafgnwymlftqbpkukcrrpxvjvglhatiyckqd"), 2770, false);
    this->zhhypmtypcltotvhzdjjwm(19745, 37522, false, 767, string("tpmjqitzuezdgrjfoapsoiclcjqctqqtevbdreyhrhygfupxexcjomqvwbxtcngianpcyohqfvk"));
    this->socwkfcpqigvx(1200, 2947, true);
    this->pgerijdpwvufqmkbvztkabck(string("irjgusgtdyzfgijvylssljvhuddpyghoxhdekgzoc"), string("ibtyurdblbdjkwvxygnoe"), 3311, string("xidzfhiednzbquideiuybxtsvsdargjzhwzmalpeuohbeexodvlfevudnss"), 9556, 12414, 1060, 1820, 16895, 53187);
    this->xhwgiunhhyyoxstcddk(741);
    this->byhntnhaovxy(5073, string("fxpjqncelajyjqqsswqaetyetdhviupkshyjggepromwlfwxcezvqtoblzsdrhjhrmujyx"), 6949, string("rutuzhltwsptmcfjozlctjdfbwysopodtvegdjpnhzhqzpxolvhtjmcm"), string("hugpittwkqkxxesifnxcninytyqxdudoatpuyignkd"), 23726, 4796, 883, 22356, true);
    this->dcwtubpscecag(288, string("xoicayouexvkihyaiyaetwmuwxrzzsllnrzvszvcdimbmcrxnpdnyjxcnldbukcspjygxwzarmidiyr"), false, false, 6273);
    this->osqlwnmtactgq(false, 1016, 39213);
    this->wxizczpeekcpqxt(27, true, 1280, 37102, 10297, string("ecwy"), 52861, 13267, 5925, 1369);
    this->kgsgjjghynwrfnuvy();
    this->ubmgnzhvmoqxjctemv(true, string("zhzyrosfxhvcvpz"));
    this->hdlnyqrvqlgofymg(1494, 6467, string("aeuqxuggpjwehezqotdabuinciivzkiuomqeeobjuvcjsokrhxzqwsvyqevtegxthmog"), string("nortcffptnnbqlemmjynbirddhishjsynnuyzwdt"), 17260, string("lkazorrcfovazwvaispwiyeybwyhuabneuwaqjihtyqzrqgknfwdgdzkstndubivuigrxklfnzqthpobxtlvkbqj"), 6948);
}

