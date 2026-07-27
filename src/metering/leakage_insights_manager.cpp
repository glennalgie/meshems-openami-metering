//
// S5 — Leakage Insights Manager implementation.
//
#include <metering/leakage_insights_manager.h>

const char* leakStateName(LeakState s) {
    switch (s) {
        case LK_NORMAL:    return "normal";
        case LK_WATCH:     return "watch";
        case LK_ISOLATING: return "isolating";
        case LK_FAULT:     return "fault";
        case LK_RESTORED:  return "restored";
        default:           return "?";
    }
}

// ---- self ---------------------------------------------------------------
void LeakageInsightsManager::updateSelf(float ac_mA, float dc_mA, bool exporting, uint32_t now) {
    float dt   = (_last_ms && now > _last_ms) ? (now - _last_ms) / 1000.0f : 0.0f;
    float ramp = (dt > 0.0f) ? (ac_mA - _last_ac) / dt : 0.0f;

    strncpy(_self.ems_id, ems_id, sizeof(_self.ems_id) - 1);
    _self.phase        = phase;
    _self.ac_mA        = ac_mA;
    _self.dc_mA        = dc_mA;
    _self.ramp_ac_mA_s = ramp;
    _self.exporting    = exporting;
    _self.ac_thresh_mA = effTrip();
    _self.valid        = true;
    _self.pushSig(ramp);

    const float watch = watchLevel();
    const float trip  = effTrip();
    LeakState prev = _state;

    if (ac_mA >= trip)                 _state = LK_FAULT;
    else if (ac_mA >= watch)           _state = (_state == LK_ISOLATING ? LK_ISOLATING : LK_WATCH);
    else if (ac_mA < watch * 0.8f) {   // hysteresis on the way down
        _state = (prev == LK_FAULT || prev == LK_WATCH || prev == LK_ISOLATING) ? LK_RESTORED : LK_NORMAL;
        if (_state == LK_RESTORED) { /* next quiet cycle drops to normal */ }
    } else {
        _state = prev;                 // in the hysteresis band: hold
    }
    if (prev == LK_RESTORED && ac_mA < watch * 0.8f) _state = LK_NORMAL;

    // onset = first time we left normal; cleared when back to normal
    if (prev == LK_NORMAL && _state != LK_NORMAL) _onset_ts = now;
    if (_state == LK_NORMAL) _onset_ts = 0;
    _self.onset_ts = _onset_ts;

    _last_ac = ac_mA;
    _last_ms = now;
}

void LeakageInsightsManager::selfTelemetry(JsonObject o) const {
    o["ems_id"]          = _self.ems_id;
    o["feeder_id"]       = feeder_id;
    o["node"]            = node;
    o["phase"]           = _self.phase;
    o["state"]           = leakStateName(_state);
    o["ac_mA"]           = _self.ac_mA;
    o["dc_mA"]           = _self.dc_mA;
    o["ac_threshold_mA"] = _self.ac_thresh_mA;
    o["dc_threshold_mA"] = dc_trip;
    o["ramp_ac_mA_s"]    = _self.ramp_ac_mA_s;
    o["onset_ts"]        = _self.onset_ts;
    o["in_fault"]        = (_state == LK_FAULT);
    JsonObject pf = o["powerflow"].to<JsonObject>();
    pf["direction"] = _self.exporting ? "export" : "import";
    o["phase_angle_deg"] = nullptr;   // MD0630 is absolute-value only (reserved)
    o["sensor_ok"]       = true;
}

void LeakageInsightsManager::addPeer(const LeakageNode& p) {
    if (_nPeers < (int)(sizeof(_peers) / sizeof(_peers[0]))) _peers[_nPeers++] = p;
}

// ---- correlation --------------------------------------------------------
float LeakageInsightsManager::pearson(const float* a, int an, const float* b, int bn) {
    int n = an < bn ? an : bn;
    if (n < 3) return 0.0f;
    float ma = 0, mb = 0;
    for (int i = 0; i < n; i++) { ma += a[i]; mb += b[i]; }
    ma /= n; mb /= n;
    float num = 0, da = 0, db = 0;
    for (int i = 0; i < n; i++) {
        float xa = a[i] - ma, xb = b[i] - mb;
        num += xa * xb; da += xa * xa; db += xb * xb;
    }
    if (da <= 1e-9f || db <= 1e-9f) return 0.0f;
    return num / sqrtf(da * db);
}

