#pragma once
#include "lighting_reference_pure.h"
#include "../auto_blinker_pure.h"
#include "../tesla_can_fields_pure.h"

#ifndef P4_TLSSC_NR_PRIORITY_YIELD
#define P4_TLSSC_NR_PRIORITY_YIELD(nowMs) (false)
#endif
#ifndef P4_LIGHTING_BODY_TX_SUPPORTED
#define P4_LIGHTING_BODY_TX_SUPPORTED() (true)
#endif

// Lighting & Rear Fog Pilot features on BODY (and CHASSIS brake/status RX).
struct LightingTemplate {
  bool valid=false, sent=false;
  uint32_t ms=0, sentMs=0;
  uint8_t data[8]={}, last[8]={}, counter=0, dlc=8;
};
struct LightingRuntime {
  uint32_t epoch=0;
  LightingTemplate stalk, body, vcCtrl;
  bool vcCtrlBodySeen=false;
  LightingAp1RunPure run={};
  LightingAp1PushPure train={};
  LightingDynHighPure dynamic={};
  LightingHs2Pure hs2={};
  LightingHsTriplePullPure hsGesture={};
  LightingHornFlashPure horn={};
  LightingBrakeSignalPure brake[2]={};
  bool brakeSeen[2]={}; uint32_t brakeMs[2]={};
  uint8_t prevPush=0, prevPull=0, beam5=0, beamD4=0, beam6=3;
  uint32_t carPushMs=0, beam5Ms=0, beam6Ms=0;
  bool dynSent=false, horn3e9Sent=false;
  bool strobeReleaseNeeded=false, lastStrobePullSent=false;
  bool lastHornPullSent=false;
  uint8_t rearFogBurstEdges=0;
  bool rearFogBurstLevel=false, rearFogAsserted=false;
  bool rearFogLampOn=false;
  uint32_t rearFogLampMs=0, lastRearFogEdgeMs=0, lastRearFogAttempt=0;
  uint32_t rearFogRevHoldUntilMs=0, rearFogLastUiZeroMs=0;
  bool rearFogWeAsserted=false, rearFogLevelHold=false, rearFogUiZeroPending=false;
  uint8_t rearFogHoldOffLeft=0, rearFogTurnOffTries=0;
  uint32_t lastBodyAttempt=0, lastStalkAttempt=0, strobeConflicts=0;
};
static portMUX_TYPE lightingSettingsMux=portMUX_INITIALIZER_UNLOCKED;
static LightingRuntime lightingState;
static bool lightingPermission=false;
static uint8_t lightingBrakeMode=0;
static bool lightingStrobeGesture=false;
static uint8_t lightingPilotMode=0; // 0=off, 1=20x burst/gesture, 2=1Hz pilot pulse, 3=steady pilot
static bool lightingReverseRearFog=false;
static bool lightingHornFlash=false;
static uint32_t lightingTxOk=0,lightingTxFail=0;

