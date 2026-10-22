/* SPDX-License-Identifier: GPL-2.0 */

#ifdef CONFIG_RV_MON_IRQSTATE
DEFINE_EVENT(event_da_monitor, event_irqstate,
	     TP_PROTO(char *state, char *event, char *next_state, bool final_state),
	     TP_ARGS(state, event, next_state, final_state));

DEFINE_EVENT(error_da_monitor, error_irqstate,
	     TP_PROTO(char *state, char *event),
	     TP_ARGS(state, event));

DEFINE_EVENT(error_env_da_monitor, error_env_irqstate,
	     TP_PROTO(char *state, char *event, char *env),
	     TP_ARGS(state, event, env));
#endif /* CONFIG_RV_MON_IRQSTATE */
