#include "np_battery.h"

const char *np_battery_state_name(np_battery_state_t state)
{
    switch(state) {
        case NP_BATTERY_ABSENT: return "Not installed";
        case NP_BATTERY_IDLE: return "Not charging";
        case NP_BATTERY_CHARGING: return "Charging";
        case NP_BATTERY_DISCHARGING: return "On battery";
        case NP_BATTERY_FULL: return "Full (est.)";
        default: return "Unavailable";
    }
}

void np_battery_update(np_battery_t *b, const np_battery_sample_t *s, uint32_t now)
{
    /* Missing/failed reads must never look like zero current, a full pack,
     * removal, or low battery. Restart qualification after any data gap. */
    if(!s->valid || s->voltage_mv < 0 || s->voltage_mv > 9000) {
        b->status = (np_battery_status_t){.percent=-1,.state=NP_BATTERY_UNKNOWN};
        b->candidate_valid = b->full_pending = false;
        return;
    }
    b->status.available = true;
    b->status.charger_enabled = s->charger_enabled;
    b->status.voltage_mv = s->voltage_mv;
    b->status.current_ma = s->current_ma;
    bool present = s->voltage_mv >= 5000;
    if(!present) {
        b->status.present=false;
        b->status.low=false;
        if(b->status.state!=NP_BATTERY_ABSENT) b->status.state=NP_BATTERY_UNKNOWN;
    }
    /* NP-F550 is 2S. Use the M5Unified voltage-based estimate (per-cell
     * 3300 mV + 800 mV span), not a claim of measured remaining capacity. */
    int level = (s->voltage_mv - 6600) * 100 / 1600;
    if(level < 0) level = 0;
    if(level > 100) level = 100;
    np_battery_state_t next = !present ? NP_BATTERY_ABSENT :
        s->current_ma < -20 ? NP_BATTERY_CHARGING :
        s->current_ma > 20 ? NP_BATTERY_DISCHARGING : NP_BATTERY_IDLE;

    /* BAT_STAT describes the charge stage, not charge completion. Infer full
     * only after 60 s of near-full voltage and settled current with charging
     * enabled. A disabled charger or an idle half-full pack cannot be full. */
    bool near_full = next == NP_BATTERY_IDLE && s->charger_enabled &&
        s->voltage_mv >= (b->status.state == NP_BATTERY_FULL ? 8100 : 8200);
    if(near_full) {
        if(!b->full_pending) { b->full_pending=true; b->full_since=now; }
        if(b->status.state == NP_BATTERY_FULL || now-b->full_since >= 60000u)
            next = NP_BATTERY_FULL;
    }
    else b->full_pending=false;

    if(!b->candidate_valid || next != b->candidate) {
        b->candidate=next; b->candidate_since=now; b->candidate_valid=true;
    }
    /* Three seconds avoids cable/contact and current-sign chatter. Keep the
     * last qualified state until then; sensor errors above remain immediate. */
    if(now-b->candidate_since >= 3000u) {
        b->status.state=next;
        b->status.present=present;
    }
    b->status.percent=b->status.present && present ? level : -1;
    if(!b->status.present || b->status.state != NP_BATTERY_DISCHARGING)
        b->status.low=false;
    else if(next==NP_BATTERY_DISCHARGING) {
        if(level <= 15) b->status.low=true;
        else if(level >= 25) b->status.low=false;
    }
}