static bool lightingApSnapshot(uint32_t now) {
  portENTER_CRITICAL(&stateMux);
  const bool ap=gateAPActive, valid=dasAutopilotStateValid;
  const uint8_t state=dasAutopilotState4;
  const uint32_t ms=lastDASStatusMillis;
  portEXIT_CRITICAL(&stateMux);
  // No reference harness exception: missing DAS data always closes the gate.
  return valid && ms!=0u && lightingApGateActivePure(ap,valid,state,ms,now);
}
static void lightingSettingsLoad(){
  Preferences p;uint32_t raw=0;
  if(p.begin("features",true)){raw=p.getUInt("lighting",0);p.end();}
  if((raw&~0x1FFu)==0u&&(raw&0x100u)&&((raw>>1)&3u)<=2u){
    lightingPermission=(raw&1u)!=0;
    lightingBrakeMode=(raw>>1)&3u;
    lightingStrobeGesture=(raw&0x08u)!=0;
    lightingPilotMode=(raw>>4)&3u;
    lightingReverseRearFog=(raw&0x40u)!=0;
    lightingHornFlash=(raw&0x80u)!=0;
  }
}
static bool lightingSettingsSaveAll(bool permission,uint8_t brakeMode,bool strobeGesture,uint8_t pilotMode,bool reverseRearFog,bool hornFlash){
  if(brakeMode>2u||pilotMode>3u||!canTxBarrierMutex||xSemaphoreTake(canTxBarrierMutex,pdMS_TO_TICKS(100))!=pdTRUE)return false;
  Preferences p;bool saved=false;
  if(p.begin("features",false)){
    const uint32_t value=0x100u|(permission?1u:0u)|(uint32_t(brakeMode)<<1)|(strobeGesture?0x08u:0u)|(uint32_t(pilotMode)<<4)|(reverseRearFog?0x40u:0u)|(hornFlash?0x80u:0u);
    saved=p.putUInt("lighting",value)==sizeof(uint32_t)&&p.getUInt("lighting",0)==value;
    p.end();
  }
  if(saved){
    portENTER_CRITICAL(&lightingSettingsMux);
    if(permission!=lightingPermission||!permission){lightingState.run={};lightingState.train={};}
    if(brakeMode!=lightingBrakeMode)lightingDynHighResetPure(lightingState.dynamic);
    if(!strobeGesture&&lightingStrobeGesture)lightingHsTriplePullResetPure(lightingState.hsGesture);
    if(pilotMode!=lightingPilotMode&&pilotMode!=1u){lightingState.rearFogBurstEdges=0;lightingState.rearFogBurstLevel=false;}
    if(!hornFlash&&lightingHornFlash){
      if(lightingState.horn.active&&lightingState.horn.pull)lightingState.horn.releaseNeeded=true;
      lightingState.horn.active=false;lightingState.horn.pull=false;lightingState.horn.burstEdgesLeft=0u;
    }
    lightingPermission=permission;
    lightingBrakeMode=brakeMode;
    lightingStrobeGesture=strobeGesture;
    lightingPilotMode=pilotMode;
    lightingReverseRearFog=reverseRearFog;
    lightingHornFlash=hornFlash;
    portEXIT_CRITICAL(&lightingSettingsMux);
  }
  xSemaphoreGive(canTxBarrierMutex);return saved;
}
static bool lightingSettingsSaveFull(bool permission,uint8_t brakeMode,bool strobeGesture,uint8_t pilotMode,bool reverseRearFog){
  portENTER_CRITICAL(&lightingSettingsMux);
  const bool hornFlash=lightingHornFlash;
  portEXIT_CRITICAL(&lightingSettingsMux);
  return lightingSettingsSaveAll(permission,brakeMode,strobeGesture,pilotMode,reverseRearFog,hornFlash);
}
static bool lightingSettingsSaveEx(bool permission,uint8_t brakeMode,bool strobeGesture,uint8_t pilotMode){
  portENTER_CRITICAL(&lightingSettingsMux);
  const bool revFog=lightingReverseRearFog;
  portEXIT_CRITICAL(&lightingSettingsMux);
  return lightingSettingsSaveFull(permission,brakeMode,strobeGesture,pilotMode,revFog);
}
static bool lightingSettingsSave(bool permission,uint8_t brakeMode){
  portENTER_CRITICAL(&lightingSettingsMux);
  const bool strobeGesture=lightingStrobeGesture;
  const uint8_t pilotMode=lightingPilotMode;
  portEXIT_CRITICAL(&lightingSettingsMux);
  return lightingSettingsSaveEx(permission,brakeMode,strobeGesture,pilotMode);
}
static bool lightingSetStrobeActive(bool active){
  if(!P4_LIGHTING_BODY_TX_SUPPORTED()||(active&&activeTurnSignalVariant!=TURN_SIGNAL_STALK))return false;
  const uint32_t now=millis();
  portENTER_CRITICAL(&lightingSettingsMux);
  if(active){
    lightingHs2ArmPure(lightingState.hs2,now);
    lightingState.strobeReleaseNeeded=false;
  }else{
    if(lightingState.hs2.armed&&lightingState.hs2.pull)lightingState.strobeReleaseNeeded=true;
    lightingHs2DisarmPure(lightingState.hs2);
  }
  portEXIT_CRITICAL(&lightingSettingsMux);
  return true;
}
static bool lightingTriggerRearFogBurst(){
  if(!P4_LIGHTING_BODY_TX_SUPPORTED())return false;
  portENTER_CRITICAL(&lightingSettingsMux);
  lightingState.rearFogBurstEdges=LIGHTING_REAR_FOG_FLASHES*2u;
  lightingState.rearFogBurstLevel=false;
  lightingState.lastRearFogEdgeMs=0u;
  portEXIT_CRITICAL(&lightingSettingsMux);
  return true;
}
static void lightingObserve(uint8_t bus,uint32_t id,uint8_t dlc,const uint8_t *data,uint32_t epoch,uint32_t ms){
  if(!data||dlc>8u||id>0x7ffu||epoch==0u)return;
  const bool ap=lightingApSnapshot(ms);
  portENTER_CRITICAL(&lightingSettingsMux);
  if(lightingState.epoch!=epoch){lightingState=LightingRuntime{};lightingState.epoch=epoch;}
  auto &s=lightingState;
  if(bus==P4_CHASSIS&&((id==0x145u&&dlc==8u)||(id==0x39du&&dlc==5u))){
    const uint8_t i=id==0x145u?0u:1u;
    s.brake[i]=i==0?lightingDecodeBrake145Pure(data,dlc):lightingDecodeBrake39DPure(data,dlc);s.brakeMs[i]=ms;s.brakeSeen[i]=true;
  }
  if((bus==P4_BODY||bus==P4_CHASSIS)&&id==LIGHTING_ID_REAR_FOG_LAMP&&dlc>=2u){
    // 0x3E3 d1[1:0] VCRIGHT_rearFogLightStatus: 0=OFF, 1=ON, 2=FAULT, 3=SNA
    const bool nowOn=(data[LIGHTING_3E3_D1]&0x03u)==1u;
    if(s.rearFogLampMs!=0u&&s.rearFogLampOn&&!nowOn&&
       s.rearFogLastUiZeroMs!=0u&&lightingAgeOkPure(ms,s.rearFogLastUiZeroMs,600u)&&
       (!s.vcCtrl.sent||!lightingAgeOkPure(ms,s.vcCtrl.sentMs,250u))&&
       (lightingPilotMode==3u||s.rearFogRevHoldUntilMs!=0u)){
      // 收到原厂 0x273=0 后灯立即熄灭，说明本车型为电平保持型，切至持续保持
      s.rearFogLevelHold=true;
    }
    s.rearFogLampOn=nowOn;
    s.rearFogLampMs=ms?ms:1u;
  }
  if(id==LIGHTING_ID_VEHICLE_CONTROL&&dlc>=3u&&dlc<=8u){
    const bool fromBody=(bus==P4_BODY);
    if(fromBody||(!s.vcCtrlBodySeen&&!s.vcCtrl.valid)){
      const bool echo=s.vcCtrl.sent&&lightingAgeOkPure(ms,s.vcCtrl.sentMs,119u)&&memcmp(data,s.vcCtrl.last,dlc)==0;
      if(!echo){
        s.vcCtrl.valid=true;
        s.vcCtrl.ms=ms;
        s.vcCtrl.dlc=dlc;
        if(fromBody)s.vcCtrlBodySeen=true;
        memset(s.vcCtrl.data,0,8);
        memcpy(s.vcCtrl.data,data,dlc);
        if((data[LIGHTING_273_D2]&LIGHTING_273_REAR_FOG_MASK)==0u){
          s.rearFogLastUiZeroMs=ms?ms:1u;
          if(s.rearFogLevelHold&&(lightingPilotMode==3u||s.rearFogRevHoldUntilMs!=0u)){
            s.rearFogUiZeroPending=true;
          }
        }
      }
    }
  }
  if(bus==P4_BODY){
    if(id==0x3f5u&&dlc>=5u){s.beam5=lightingHighBeamControlStatePure(data);s.beamD4=data[4];s.beam5Ms=ms;}
    if(id==0x3f6u&&dlc>=1u){s.beam6=lightingHighBeamLeftStatusPure(data);s.beam6Ms=ms;}
    if(id==LIGHTING_ID_VCLEFT_SWITCH&&dlc==8u){
      if(P4_LIGHTING_BODY_TX_SUPPORTED()){
        bool hornMux0=false;
        const bool hornPressed=lightingVcleftHornPressedPure(data,dlc,hornMux0);
        if(hornMux0){
          lightingHornFlashObservePure(s.horn,lightingHornFlash,hornPressed,true,ms);
        }
        if(lightingPilotMode==1u&&lightingRearFogGesturePure(data,true)){
          if(s.rearFogBurstEdges==0u){
            s.rearFogBurstEdges=LIGHTING_REAR_FOG_FLASHES*2u;
            s.rearFogBurstLevel=false;
            s.lastRearFogEdgeMs=0u;
          }
        }
      }
    }
    if((id==0x249u&&dlc==4u)||(id==0x3e9u&&dlc==8u)){
      auto &t=id==0x249u?s.stalk:s.body;
      const bool stalk=id==0x249u;
      const bool echo=t.sent&&lightingAgeOkPure(ms,t.sentMs,stalk?699u:199u)&&
        (stalk?((data[1]&0xf0u)==(t.last[1]&0xf0u)&&memcmp(data+2,t.last+2,2)==0):memcmp(data,t.last,6)==0);
      const bool checksum=stalk?data[0]==leftStalkChecksumPure(data,data[1]&15u):data[7]==teslaAdditiveChecksumPure(0x3e9u,data);
      if(!echo&&checksum){
        t.valid=true;t.ms=ms;memcpy(t.data,data,dlc);
        if(!stalk||(s.train.left==0&&!s.hs2.armed&&!s.strobeReleaseNeeded))t.counter=stalk?(data[1]&15u):(data[6]>>4);
        if(stalk){
          const uint8_t dir=(data[1]>>4)&3u;
          const bool quiet=!t.sent||!lightingAgeOkPure(ms,t.sentMs,LIGHTING_AP1_TX_QUIET_MS-1u);
          lightingAp1RunStepPure(s.run,lightingPermission&&ap&&activeTurnSignalVariant==TURN_SIGNAL_STALK,dir==2u&&s.prevPush!=2u,quiet,ms);
          s.prevPush=dir;if(dir==2u&&quiet)s.carPushMs=ms;
          if(P4_LIGHTING_BODY_TX_SUPPORTED()&&lightingStrobeGesture&&activeTurnSignalVariant==TURN_SIGNAL_STALK){
            const bool pullEdge=(dir==LIGHTING_249_HIGH_BEAM_PULL&&s.prevPull!=LIGHTING_249_HIGH_BEAM_PULL);
            const int8_t act=lightingHsGestureFeedPure(s.hsGesture,ms,pullEdge,s.hs2.armed,s.hs2.armedAtMs);
            if(act==LIGHTING_HS_ACTION_ARM){
              lightingHs2ArmAtPure(s.hs2,ms);
              s.strobeReleaseNeeded=false;
            }else if(act==LIGHTING_HS_ACTION_DISARM){
              if(s.hs2.pull)s.strobeReleaseNeeded=true;
              lightingHs2DisarmPure(s.hs2);
            }
          }
          s.prevPull=dir;
        }
      }
    }
  }
  portEXIT_CRITICAL(&lightingSettingsMux);
}
static void lightingService(){
  const uint32_t now=millis();
  // 最高优先级保护：当 TLSSC 免重启窗口需要抢占发送 0x331 时，灯光任务立即让出总线屏障锁。
  if(P4_TLSSC_NR_PRIORITY_YIELD(now))return;
  if(!canTxBarrierMutex||xSemaphoreTake(canTxBarrierMutex,0)!=pdTRUE)return;
  const uint32_t epoch=canTxBarrierState.epoch;
  const bool bodyTxAllowed=P4_LIGHTING_BODY_TX_SUPPORTED();
  const bool transport=bodyTxAllowed&&!canTxAdministrativeHold&&!canUsbPassive()&&p4CanAllRunning()&&epoch!=0u&&(P4_BODY!=p4CanBusB()||twaiNonSummonAdmissionOpen());
  const bool ap=lightingApSnapshot(now);
  portENTER_CRITICAL(&stateMux);
  const uint8_t gear=gear118Raw;const uint32_t gearMs=gear118Ms;
  portEXIT_CRITICAL(&stateMux);
  portENTER_CRITICAL(&nagCtxMux);
  const bool speedValid=nagCtx.vehicleSpeedValid&&lightingAgeOkPure(now,nagCtx.lastVehicleSpeedMs,500u);
  const int32_t speed=nagPartySpeedKphX100Pure(nagCtx.vehicleSpeedRaw);
  portEXIT_CRITICAL(&nagCtxMux);
  portENTER_CRITICAL(&blinkAMux);
  const bool blinkerBusy=activeTurn!=STALK_IDLE||blinkerTxRequestState.pendingDir!=0u;
  portEXIT_CRITICAL(&blinkAMux);
  CanFrame stalkOut={};stalkOut.bus=P4_BODY;bool sendStalk=false;
  enum StalkTxTarget:uint8_t{STALK_NONE=0,STALK_AP1,STALK_STROBE,STALK_STROBE_REL,STALK_HORN,STALK_HORN_REL} stalkTarget=STALK_NONE;
  CanFrame bodyOut={};bodyOut.bus=P4_BODY;bool sendBody=false;
  CanFrame fogOut={};fogOut.bus=P4_BODY;bool sendFog=false;
  uint8_t stalkCounter=0,bodyCounter=0;
  bool requested=false,wantRearFogOn=false,isBurstEdge=false,horn3e9Next=false,fogHoldOffStep=false;
  portENTER_CRITICAL(&lightingSettingsMux);
  auto &s=lightingState;
  if(s.epoch!=epoch){s=LightingRuntime{};s.epoch=epoch;}
  if(!transport){
    s.run={};s.train={};s.dynamic={};s.dynSent=false;s.horn3e9Sent=false;
    lightingHs2ResetPure(s.hs2);s.strobeReleaseNeeded=false;
    lightingHornFlashResetPure(s.horn);
    s.rearFogBurstEdges=0u;s.rearFogBurstLevel=false;s.rearFogAsserted=false;
    s.rearFogWeAsserted=false;s.rearFogRevHoldUntilMs=0u;s.rearFogUiZeroPending=false;
    s.rearFogHoldOffLeft=0u;s.rearFogTurnOffTries=0u;
  } else {
    lightingAp1RunStepPure(s.run,lightingPermission&&ap&&activeTurnSignalVariant==TURN_SIGNAL_STALK,false,true,now);
    const bool beam=lightingPermission&&ap&&s.run.running;
    const bool hornPullLevel=lightingHornFlashStepPure(s.horn,lightingHornFlash,now);
    const bool hornActiveNow=s.horn.active;
    const bool isStalkCar=(activeTurnSignalVariant==TURN_SIGNAL_STALK);
    const bool hornOwns249=isStalkCar&&(hornActiveNow||s.horn.releaseNeeded);
    LightingBrakeSignalPure a={},b={};
    if(s.brakeSeen[0]&&lightingAgeOkPure(now,s.brakeMs[0],300u))a=s.brake[0];
    if(s.brakeSeen[1]&&lightingAgeOkPure(now,s.brakeMs[1],300u))b=s.brake[1];
    const auto brake=lightingMergeBrakePure(a,b);
    const bool driveFresh=gearMs!=0u&&lightingAgeOkPure(now,gearMs,500u)&&speedValid;
    lightingDynHighStepPure(s.dynamic,driveFresh?lightingBrakeMode:0u,brake.brake,brake.hard,gear,driveFresh?speed:0);
    requested=s.dynamic.requested;
    bool fresh=false,lit=false,cancel=false;
    lightingBeamReportPure(now,s.beam6Ms,s.beam6,s.beam5Ms,s.beam5,fresh,lit,cancel,s.beamD4,s.beam5Ms!=0u);
    LightingAp1TopupEventPure event={};
    bool train=false;
    if(!beam)lightingAp1PushResetPure(s.train);
    else if(fresh||s.train.left!=0u)train=lightingAp1TopupDuePure(s.train,beam,fresh,lit,s.carPushMs!=0u&&lightingAgeOkPure(now,s.carPushMs,119u),s.carPushMs,now,LIGHTING_TOPUP_PROFILE_RETRY,&event);

    const bool stalkFresh=!blinkerBusy&&s.stalk.valid&&lightingAgeOkPure(now,s.stalk.ms,300u)&&(s.stalk.data[2]&15u)==0u;
    const bool stalkIdle=((s.stalk.data[1]>>4)&3u)==0u;
    const bool stalkReady=stalkFresh&&stalkIdle;
    const bool stalkReadyAp1=stalkFresh&&(stalkIdle||s.train.left>0u);
    const bool strobeActive=s.hs2.armed&&isStalkCar;
    const bool strobeOwns=strobeActive||s.strobeReleaseNeeded;
    const uint8_t owner249=lighting249OwnerPure(strobeOwns,hornOwns249,train);
    if((strobeActive||hornOwns249)&&train)s.strobeConflicts++;

    // A) 0x249 左拨杆通路（爆闪 / 喇叭闪灯 / AP 远光补推）
    if(owner249==LIGHTING_249_OWNER_STROBE&&stalkReady){
      if(!strobeActive&&s.strobeReleaseNeeded){
        if(uint32_t(now-s.lastStalkAttempt)>=10u){
          sendStalk=true;stalkTarget=STALK_STROBE_REL;stalkOut.can_id=0x249u;stalkOut.can_dlc=4;memcpy(stalkOut.data,s.stalk.data,4);
          stalkCounter=(s.stalk.counter+1u)&15u;stalkOut.data[1]=(stalkOut.data[1]&0xc0u)|stalkCounter;
          stalkOut.data[0]=leftStalkChecksumPure(stalkOut.data,stalkCounter);s.lastStalkAttempt=now;
        }
      }else if(strobeActive){
        const bool pullLevel=lightingHs2LevelPure(s.hs2,now);
        const bool flipped=(pullLevel!=s.lastStrobePullSent);
        if(flipped||uint32_t(now-s.lastStalkAttempt)>=20u){
          sendStalk=true;stalkTarget=STALK_STROBE;stalkOut.can_id=0x249u;stalkOut.can_dlc=4;memcpy(stalkOut.data,s.stalk.data,4);
          stalkCounter=(s.stalk.counter+1u)&15u;
          stalkOut.data[1]=(stalkOut.data[1]&0xc0u)|(pullLevel?LIGHTING_249_PULL_BIT:0x00u)|stalkCounter;
          stalkOut.data[0]=leftStalkChecksumPure(stalkOut.data,stalkCounter);
          s.lastStalkAttempt=now;s.lastStrobePullSent=pullLevel;
        }
      }
    }else if(owner249==LIGHTING_249_OWNER_ALBERT&&isStalkCar&&!blinkerBusy&&s.stalk.valid&&((s.stalk.data[1]>>4)&3u)==0u&&(s.stalk.data[2]&15u)==0u){
      if(!hornActiveNow&&s.horn.releaseNeeded&&lightingAgeOkPure(now,s.stalk.ms,800u)){
        if(uint32_t(now-s.lastStalkAttempt)>=10u){
          sendStalk=true;stalkTarget=STALK_HORN_REL;stalkOut.can_id=0x249u;stalkOut.can_dlc=4;memcpy(stalkOut.data,s.stalk.data,4);
          stalkCounter=(s.stalk.counter+1u)&15u;stalkOut.data[1]=(stalkOut.data[1]&0xc0u)|stalkCounter;
          stalkOut.data[0]=leftStalkChecksumPure(stalkOut.data,stalkCounter);s.lastStalkAttempt=now;
        }
      }else if(hornActiveNow&&lightingAgeOkPure(now,s.stalk.ms,300u)){
        const bool flipped=(hornPullLevel!=s.lastHornPullSent);
        if(flipped||uint32_t(now-s.lastStalkAttempt)>=20u){
          sendStalk=true;stalkTarget=STALK_HORN;stalkOut.can_id=0x249u;stalkOut.can_dlc=4;memcpy(stalkOut.data,s.stalk.data,4);
          stalkCounter=(s.stalk.counter+1u)&15u;
          stalkOut.data[1]=(stalkOut.data[1]&0xc0u)|(hornPullLevel?LIGHTING_249_PULL_BIT:0x00u)|stalkCounter;
          stalkOut.data[0]=leftStalkChecksumPure(stalkOut.data,stalkCounter);
          s.lastStalkAttempt=now;s.lastHornPullSent=hornPullLevel;
        }
      }
    }else if(owner249==LIGHTING_249_OWNER_AP1&&stalkReadyAp1&&uint32_t(now-s.lastStalkAttempt)>=10u){
      sendStalk=true;stalkTarget=STALK_AP1;stalkOut.can_id=0x249u;stalkOut.can_dlc=4;memcpy(stalkOut.data,s.stalk.data,4);
      stalkCounter=(s.stalk.counter+1u)&15u;stalkOut.data[1]=(stalkOut.data[1]&0xc0u)|0x20u|stalkCounter;
      stalkOut.data[0]=leftStalkChecksumPure(stalkOut.data,stalkCounter);s.lastStalkAttempt=now;
    }

    // B) 0x3E9 车身通路（独立周期发送，不被 0x249 / 0x273 饿死）
    if(s.body.valid&&lightingAgeOkPure(now,s.body.ms,3000u)&&uint32_t(now-s.lastBodyAttempt)>=20u&&
       (beam||requested||s.dynSent||(!isStalkCar&&(hornActiveNow||s.horn3e9Sent)))){
      sendBody=true;bodyOut.can_id=0x3e9u;bodyOut.can_dlc=8;memcpy(bodyOut.data,s.body.data,8);
      if(requested)lightingApplyDynamicBrakePure(bodyOut.data,true);
      if(!isStalkCar&&hornActiveNow){
        lightingApplyApForcedHighBeamPure(bodyOut.data,hornPullLevel,true);
        horn3e9Next=true;s.horn.releaseNeeded=false;
      }else{
        horn3e9Next=false;
        if(beam){
          lightingApplyApForcedHighBeamPure(bodyOut.data,true,true);
          if((bodyOut.data[0]&0x03u)==0u)bodyOut.data[0]=(uint8_t)(bodyOut.data[0]|0x01u);
        }
      }
      bodyCounter=(s.body.counter+1u)&15u;bodyOut.data[6]=(bodyOut.data[6]&15u)|(bodyCounter<<4);
      bodyOut.data[7]=teslaAdditiveChecksumPure(0x3e9u,bodyOut.data);s.lastBodyAttempt=now;
    }

    // C) 0x273 后雾灯通路（倒挡防抖 + 自动识别自锁切换 / 电平保持，彻底消除倒车闪烁）
    if(s.vcCtrl.valid){
      const bool revRaw=lightingReverseRearFog&&gearMs!=0u&&lightingAgeOkPure(now,gearMs,1000u)&&gear==TESLA_GEAR_R;
      if(revRaw){
        uint32_t hold=now+250u;
        s.rearFogRevHoldUntilMs=hold?hold:1u;
      }else if(s.rearFogRevHoldUntilMs!=0u&&(int32_t)(now-s.rearFogRevHoldUntilMs)>=0){
        s.rearFogRevHoldUntilMs=0u;
      }
      const bool reverseDemand=lightingReverseRearFog&&s.rearFogRevHoldUntilMs!=0u;
      const bool steadyDemand=reverseDemand||lightingPilotMode==3u;
      const bool fogFbFresh=lightingAgeOkPure(now,s.rearFogLampMs,1500u);
      bool send273=false;
      if(s.rearFogBurstEdges>0u){
        s.rearFogUiZeroPending=false;s.rearFogHoldOffLeft=0u;
        if(s.lastRearFogEdgeMs==0u||uint32_t(now-s.lastRearFogEdgeMs)>=LIGHTING_REAR_FOG_HALF_PERIOD_MS){
          wantRearFogOn=(s.rearFogBurstEdges%2u)==0u;
          send273=true;isBurstEdge=true;
        }else if(uint32_t(now-s.lastRearFogAttempt)>=40u){
          wantRearFogOn=s.rearFogBurstLevel;
          send273=true;
        }
      }else if(steadyDemand){
        s.rearFogHoldOffLeft=0u;
        if(s.rearFogLevelHold){
          wantRearFogOn=true;
          if(s.rearFogUiZeroPending||!s.rearFogAsserted||uint32_t(now-s.lastRearFogAttempt)>=10u){
            send273=true;s.rearFogUiZeroPending=false;
          }
        }else if(fogFbFresh&&s.rearFogLampOn){
          // 实车 0x3E3 已确认为常亮：切换型车型不再重复发 0x273，避免和原厂 0 碰撞翻转熄灭
          s.rearFogTurnOffTries=0u;s.rearFogUiZeroPending=false;
        }else{
          wantRearFogOn=true;s.rearFogUiZeroPending=false;
          if(s.lastRearFogAttempt==0u||uint32_t(now-s.lastRearFogAttempt)>=450u){
            send273=true;
          }
        }
      }else if(lightingPilotMode==2u){
        s.rearFogUiZeroPending=false;s.rearFogHoldOffLeft=0u;s.rearFogWeAsserted=false;
        wantRearFogOn=(now%1000u)<450u;
        if(wantRearFogOn!=s.rearFogAsserted||uint32_t(now-s.lastRearFogAttempt)>=40u){
          send273=true;
        }
      }else if(s.rearFogWeAsserted||s.rearFogAsserted||s.rearFogHoldOffLeft>0u){
        s.rearFogUiZeroPending=false;
        if(s.rearFogLevelHold){
          if(s.rearFogHoldOffLeft==0u)s.rearFogHoldOffLeft=2u;
          wantRearFogOn=false;
          if(s.lastRearFogAttempt==0u||uint32_t(now-s.lastRearFogAttempt)>=20u){
            send273=true;fogHoldOffStep=true;
          }
        }else if(fogFbFresh&&!s.rearFogLampOn){
          s.rearFogWeAsserted=false;s.rearFogAsserted=false;s.rearFogTurnOffTries=0u;
        }else if(!s.rearFogWeAsserted){
          wantRearFogOn=false;send273=true;
        }else if(fogFbFresh&&s.rearFogLampOn&&s.rearFogTurnOffTries<3u){
          if(s.lastRearFogAttempt==0u||uint32_t(now-s.lastRearFogAttempt)>=450u){
            wantRearFogOn=true;send273=true;
          }
        }else{
          wantRearFogOn=false;send273=true;
          s.rearFogWeAsserted=false;s.rearFogTurnOffTries=0u;
        }
      }
      if(send273){
        sendFog=true;
        fogOut.can_id=LIGHTING_ID_VEHICLE_CONTROL;fogOut.can_dlc=s.vcCtrl.dlc>=3u?s.vcCtrl.dlc:8u;
        memcpy(fogOut.data,s.vcCtrl.data,8);
        fogOut.data[LIGHTING_273_D2]=wantRearFogOn
          ?(uint8_t)(fogOut.data[LIGHTING_273_D2]|LIGHTING_273_REAR_FOG_MASK)
          :(uint8_t)(fogOut.data[LIGHTING_273_D2]&~LIGHTING_273_REAR_FOG_MASK);
        s.lastRearFogAttempt=now;
      }
    }
  }
  portEXIT_CRITICAL(&lightingSettingsMux);
  if(sendBody){
    const esp_err_t result=p4CanSendFrame(&bodyOut);
    portENTER_CRITICAL(&lightingSettingsMux);
    if(result==ESP_OK){
      lightingTxOk++;
      s.body.counter=bodyCounter;s.body.sent=true;s.body.sentMs=now;memcpy(s.body.last,bodyOut.data,8);
      s.dynSent=requested;s.horn3e9Sent=horn3e9Next;
    }else lightingTxFail++;
    portEXIT_CRITICAL(&lightingSettingsMux);
  }
  if(sendStalk){
    const esp_err_t result=p4CanSendFrame(&stalkOut);
    portENTER_CRITICAL(&lightingSettingsMux);
    if(result==ESP_OK){
      lightingTxOk++;
      s.stalk.counter=stalkCounter;s.stalk.sent=true;s.stalk.sentMs=now;memcpy(s.stalk.last,stalkOut.data,stalkOut.can_dlc);
      if(stalkTarget==STALK_AP1)lightingAp1PushSentPure(s.train);
      else if(stalkTarget==STALK_STROBE_REL)s.strobeReleaseNeeded=false;
      else if(stalkTarget==STALK_HORN_REL)s.horn.releaseNeeded=false;
    }else lightingTxFail++;
    portEXIT_CRITICAL(&lightingSettingsMux);
  }
  if(sendFog){
    const esp_err_t result=p4CanSendFrame(&fogOut);
    portENTER_CRITICAL(&lightingSettingsMux);
    if(result==ESP_OK){
      lightingTxOk++;
      s.vcCtrl.sent=true;s.vcCtrl.sentMs=now;memcpy(s.vcCtrl.last,fogOut.data,8);
      s.rearFogAsserted=wantRearFogOn;
      if(isBurstEdge&&s.rearFogBurstEdges>0u){
        s.rearFogBurstEdges--;
        s.rearFogBurstLevel=wantRearFogOn;
        s.lastRearFogEdgeMs=now;
      }else if(fogHoldOffStep){
        if(s.rearFogHoldOffLeft>0u)s.rearFogHoldOffLeft--;
        if(s.rearFogHoldOffLeft==0u){s.rearFogWeAsserted=false;s.rearFogAsserted=false;}
      }else if(wantRearFogOn&&(lightingPilotMode==3u||s.rearFogRevHoldUntilMs!=0u)){
        s.rearFogWeAsserted=true;
      }else if(wantRearFogOn&&s.rearFogWeAsserted&&lightingPilotMode!=2u){
        s.rearFogTurnOffTries++;
        if(s.rearFogTurnOffTries>=3u){s.rearFogWeAsserted=false;s.rearFogAsserted=false;}
      }
    }else lightingTxFail++;
    portEXIT_CRITICAL(&lightingSettingsMux);
  }
  xSemaphoreGive(canTxBarrierMutex);
}
static void lightingTask(void*){for(;;){lightingService();vTaskDelay(pdMS_TO_TICKS(5));}}
