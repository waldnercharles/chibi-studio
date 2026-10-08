#ifndef CHIBI_FLOW_VISUAL_H
#define CHIBI_FLOW_VISUAL_H

/* Host-independent flow envelopes, sampled from the seconds since a wire or
 * node last fired; they never depend on frame count or evaluator callbacks. */

/* UE-style execution heat: a wire's glow, held and then released over about
 * 2.6 s. */
double flow_visual_heat(double since);
/* A pulse's size: whole while firing continues (a fire within the last
 * 0.1 s), then easing to zero over 0.4 s once it stops. */
double flow_visual_pulse(double since);

#endif