// ---- nomination round ---------------------------------------------------
LeakageRound LeakageInsightsManager::nominate(uint32_t now) {
    LeakageRound r;
    if (_state == LK_NORMAL) return r;           // nothing to nominate

    // candidate list: self + peers on the same phase with a correlated onset
    const LeakageNode* cand[7]; int nc = 0;
    cand[nc++] = &_self;
    for (int i = 0; i < _nPeers && nc < 7; i++) {
        const LeakageNode& p = _peers[i];
        if (!p.valid || p.phase != _self.phase) continue;
        uint32_t d = (p.onset_ts > _self.onset_ts) ? (p.onset_ts - _self.onset_ts)
                                                    : (_self.onset_ts - p.onset_ts);
        if (_self.onset_ts && p.onset_ts && d > onset_window_ms) continue;   // uncorrelated
        cand[nc++] = &p;
    }

    // rank by ac_mA descending (selection sort, nc <= 7)
    int order[7]; for (int i = 0; i < nc; i++) order[i] = i;
    for (int i = 0; i < nc; i++)
        for (int j = i + 1; j < nc; j++)
            if (cand[order[j]]->ac_mA > cand[order[i]]->ac_mA) { int t = order[i]; order[i] = order[j]; order[j] = t; }

    // peer-correlation "odd one out": min mean-correlation with the others
    int   minCorrIdx = order[0];
    float minCorr    = 1e9f;
    bool  anyExport  = false;
    for (int i = 0; i < nc; i++) {
        anyExport |= cand[i]->exporting;
        if (nc < 3) break;                        // need >=3 nodes for a meaningful correlation
        float sum = 0; int cnt = 0;
        for (int j = 0; j < nc; j++) {
            if (i == j) continue;
            sum += pearson(cand[i]->sig, cand[i]->sigN, cand[j]->sig, cand[j]->sigN);
            cnt++;
        }
        float mc = cnt ? sum / cnt : 0.0f;
        if (mc < minCorr) { minCorr = mc; minCorrIdx = i; }
    }

    float top1 = cand[order[0]]->ac_mA;
    float top2 = nc > 1 ? cand[order[1]]->ac_mA : 0.0f;
    float conf = (top1 > 0.2f) ? (top1 - top2) / top1 : 0.0f;
    bool  agree = (order[0] == minCorrIdx);
    if (nc >= 3) conf = agree ? fminf(1.0f, conf * 1.2f + 0.1f) : conf * 0.6f;
    if (anyExport) conf *= 0.75f;                 // reverse-flow de-rate
    conf = fmaxf(0.0f, fminf(1.0f, conf));

    r.ran          = true;
    r.reporting    = nc;
    r.top1_mA      = top1;
    r.top2_mA      = top2;
    r.magCorrAgree = agree;
    r.confidence   = conf;
    strncpy(r.nominated, cand[order[0]]->ems_id, sizeof(r.nominated) - 1);

    if (conf >= conf_min && nc >= 2) {
        r.isolate = true;  strncpy(r.reason, agree ? "mag+corr agree" : "confident", sizeof(r.reason)-1);
    } else {
        r.isolate = false;
        strncpy(r.reason, (nc < 2) ? "no quorum" : "low confidence (fault between cabinets?)", sizeof(r.reason)-1);
    }
    return r;
}

void LeakageInsightsManager::roundJson(const LeakageRound& r, JsonObject o) const {
    o["feeder_id"] = feeder_id;
    o["phase"]     = _self.phase;
    o["nominated_ems"] = r.nominated;
    o["rule"]      = "max_ac_mA + min_peer_corr, correlated_onset";
    o["confidence"]= r.confidence;
    o["action"]    = r.isolate ? "isolate_takeoff (bracket upstream+downstream)" : "alert_only";
    o["reason"]    = r.reason;
    JsonObject q = o["quorum"].to<JsonObject>();
    q["reporting"] = r.reporting;
}

void LeakageInsightsManager::alertJson(const LeakageRound& r, JsonObject o, uint32_t now) const {
    bool crit = (_state == LK_FAULT);
    o["severity"]  = crit ? "critical" : (r.isolate ? "warning" : "info");
    o["feeder_id"] = feeder_id;
    JsonObject at = o["located_at"].to<JsonObject>();
    at["ems_id"] = r.nominated; at["phase"] = _self.phase;
    o["ac_mA"] = _self.ac_mA;   o["dc_mA"] = _self.dc_mA;
    o["headroom_to_trip_mA"] = fmaxf(0.0f, effTrip() - _self.ac_mA);
    o["classification"] = "leakage";
    o["confidence"] = r.confidence;
    o["recommended_action"] = r.isolate ? "Isolate the nominated takeoff (bracket up/downstream)"
                                        : "Inspect: candidates too close / low confidence — do not auto-isolate";
}
