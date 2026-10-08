#include "editor/graph/flow_visual.h"

#include "support/math.h"

#include <math.h>

#define FLOW_ATTACK_HOLD 0.3
#define FLOW_ATTACK_DECAY 0.4
#define FLOW_SUSTAIN_HOLD 0.4
#define FLOW_RELEASE 1.5
/* Longer than a fixed step at the slowest display rate, so a wire firing
   every step reads as one stream. */
#define FLOW_PULSE_HOLD 0.1
#define FLOW_PULSE_FADE 0.4

double flow_visual_heat(double since)
{
    double attack = math_clamp01(1.0 - (since - FLOW_ATTACK_HOLD) / FLOW_ATTACK_DECAY);
    double sustain_start = FLOW_ATTACK_HOLD + FLOW_ATTACK_DECAY + FLOW_SUSTAIN_HOLD;
    double release = math_clamp01(1.0 - (since - sustain_start) / FLOW_RELEASE);
    return fmax(math_pow(attack, 1.8), math_pow(release, 1.4));
}

double flow_visual_pulse(double since)
{
    double t = math_clamp01((since - FLOW_PULSE_HOLD) / FLOW_PULSE_FADE);
    return 1.0 - t * t * (3.0 - 2.0 * t);
}
