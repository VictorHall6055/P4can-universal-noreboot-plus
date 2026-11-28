#pragma once
static void httpLightingStats(){
  const uint32_t now=millis();
  portENTER_CRITICAL(&lightingSettingsMux);
  const bool permission=lightingPermission,running=lightingState.run.running;
  const uint8_t brakeMode=lightingBrakeMode;
  const bool strobeGesture=lightingStrobeGesture,strobeActive=lightingState.hs2.armed;
  const uint8_t pilotMode=lightingPilotMode;
  const bool reverseRearFog=lightingReverseRearFog;
  const bool hornFlash=lightingHornFlash;
  const bool hornActive=lightingState.horn.active;
  const bool rearFogBurstActive=lightingState.rearFogBurstEdges>0u;
  const bool rearFogLampOn=lightingState.rearFogLampOn&&lightingState.rearFogLampMs!=0u&&(uint32_t)(now-lightingState.rearFogLampMs)<=2000u;
  const bool rearFogTemplateReady=lightingState.vcCtrl.valid;
  const bool highBeamTemplateReady=lightingState.stalk.valid&&(uint32_t)(now-lightingState.stalk.ms)<=1000u;
  const uint32_t ok=lightingTxOk,fail=lightingTxFail;
  portEXIT_CRITICAL(&lightingSettingsMux);
  String json;JsonWriterArduino jw(json);jw.boolean("ok",true);jw.boolean("apForcedHighBeam",permission);
  jw.u32("dynamicBrakeMode",brakeMode);jw.boolean("running",running);jw.boolean("apActive",lightingApSnapshot(now));
  jw.boolean("stalkSupported",activeTurnSignalVariant==TURN_SIGNAL_STALK);
  jw.boolean("bodyTxSupported",P4_LIGHTING_BODY_TX_SUPPORTED());
  jw.boolean("strobeGesture",strobeGesture);
  jw.boolean("strobeActive",strobeActive);
  jw.u32("pilotMode",pilotMode);
  jw.boolean("reverseRearFog",reverseRearFog);
  jw.boolean("hornFlash",hornFlash);
  jw.boolean("hornActive",hornActive);
  jw.boolean("rearFogBurstActive",rearFogBurstActive);
  jw.boolean("rearFogLampOn",rearFogLampOn);
  jw.boolean("rearFogTemplateReady",rearFogTemplateReady);
  jw.boolean("highBeamTemplateReady",highBeamTemplateReady);
  jw.u32("txOk",ok);jw.u32("txFail",fail);
  jw.finish();server.send(200,"application/json",json);
}
static void httpLightingUpdate(){
  portENTER_CRITICAL(&lightingSettingsMux);
  bool permission=lightingPermission;
  uint8_t mode=lightingBrakeMode;
  bool strobeGesture=lightingStrobeGesture;
  uint8_t pilotMode=lightingPilotMode;
  bool reverseRearFog=lightingReverseRearFog;
  bool hornFlash=lightingHornFlash;
  portEXIT_CRITICAL(&lightingSettingsMux);
  const bool hasAp=server.hasArg("apForcedHighBeam");
  const bool hasBrake=server.hasArg("dynamicBrakeMode");
  const bool hasStrobeGesture=server.hasArg("strobeGesture");
  const bool hasStrobeActive=server.hasArg("strobeActive");
  const bool hasPilotMode=server.hasArg("pilotMode");
  const bool hasRevFog=server.hasArg("reverseRearFog");
  const bool hasHornFlash=server.hasArg("hornFlash");
  const bool hasBurst=server.hasArg("rearFogBurst");
  if(!hasAp&&!hasBrake&&!hasStrobeGesture&&!hasStrobeActive&&!hasPilotMode&&!hasRevFog&&!hasHornFlash&&!hasBurst){
    server.send(400,"application/json","{\"error\":\"no-setting\"}");return;
  }
  if(hasAp&&!httpBoolArg("apForcedHighBeam",permission)){server.send(400,"application/json","{\"error\":\"invalid-permission\"}");return;}
  if(hasBrake){
    const String value=server.arg("dynamicBrakeMode");
    if(value!="0"&&value!="1"&&value!="2"){server.send(400,"application/json","{\"error\":\"invalid-brake-mode\"}");return;}
    mode=(uint8_t)value.toInt();
  }
  if(hasStrobeGesture&&!httpBoolArg("strobeGesture",strobeGesture)){server.send(400,"application/json","{\"error\":\"invalid-strobe-gesture\"}");return;}
  bool strobeActiveReq=false;
  if(hasStrobeActive&&!httpBoolArg("strobeActive",strobeActiveReq)){server.send(400,"application/json","{\"error\":\"invalid-strobe-active\"}");return;}
  if(hasPilotMode){
    const String value=server.arg("pilotMode");
    if(value!="0"&&value!="1"&&value!="2"&&value!="3"){server.send(400,"application/json","{\"error\":\"invalid-pilot-mode\"}");return;}
    pilotMode=(uint8_t)value.toInt();
  }
  if(hasRevFog&&!httpBoolArg("reverseRearFog",reverseRearFog)){server.send(400,"application/json","{\"error\":\"invalid-reverse-rear-fog\"}");return;}
  if(hasHornFlash&&!httpBoolArg("hornFlash",hornFlash)){server.send(400,"application/json","{\"error\":\"invalid-horn-flash\"}");return;}
  bool burstReq=false;
  if(hasBurst&&!httpBoolArg("rearFogBurst",burstReq)){server.send(400,"application/json","{\"error\":\"invalid-rear-fog-burst\"}");return;}
  if((hasAp&&permission||hasStrobeGesture&&strobeGesture||hasStrobeActive&&strobeActiveReq)&&activeTurnSignalVariant!=TURN_SIGNAL_STALK){
    server.send(409,"application/json","{\"error\":\"stalk-required\"}");return;
  }
  if((hasStrobeGesture||hasStrobeActive||hasPilotMode||hasRevFog||hasHornFlash||hasBurst)&&!P4_LIGHTING_BODY_TX_SUPPORTED()){
    server.send(409,"application/json","{\"error\":\"body-tx-unsupported\"}");return;
  }
  if(hasAp||hasBrake||hasStrobeGesture||hasPilotMode||hasRevFog||hasHornFlash){
    if(!lightingSettingsSaveAll(permission,mode,strobeGesture,pilotMode,reverseRearFog,hornFlash)){server.send(503,"application/json","{\"error\":\"save-failed-or-busy\"}");return;}
  }
  if(hasStrobeActive&&!lightingSetStrobeActive(strobeActiveReq)){
    server.send(409,"application/json","{\"error\":\"stalk-required\"}");return;
  }
  if(hasBurst&&burstReq&&!lightingTriggerRearFogBurst()){
    server.send(409,"application/json","{\"error\":\"body-tx-unsupported\"}");return;
  }
  httpLightingStats();
}
